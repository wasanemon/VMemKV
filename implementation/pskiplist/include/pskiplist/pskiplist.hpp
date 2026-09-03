#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace pskiplist {

using Offset = uint64_t;
inline constexpr Offset kNullOffset = std::numeric_limits<Offset>::max();

enum class NodeState : uint8_t {
  kLive = 0,
  kTombstonedLinked = 1,
  kTombstonedUnlinked = 2,
};

class PackedValue {
 public:
  PackedValue() = default;
  explicit PackedValue(uint64_t bits) : bits_(bits) {}
  static auto live(uint64_t payload) -> PackedValue { return PackedValue(encode(NodeState::kLive, payload)); }

  [[nodiscard]] auto state() const -> NodeState { return static_cast<NodeState>(bits_ >> kPayloadBits); }
  [[nodiscard]] auto payload() const -> uint64_t { return bits_ & kPayloadMask; }
  [[nodiscard]] auto raw() const -> uint64_t { return bits_; }
  [[nodiscard]] auto with_state(NodeState state) const -> PackedValue { return PackedValue(encode(state, payload())); }

  static constexpr int kPayloadBits = 62;
  static constexpr uint64_t kPayloadMask = (uint64_t{1} << kPayloadBits) - 1;

 private:
  static auto encode(NodeState state, uint64_t payload) -> uint64_t {
    return (static_cast<uint64_t>(state) << kPayloadBits) | (payload & kPayloadMask);
  }
  uint64_t bits_ = 0;
};

template <typename Key>
struct DurableNode {
  std::atomic<uint64_t> epoch{0};
  Key key{};
  std::atomic<uint64_t> value{0};
  std::atomic<Offset> forward0{kNullOffset};
};

template <typename Key, typename Compare = std::less<Key>>
class PSkipList {
 public:
  explicit PSkipList(size_t capacity_nodes) : nodes_(capacity_nodes + 2) {
    nodes_[kHead].forward0.store(kTail, std::memory_order_relaxed);
  }

  PSkipList(const PSkipList &) = delete;
  auto operator=(const PSkipList &) -> PSkipList & = delete;

  [[nodiscard]] auto get(const Key &key) const -> std::optional<uint64_t> {
    const EpochToken token(*this);
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
    const EpochToken token(*this);
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
      nodes_[fresh].forward0.store(existing, std::memory_order_relaxed);

      Offset expected_next = existing;
      if (nodes_[predecessor].forward0.compare_exchange_strong(expected_next, fresh, std::memory_order_acq_rel)) {
        return true;
      }
    }
  }

  [[nodiscard]] auto remove(const Key &key) -> bool {
    const EpochToken token(*this);
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
    const EpochToken token(*this);
    Offset predecessor = kNullOffset;
    Offset current = find_at_or_after(begin, &predecessor);
    while (current != kNullOffset && current != kTail) {
      const auto &node = nodes_[current];
      if (!less_(node.key, end)) break;
      const PackedValue v(node.value.load(std::memory_order_acquire));
      if (v.state() == NodeState::kLive) {
        callback(node.key, v.payload());
      }
      current = node.forward0.load(std::memory_order_acquire);
    }
  }

  void reclaim() {
    const std::lock_guard<std::mutex> reclaim_lock(reclaim_mutex_);

    std::vector<Offset> snapshot;
    {
      const std::lock_guard<std::mutex> pending_lock(pending_mutex_);
      snapshot.swap(pending_unlinks_);
    }

    const uint64_t old_parity = epoch_parity_.load(std::memory_order_acquire) & 1;
    epoch_parity_.fetch_add(1, std::memory_order_acq_rel);
    while (active_[old_parity].load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }

    const std::lock_guard<std::mutex> free_lock(free_mutex_);
    free_list_.insert(free_list_.end(), snapshot.begin(), snapshot.end());
  }

 private:
  static constexpr Offset kHead = 0;
  static constexpr Offset kTail = 1;

  class EpochToken {
   public:
    explicit EpochToken(const PSkipList &owner) : owner_(owner) {
      parity_ = owner_.epoch_parity_.load(std::memory_order_acquire) & 1;
      owner_.active_[parity_].fetch_add(1, std::memory_order_acq_rel);
    }
    ~EpochToken() { owner_.active_[parity_].fetch_sub(1, std::memory_order_acq_rel); }
    EpochToken(const EpochToken &) = delete;
    auto operator=(const EpochToken &) -> EpochToken & = delete;

   private:
    const PSkipList &owner_;
    uint64_t parity_;
  };

  [[nodiscard]] auto keys_equal(const Key &a, const Key &b) const -> bool {
    return !less_(a, b) && !less_(b, a);
  }

  [[nodiscard]] auto find_at_or_after(const Key &key, Offset *predecessor) const -> Offset {
    Offset pred = kHead;
    Offset current = nodes_[pred].forward0.load(std::memory_order_acquire);
    while (current != kTail && current != kNullOffset && less_(nodes_[current].key, key)) {
      pred = current;
      current = nodes_[current].forward0.load(std::memory_order_acquire);
    }
    *predecessor = pred;
    return current;
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
    if (next >= nodes_.size()) {
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

    {
      const std::lock_guard<std::mutex> unlink_lock(unlink_mutex_);
      for (;;) {
        Offset predecessor = kNullOffset;
        const Offset found = find_at_or_after(key, &predecessor);
        if (found != node_offset) return;
        Offset expected_next = node_offset;
        const Offset successor = nodes_[node_offset].forward0.load(std::memory_order_acquire);
        if (nodes_[predecessor].forward0.compare_exchange_strong(expected_next, successor, std::memory_order_acq_rel)) {
          break;
        }
      }
    }

    const std::lock_guard<std::mutex> pending_lock(pending_mutex_);
    pending_unlinks_.push_back(node_offset);
  }

  std::vector<DurableNode<Key>> nodes_;
  std::atomic<Offset> high_water_mark_{2};
  Compare less_{};

  mutable std::array<std::atomic<int64_t>, 2> active_{};
  mutable std::atomic<uint64_t> epoch_parity_{0};
  std::mutex reclaim_mutex_;
  std::mutex pending_mutex_;
  std::mutex unlink_mutex_;
  std::vector<Offset> pending_unlinks_;
  std::mutex free_mutex_;
  std::vector<Offset> free_list_;
};

}  // namespace pskiplist
