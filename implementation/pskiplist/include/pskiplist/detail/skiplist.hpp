#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include "pskiplist/detail/concepts.hpp"
#include "pskiplist/detail/durable_node.hpp"
#include "pskiplist/detail/epoch_token.hpp"
#include "pskiplist/detail/marked_offset.hpp"
#include "pskiplist/detail/mmap_file.hpp"
#include "pskiplist/detail/packed_value.hpp"

namespace pskiplist {

template <typename Key, typename Compare = std::less<Key>>
  requires SkipListKey<Key> && SkipListCompare<Compare, Key>
class PSkipList {
 public:
  // `capacity_bytes` is fixed for the lifetime of the mapping (2.6節) — sized generously
  // up front, since a sparse file only consumes disk for pages actually written. The data
  // file at `path` is mutated in place via MAP_SHARED; it is not rewritten on checkpoint.
  explicit PSkipList(const std::filesystem::path &path, size_t capacity_bytes)
      : file_(path, capacity_bytes),
        nodes_(static_cast<DurableNode<Key> *>(file_.data())),
        capacity_slots_(file_.size() / sizeof(DurableNode<Key>)) {
    if (capacity_slots_ < 2) {
      throw std::invalid_argument("pskiplist: capacity_bytes too small to hold head/tail sentinels");
    }
    ::new (&nodes_[kHead]) DurableNode<Key>();
    ::new (&nodes_[kTail]) DurableNode<Key>();
    nodes_[kHead].forward0.store(pack_forward(kTail, false), std::memory_order_relaxed);
  }

  PSkipList(const PSkipList &) = delete;
  auto operator=(const PSkipList &) -> PSkipList & = delete;

  [[nodiscard]] auto get(const Key &key) const -> std::optional<uint64_t> {
    const EpochToken token(epoch_slots_, epoch_, EpochRole::kReader);
    Offset predecessor = kNullOffset;
    const Offset current = find_at_or_after(key, &predecessor);
    if (current == kNullOffset || current == kTail || !keys_equal(nodes_[current].key, key)) {
      return std::nullopt;
    }
    const PackedValue v(nodes_[current].value.load(std::memory_order_acquire));
    if (v.state() != NodeState::kLive) return std::nullopt;
    return v.payload();
  }

  [[nodiscard]] auto put(const Key &key, uint64_t payload) -> bool {
    const EpochToken token(epoch_slots_, epoch_, EpochRole::kWriter);
    Offset fresh = kNullOffset;
    for (;;) {
      Offset predecessor = kNullOffset;
      const Offset existing = find_at_or_after(key, &predecessor);
      if (existing != kNullOffset && existing != kTail && keys_equal(nodes_[existing].key, key)) {
        uint64_t expected = nodes_[existing].value.load(std::memory_order_acquire);
        if (PackedValue(expected).state() == NodeState::kTombstonedUnlinked) {
          std::this_thread::yield();
          continue;
        }
        const uint64_t desired = PackedValue::live(payload).raw();
        if (nodes_[existing].value.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
          return true;
        }
        continue;
      }

      if (fresh == kNullOffset) {
        fresh = allocate();
        if (fresh == kNullOffset) return false;
        nodes_[fresh].key = key;
        nodes_[fresh].value.store(PackedValue::live(payload).raw(), std::memory_order_relaxed);
      }
      nodes_[fresh].forward0.store(pack_forward(existing, false), std::memory_order_relaxed);

      uint64_t expected_next = pack_forward(existing, false);
      const uint64_t desired_next = pack_forward(fresh, false);
      if (nodes_[predecessor].forward0.compare_exchange_strong(expected_next, desired_next, std::memory_order_acq_rel)) {
        return true;
      }
    }
  }

  [[nodiscard]] auto remove(const Key &key) -> bool {
    const EpochToken token(epoch_slots_, epoch_, EpochRole::kWriter);
    Offset predecessor = kNullOffset;
    const Offset current = find_at_or_after(key, &predecessor);
    if (current == kNullOffset || current == kTail || !keys_equal(nodes_[current].key, key)) return false;

    for (;;) {
      uint64_t expected = nodes_[current].value.load(std::memory_order_acquire);
      const PackedValue v(expected);
      if (v.state() != NodeState::kLive) return false;
      const uint64_t desired = v.with_state(NodeState::kTombstonedLinked).raw();
      if (nodes_[current].value.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
        break;
      }
    }

    physically_unlink_best_effort(key, current);
    return true;
  }

  template <typename Callback>
  void scan(const Key &begin, const Key &end, Callback &&callback) const {
    const EpochToken token(epoch_slots_, epoch_, EpochRole::kReader);
    Offset predecessor = kNullOffset;
    Offset current = find_at_or_after(begin, &predecessor);
    while (current != kNullOffset && current != kTail) {
      const auto &node = nodes_[current];
      if (!less_(node.key, end)) break;
      const PackedValue v(node.value.load(std::memory_order_acquire));
      if (v.state() == NodeState::kLive) {
        callback(node.key, v.payload());
      }
      current = forward_offset(node.forward0.load(std::memory_order_acquire));
    }
  }

