// pskiplist: an offset-based, crash-consistent, lock-free skip list.
// See ../../high_level_design.md for the full design.
//
// Scope of this file (first implementable increment):
//  - Level 0 only (high_level_design.md 2.2/2.4): correctness does not depend on upper
//    levels, so they are deferred. Search is currently O(N), not O(log N).
//  - Node storage stands in for the eventual offset-based mmap'd file (2.1 章): a
//    pre-reserved, never-reallocated in-memory array indexed by Offset. Real mmap/offset
//    persistence, the manifest, and recovery (3 章) are not implemented yet.
//  - The 3-state node CAS arbitration for put/remove/physical-unlink (4.1 章) is
//    implemented and is the part most worth stress-testing at this stage.
//  - Physical reclaim/offset reuse (2.3 章) and the epoch/EBR system (3 章) are not
//    implemented yet: unlinked node offsets currently leak (never returned to a free
//    list). This is safe (no use-after-free) precisely because nothing is ever reused.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <vector>

namespace pskiplist {

using Offset = uint64_t;
inline constexpr Offset kNullOffset = std::numeric_limits<Offset>::max();

enum class NodeState : uint8_t {
  kLive = 0,
  kTombstonedLinked = 1,
  kTombstonedUnlinked = 2,
};

// Packs the node's 3-state (2 bits) and its payload into a single 64-bit word, so every
// state transition (high_level_design.md 4.1) is one atomic CAS on one word. Leaves 62
// usable payload bits (the design's "Value must fit in a single atomically-writable word"
// constraint, 2.1 章, minus the 2 bits spent on state here).
class PackedValue {
 public:
  PackedValue() = default;
  explicit PackedValue(uint64_t bits) : bits_(bits) {}
  static auto live(uint64_t payload) -> PackedValue { return PackedValue(encode(NodeState::kLive, payload)); }

  [[nodiscard]] auto state() const -> NodeState { return static_cast<NodeState>(bits_ >> kPayloadBits); }
  [[nodiscard]] auto payload() const -> uint64_t { return bits_ & kPayloadMask; }
  [[nodiscard]] auto raw() const -> uint64_t { return bits_; }

  [[nodiscard]] auto with_state(NodeState state) const -> PackedValue { return PackedValue(encode(state, payload())); }

  friend auto operator==(PackedValue lhs, PackedValue rhs) -> bool { return lhs.bits_ == rhs.bits_; }
  friend auto operator!=(PackedValue lhs, PackedValue rhs) -> bool { return lhs.bits_ != rhs.bits_; }

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
  std::atomic<uint64_t> epoch{0};  // Reserved for 3 章; unused until checkpoint/recovery land.
  Key key{};
  std::atomic<uint64_t> value{0};  // PackedValue::raw().
  std::atomic<Offset> forward0{kNullOffset};
};

template <typename Key, typename Compare = std::less<Key>>
class PSkipList {
 public:
  // capacity_nodes stands in for high_level_design.md 2.6's capacity_bytes until real
  // mmap'd storage lands; fixed for the object's lifetime, matching that design.
  explicit PSkipList(size_t capacity_nodes) : nodes_(capacity_nodes + 2) {
    nodes_[kHead].forward0.store(kTail, std::memory_order_relaxed);
  }

  PSkipList(const PSkipList &) = delete;
  auto operator=(const PSkipList &) -> PSkipList & = delete;

  [[nodiscard]] auto get(const Key &key) const -> std::optional<uint64_t> {
    Offset predecessor = kNullOffset;
    const Offset current = find_at_or_after(key, &predecessor);
    if (current == kNullOffset || current == kTail || !keys_equal(nodes_[current].key, key)) {
      return std::nullopt;
    }
    const PackedValue v(nodes_[current].value.load(std::memory_order_acquire));
    // Both tombstoned states are treated identically here: tombstoned-unlinked nodes can
    // still be reached via a stale (not-yet-fixed-up) predecessor pointer (4.1 章).
    if (v.state() != NodeState::kLive) return std::nullopt;
    return v.payload();
  }