  void reclaim() {
    const std::lock_guard<std::mutex> epoch_lock(epoch_mutex_);

    std::vector<Offset> snapshot;
    {
      const std::lock_guard<std::mutex> pending_lock(pending_mutex_);
      snapshot.swap(pending_unlinks_);
    }

    // Physical-reclaim safety needs every possible holder of a stale offset — reader or
    // writer — to have drained, unlike checkpoint()'s writer-only wait (3章).
    const uint64_t old_parity = epoch_.load(std::memory_order_acquire) & 1;
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    auto &old_slot = epoch_slots_[old_parity];
    while (old_slot.readers.load(std::memory_order_acquire) != 0 ||
           old_slot.writers.load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }

    const std::lock_guard<std::mutex> free_lock(free_mutex_);
    free_list_.insert(free_list_.end(), snapshot.begin(), snapshot.end());
  }

 private:
  static constexpr Offset kHead = 0;
  static constexpr Offset kTail = 1;

  [[nodiscard]] auto keys_equal(const Key &a, const Key &b) const -> bool {
    return !less_(a, b) && !less_(b, a);
  }

  // Traverses to the first node with key >= `key`, helping to physically unlink any
  // marked (logically deleted) node it encounters along the way (4.2節). A helping CAS
  // that wins pushes the unlinked node to the pending-unlink queue; on any CAS outcome
  // touching a marked node the whole search restarts from head, since a stale predecessor
  // can no longer be trusted.
  [[nodiscard]] auto find_at_or_after(const Key &key, Offset *predecessor) const -> Offset {
    for (;;) {
      Offset pred = kHead;
      Offset current = forward_offset(nodes_[pred].forward0.load(std::memory_order_acquire));
      for (;;) {
        if (current == kTail) {
          *predecessor = pred;
          return current;
        }
        const uint64_t current_raw = nodes_[current].forward0.load(std::memory_order_acquire);
        if (forward_marked(current_raw)) {
          const Offset successor = forward_offset(current_raw);
          uint64_t expected = pack_forward(current, false);
          const uint64_t desired = pack_forward(successor, false);
          if (nodes_[pred].forward0.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
            enqueue_pending_unlink(current);
          }
          break;  // restart the whole search from head
        }
        if (!less_(nodes_[current].key, key)) {
          *predecessor = pred;
          return current;
        }
        pred = current;
        current = forward_offset(current_raw);
      }
    }
  }

  void enqueue_pending_unlink(Offset offset) const {
    const std::lock_guard<std::mutex> pending_lock(pending_mutex_);
    pending_unlinks_.push_back(offset);
  }

  auto allocate() -> Offset {
    {
      const std::lock_guard<std::mutex> free_lock(free_mutex_);
      if (!free_list_.empty()) {
        const Offset reused = free_list_.back();
        free_list_.pop_back();
        return reused;
      }
    }
    const Offset next = high_water_mark_.fetch_add(1, std::memory_order_relaxed);
    if (next >= capacity_slots_) {
      high_water_mark_.fetch_sub(1, std::memory_order_relaxed);
      return kNullOffset;
    }
    return next;
  }

  void physically_unlink_best_effort(const Key &key, Offset node_offset) {
    uint64_t expected = nodes_[node_offset].value.load(std::memory_order_acquire);
    const PackedValue v(expected);
    if (v.state() != NodeState::kTombstonedLinked) return;
    const uint64_t desired = v.with_state(NodeState::kTombstonedUnlinked).raw();
    if (!nodes_[node_offset].value.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
      return;
    }

    uint64_t forward_raw = nodes_[node_offset].forward0.load(std::memory_order_acquire);
    while (!forward_marked(forward_raw)) {
      const uint64_t marked = pack_forward(forward_offset(forward_raw), true);
      if (nodes_[node_offset].forward0.compare_exchange_strong(forward_raw, marked, std::memory_order_acq_rel)) {
        break;
      }
    }

    Offset predecessor = kNullOffset;
    static_cast<void>(find_at_or_after(key, &predecessor));
  }

  MmapFile file_;
  DurableNode<Key> *nodes_;
  size_t capacity_slots_;
  std::atomic<Offset> high_water_mark_{2};
  Compare less_{};

  mutable std::array<EpochSlot, 2> epoch_slots_{};
  mutable std::atomic<uint64_t> epoch_{0};
  std::mutex epoch_mutex_;
  mutable std::mutex pending_mutex_;
  mutable std::vector<Offset> pending_unlinks_;
  std::mutex free_mutex_;
  std::vector<Offset> free_list_;
};

}  // namespace pskiplist