  // Returns false only on capacity exhaustion (2.6 章). Retries internally on CAS
  // conflicts with concurrent put/remove/unlink (4.1 章) -- never fails for that reason.
  [[nodiscard]] auto put(const Key &key, uint64_t payload) -> bool {
    // Allocated at most once per call and reused across predecessor-CAS retries below --
    // threads inserting *different* keys can still race for the same predecessor slot
    // (e.g. adjacent keys), so retrying must not re-allocate on every attempt (that would
    // burn through capacity_bytes, 2.6 章, far faster than the number of distinct keys
    // actually being inserted; there is no reclaim yet, 2.3 章, so any allocation not used
    // by a winning CAS is leaked for the object's lifetime).
    Offset fresh = kNullOffset;
    for (;;) {
      Offset predecessor = kNullOffset;
      const Offset existing = find_at_or_after(key, &predecessor);
      if (existing != kNullOffset && existing != kTail && keys_equal(nodes_[existing].key, key)) {
        uint64_t expected = nodes_[existing].value.load(std::memory_order_acquire);
        const uint64_t desired = PackedValue::live(payload).raw();
        if (nodes_[existing].value.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
          return true;  // Update, or resurrect of a tombstoned node -- same CAS either way.
          // `fresh`, if allocated, is now leaked: a concurrent insert of this exact key won.
        }
        continue;  // Lost a race; retry from find().
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
        return true;  // insert(new key) linearization point.
      }
      // Lost the predecessor race; retry with the same `fresh` node, no new allocation.
    }
  }

  // Returns true only if the key was live and this call transitioned it to tombstoned
  // (the linearization point, 4.1 章). Physical unlink is attempted best-effort inline
  // here in place of the background sweep described in 2.3 章 (not implemented yet).
  [[nodiscard]] auto remove(const Key &key) -> bool {
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
    Offset predecessor = kNullOffset;
    Offset current = find_at_or_after(begin, &predecessor);
    while (current != kNullOffset && current != kTail) {
      const auto &node = nodes_[current];
      if (!less_(node.key, end)) break;  // [begin, end)
      const PackedValue v(node.value.load(std::memory_order_acquire));
      if (v.state() == NodeState::kLive) {
        callback(node.key, v.payload());
      }
      current = node.forward0.load(std::memory_order_acquire);
    }
  }

 private:
  static constexpr Offset kHead = 0;
  static constexpr Offset kTail = 1;

  [[nodiscard]] auto keys_equal(const Key &a, const Key &b) const -> bool {
    return !less_(a, b) && !less_(b, a);
  }

  // Walks level 0 from head, returning the first node whose key is >= `key` (or kTail),
  // and setting *predecessor to the immediately preceding offset. O(N): upper levels
  // (2.4 章) are not implemented yet, so there is no O(log N) path in this increment.
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
    const Offset next = high_water_mark_.fetch_add(1, std::memory_order_relaxed);
    if (next >= nodes_.size()) {
      high_water_mark_.fetch_sub(1, std::memory_order_relaxed);
      return kNullOffset;
    }
    return next;
  }

  // 4.1 章: tombstoned-linked -> tombstoned-unlinked CAS (the first step of physical
  // unlink). On success, fixes up the predecessor's real pointer (correctness-irrelevant
  // cleanup, 4.1 章). If this races a concurrent resurrect (put()), the CAS simply fails
  // and this function gives up -- no separate reachability re-check is needed.
  void physically_unlink_best_effort(const Key &key, Offset node_offset) {
    uint64_t expected = nodes_[node_offset].value.load(std::memory_order_acquire);
    const PackedValue v(expected);
    if (v.state() != NodeState::kTombstonedLinked) return;
    const uint64_t desired = v.with_state(NodeState::kTombstonedUnlinked).raw();
    if (!nodes_[node_offset].value.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
      return;
    }

    Offset predecessor = kNullOffset;
    const Offset found = find_at_or_after(key, &predecessor);
    if (found != node_offset) return;  // Already unlinked by a helper.
    Offset expected_next = node_offset;
    const Offset successor = nodes_[node_offset].forward0.load(std::memory_order_acquire);
    nodes_[predecessor].forward0.compare_exchange_strong(expected_next, successor, std::memory_order_acq_rel);
    // `node_offset` is now unreachable but not reclaimed -- 2.3 章 not implemented yet.
  }

  std::vector<DurableNode<Key>> nodes_;
  std::atomic<Offset> high_water_mark_{2};  // 0=head, 1=tail; bump allocation starts at 2.
  Compare less_{};
};

}  // namespace pskiplist
