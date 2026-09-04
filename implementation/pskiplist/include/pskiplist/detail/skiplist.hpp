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
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "pskiplist/detail/concepts.hpp"
#include "pskiplist/detail/durable_node.hpp"
#include "pskiplist/detail/epoch_token.hpp"
#include "pskiplist/detail/level_generator.hpp"
#include "pskiplist/detail/manifest.hpp"
#include "pskiplist/detail/marked_list.hpp"
#include "pskiplist/detail/marked_offset.hpp"
#include "pskiplist/detail/marked_pointer.hpp"
#include "pskiplist/detail/mmap_file.hpp"
#include "pskiplist/detail/tombstone.hpp"
#include "pskiplist/detail/upper_node.hpp"

namespace pskiplist {

template <typename Key, typename Value = uint64_t, typename Compare = std::less<Key>>
  requires SkipListKey<Key> && SkipListValue<Value> && SkipListCompare<Compare, Key>
class PSkipList {
 public:
  // `capacity_bytes` is fixed for the mapping's lifetime; size generously — a sparse file
  // only consumes disk for pages actually written.
  explicit PSkipList(const std::filesystem::path &path, size_t capacity_bytes)
      : path_(path),
        file_(path, capacity_bytes),
        nodes_(static_cast<DurableNode<Key, Value> *>(file_.data())),
        capacity_slots_(file_.size() / sizeof(DurableNode<Key, Value>)),
        level_generator_(std::random_device{}()) {
    if (capacity_slots_ < 2) {
      throw std::invalid_argument("pskiplist: capacity_bytes too small to hold head/tail sentinels");
    }
    if (capacity_slots_ > kMaxTaggedOffset) {
      throw std::invalid_argument("pskiplist: capacity_bytes exceeds the maximum representable node count");
    }
    if (file_.reused()) {
      recover();
    } else {
      initialize_fresh();
    }
  }

  ~PSkipList() {
    UpperNode *current = marked_ptr_value<UpperNode>(upper_heads_[0].load(std::memory_order_relaxed));
    while (current != nullptr) {
      UpperNode *next = marked_ptr_value<UpperNode>(current->forwards[0].load(std::memory_order_relaxed));
      deallocate_upper_node(current);
      current = next;
    }
    UpperNode *pending = pending_upper_deletes_.load(std::memory_order_relaxed);
    while (pending != nullptr) {
      UpperNode *next = pending->next_pending.load(std::memory_order_relaxed);
      deallocate_upper_node(pending);
      pending = next;
    }
  }

  PSkipList(const PSkipList &) = delete;
  auto operator=(const PSkipList &) -> PSkipList & = delete;

  [[nodiscard]] auto get(const Key &key) const -> std::optional<Value> {
    const auto found = get_with_key(key);
    if (!found.has_value()) return std::nullopt;
    return found->second;
  }

  // Like get(), but also returns the matched node's actual stored key — not merely `key` itself.
  // Useful when Compare/keys_equal treats keys as equal despite differing in some payload-carrying
  // bits Compare ignores (e.g. metadata packed into otherwise-unordered bits of a composite key).
  [[nodiscard]] auto get_with_key(const Key &key) const -> std::optional<std::pair<Key, Value>> {
    const EpochToken token(epoch_slots_, epoch_, EpochRole::kReader);
    Offset predecessor = kNullOffset;
    const Offset current = find_at_or_after(key, &predecessor, search_upper_levels(key).level0_hint);
    if (current == kNullOffset || current == kTail || !keys_equal(nodes_[current].key, key)) {
      return std::nullopt;
    }
    if (nodes_[current].state.load(std::memory_order_acquire) != NodeState::kLive) {
      return std::nullopt;
    }
    return std::make_pair(nodes_[current].key, read_value(current));
  }

  [[nodiscard]] auto put(const Key &key, const Value &payload) -> bool {
    const EpochToken token(epoch_slots_, epoch_, EpochRole::kWriter);
    const Offset hint = search_upper_levels(key).level0_hint;
    Offset fresh = kNullOffset;
    for (;;) {
      Offset predecessor = kNullOffset;
      const Offset existing = find_at_or_after(key, &predecessor, hint);
      if (existing != kNullOffset && existing != kTail && keys_equal(nodes_[existing].key, key)) {
        const NodeState state = nodes_[existing].state.load(std::memory_order_acquire);
        if (state == NodeState::kTombstonedUnlinked) {
          std::this_thread::yield();
          continue;
        }
        write_value_locked(existing, [&](Value &v) { v = payload; });
        if (state == NodeState::kTombstonedLinked) {
          NodeState expected = NodeState::kTombstonedLinked;
          if (!nodes_[existing].state.compare_exchange_strong(expected, NodeState::kLive, std::memory_order_acq_rel)) {
            // Lost the resurrect race to a concurrent physically_unlink_best_effort(), which
            // just committed to permanently retiring this offset -- the value write above is
            // harmless waste on a node that's about to be spliced out. Retry from the top.
            continue;
          }
        }
        return true;
      }

      if (fresh == kNullOffset) {
        fresh = allocate();
        if (fresh == kNullOffset) return false;
        nodes_[fresh].key = key;
        nodes_[fresh].value = payload;
        nodes_[fresh].epoch.store(epoch_.load(std::memory_order_acquire), std::memory_order_relaxed);
        // A reused slot may carry a previous occupant's state/shadow; reset it.
        nodes_[fresh].state.store(NodeState::kLive, std::memory_order_relaxed);
        nodes_[fresh].checkpointed_state.store(NodeState::kLive, std::memory_order_relaxed);
        nodes_[fresh].version.store(0, std::memory_order_relaxed);
        nodes_[fresh].checkpointed_value = Value{};
        nodes_[fresh].mutation_epoch.store(epoch_.load(std::memory_order_acquire), std::memory_order_relaxed);
      }
      nodes_[fresh].forward0.store(pack_forward(existing, false), std::memory_order_relaxed);

      uint64_t expected_next = pack_forward(existing, false);
      const uint64_t desired_next = pack_forward(fresh, false);
      if (nodes_[predecessor].forward0.compare_exchange_strong(
              expected_next, desired_next, std::memory_order_acq_rel)) {
        link_upper_levels(fresh, key, level_generator_.next_level());
        return true;
      }
    }
  }

  [[nodiscard]] auto remove(const Key &key) -> bool {
    const EpochToken token(epoch_slots_, epoch_, EpochRole::kWriter);
    Offset predecessor = kNullOffset;
    const Offset current = find_at_or_after(key, &predecessor, search_upper_levels(key).level0_hint);
    if (current == kNullOffset || current == kTail || !keys_equal(nodes_[current].key, key)) return false;

    for (;;) {
      if (nodes_[current].state.load(std::memory_order_acquire) != NodeState::kLive) {
        return false;
      }
      write_value_locked(current, [](Value &) {});  // no value change, just the shadow capture
      NodeState expected = NodeState::kLive;
      if (nodes_[current].state.compare_exchange_strong(
              expected, NodeState::kTombstonedLinked, std::memory_order_acq_rel)) {
        break;
      }
      // Lost the race to a concurrent remove() — re-check state fresh next iteration.
    }

    unlink_upper_levels(key, current);
    // Physical unlink (slot reuse) waits for checkpoint(): reusing the slot before this
    // removal is durable would destroy data a crash-then-recover should still fall back to.
    enqueue_pending_checkpoint_unlink(current);
    return true;
  }

  template <typename Callback>
  void scan(const Key &begin, const Key &end, Callback &&callback) const {
    const EpochToken token(epoch_slots_, epoch_, EpochRole::kReader);
    Offset predecessor = kNullOffset;
    Offset current = find_at_or_after(begin, &predecessor, search_upper_levels(begin).level0_hint);
    while (current != kNullOffset && current != kTail) {
      const auto &node = nodes_[current];
      if (!less_(node.key, end)) break;
      if (node.state.load(std::memory_order_acquire) == NodeState::kLive) {
        callback(node.key, read_value(current));
      }
      current = forward_offset(node.forward0.load(std::memory_order_acquire));
    }
  }

  void reclaim() {
    const std::lock_guard<std::mutex> epoch_lock(epoch_mutex_);

    Offset pending_head = drain_pending_unlinks();
    UpperNode *pending_upper = pending_upper_deletes_.exchange(nullptr, std::memory_order_acq_rel);

    // Unlike checkpoint()'s writer-only wait, physical reclaim needs every reader or writer
    // that could hold a stale offset to have drained first.
    const uint64_t old_parity = epoch_.load(std::memory_order_acquire) & 1;
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    auto &old_slot = epoch_slots_[old_parity];
    while (old_slot.readers.load(std::memory_order_acquire) != 0 ||
           old_slot.writers.load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }

    while (pending_head != kNullOffset) {
      const Offset next = forward_offset(nodes_[pending_head].forward0.load(std::memory_order_relaxed));
      free_push(pending_head);
      pending_head = next;
    }

    while (pending_upper != nullptr) {
      UpperNode *next = pending_upper->next_pending.load(std::memory_order_relaxed);
      deallocate_upper_node(pending_upper);
      pending_upper = next;
    }
  }

  // Synchronous and blocking. Any put()/remove() that had already returned before this call
  // started is durable once this returns true. Concurrent checkpoint() calls serialize on
  // epoch_mutex_; only writers are waited for, so a long-running scan() never blocks this.
  [[nodiscard]] auto checkpoint() -> bool {
    const std::lock_guard<std::mutex> epoch_lock(epoch_mutex_);

    Offset pending_unlink = pending_checkpoint_unlink_.exchange(kNullOffset, std::memory_order_acq_rel);

    const uint64_t published_epoch = epoch_.load(std::memory_order_acquire);
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    auto &old_slot = epoch_slots_[published_epoch & 1];
    while (old_slot.writers.load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }

    file_.sync();
    write_manifest(path_, published_epoch, high_water_mark_.load(std::memory_order_acquire));
    // Only after the manifest is durable — otherwise a concurrent put()/remove() could see
    // "already checkpointed" before that's true and skip a shadow recover() needs.
    last_published_epoch_.store(published_epoch, std::memory_order_release);

    while (pending_unlink != kNullOffset) {
      const Offset next = nodes_[pending_unlink].next_checkpoint_unlink.load(std::memory_order_relaxed);
      nodes_[pending_unlink].pending_checkpoint_unlink_queued.store(false, std::memory_order_release);
      physically_unlink_best_effort(nodes_[pending_unlink].key, pending_unlink);
      pending_unlink = next;
    }
    return true;
  }

 private:
  static constexpr Offset kHead = 0;
  static constexpr Offset kTail = 1;

  struct OffsetMarkedTraits {
    static constexpr auto pack(Offset id, bool marked) -> uint64_t { return pack_forward(id, marked); }
    static constexpr auto value(uint64_t raw) -> Offset { return forward_offset(raw); }
    static constexpr auto is_marked(uint64_t raw) -> bool { return forward_marked(raw); }
    static constexpr auto null_id() -> Offset { return kTail; }
  };

  void initialize_fresh() {
    ::new (&nodes_[kHead]) DurableNode<Key, Value>();
    ::new (&nodes_[kTail]) DurableNode<Key, Value>();
    nodes_[kHead].forward0.store(pack_forward(kTail, false), std::memory_order_relaxed);
  }

  // Seqlock read of `value`, mirroring the classic read-retry pattern (load version, read data,
  // fence, re-check version unchanged) — same idea as vmemkv's own T2FlatFile record seqlock.
  [[nodiscard]] auto read_value(Offset offset) const -> Value {
    const auto &node = nodes_[offset];
    for (;;) {
      const uint64_t v1 = node.version.load(std::memory_order_acquire);
      if ((v1 & 1) != 0) {
        std::this_thread::yield();
        continue;
      }
      Value result = node.value;
      std::atomic_thread_fence(std::memory_order_acquire);
      if (v1 == node.version.load(std::memory_order_relaxed)) {
        return result;
      }
    }
  }

  // Claims exclusive write access to `offset`'s value (version even->odd CAS — this doubles as
  // write-side mutual exclusion against other concurrent writers of the same node, not just a
  // reader-tear guard), shadows the pre-mutation value+state on first touch since the last
  // checkpoint, runs `mutate(value)`, then releases (version -> v+2). Used by both put() (real
  // mutation) and remove() (no-op mutation — it only needs the shadow capture).
  template <typename MutateFn>
  void write_value_locked(Offset offset, MutateFn &&mutate) {
    auto &node = nodes_[offset];
    uint64_t v = node.version.load(std::memory_order_relaxed);
    for (;;) {
      if ((v & 1) != 0) {
        std::this_thread::yield();
        v = node.version.load(std::memory_order_relaxed);
        continue;
      }
      if (node.version.compare_exchange_weak(v, v + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        break;
      }
    }
    if (node.mutation_epoch.load(std::memory_order_acquire) <= last_published_epoch_.load(std::memory_order_acquire)) {
      node.checkpointed_value = node.value;
      node.checkpointed_state.store(node.state.load(std::memory_order_acquire), std::memory_order_relaxed);
      node.mutation_epoch.store(epoch_.load(std::memory_order_acquire), std::memory_order_relaxed);
    }
    mutate(node.value);
    node.version.store(v + 2, std::memory_order_release);
  }

  // No manifest means no checkpoint() ever completed — start fresh. Otherwise walk Level 0
  // from head, trusting nodes only while their creation epoch is <= the manifest's; the walk
  // stops and splices at the first untrusted node, and every allocated-but-unreached slot
  // below high_water_mark becomes free. A trusted node's latest value/state may still be newer
  // than the checkpoint (put() or remove(), indistinguishable here), so both are reverted to
  // their shadow whenever mutation_epoch exceeds the threshold.
  void recover() {
    const auto manifest = read_manifest(path_);
    if (!manifest.has_value()) {
      initialize_fresh();
      return;
    }

    const uint64_t threshold = manifest->epoch;
    const uint64_t recovered_high_water_mark = manifest->high_water_mark;

    // epoch_ restarts at 0 every reopen; resuming from threshold + 1 keeps epoch numbers
    // monotonic across restarts (otherwise a later recovery could wrongly trim nodes this
    // session already checkpointed).
    epoch_.store(threshold + 1, std::memory_order_relaxed);
    last_published_epoch_.store(threshold, std::memory_order_relaxed);

    std::vector<bool> reached(recovered_high_water_mark, false);
    Offset pred = kHead;
    Offset current = forward_offset(nodes_[kHead].forward0.load(std::memory_order_relaxed));
    while (current != kTail) {
      if (current >= recovered_high_water_mark || nodes_[current].epoch.load(std::memory_order_relaxed) > threshold) {
        nodes_[pred].forward0.store(pack_forward(kTail, false), std::memory_order_relaxed);
        break;
      }
      if (nodes_[current].mutation_epoch.load(std::memory_order_relaxed) > threshold) {
        nodes_[current].value = nodes_[current].checkpointed_value;
        nodes_[current].state.store(nodes_[current].checkpointed_state.load(std::memory_order_relaxed),
                                    std::memory_order_relaxed);
        nodes_[current].mutation_epoch.store(threshold, std::memory_order_relaxed);
      }
      reached[current] = true;
      pred = current;
      current = forward_offset(nodes_[current].forward0.load(std::memory_order_relaxed));
    }

    free_head_.store(0, std::memory_order_relaxed);
    for (Offset offset = 2; offset < recovered_high_water_mark; ++offset) {
      if (!reached[offset]) {
        free_push(offset);
      }
    }

    high_water_mark_.store(recovered_high_water_mark, std::memory_order_relaxed);

    // A crash between checkpoint()'s manifest write and its (in-memory, lost) drain of
    // pending_checkpoint_unlink_ can leave an already-durable removal still TombstonedLinked;
    // nothing at runtime retries it, so finish it here in its own pass (recovery is
    // single-threaded, so this is safe; folding it into the walk above would corrupt that
    // walk's pred/current bookkeeping via find_at_or_after's own splicing).
    Offset walk = forward_offset(nodes_[kHead].forward0.load(std::memory_order_relaxed));
    while (walk != kTail) {
      const Offset next = forward_offset(nodes_[walk].forward0.load(std::memory_order_relaxed));
      if (nodes_[walk].state.load(std::memory_order_relaxed) == NodeState::kTombstonedLinked) {
        physically_unlink_best_effort(nodes_[walk].key, walk);
      }
      walk = next;
    }

    rebuild_upper_levels();
  }

  // Upper levels are pure DRAM search hints, never persisted, so every restart rebuilds them
  // by re-linking each surviving Level 0 node exactly as put() would. O(corpus), unavoidable
  // without persisting the upper levels themselves.
  void rebuild_upper_levels() {
    Offset current = forward_offset(nodes_[kHead].forward0.load(std::memory_order_relaxed));
    while (current != kTail) {
      link_upper_levels(current, nodes_[current].key, level_generator_.next_level());
      current = forward_offset(nodes_[current].forward0.load(std::memory_order_relaxed));
    }
  }

  [[nodiscard]] auto keys_equal(const Key &a, const Key &b) const -> bool { return !less_(a, b) && !less_(b, a); }

  // Traverses to the first node with key >= `key`, helping to physically unlink any marked
  // node along the way. `hint` (typically from the upper levels) seeds the first attempt in
  // place of kHead; it's safe to trust as < `key` forever, but is re-verified as still
  // unmarked before use, falling back to kHead otherwise.
  [[nodiscard]] auto find_at_or_after(const Key &key, Offset *predecessor, Offset hint = kHead) const -> Offset {
    return marked_list_find<Offset, OffsetMarkedTraits>(
        hint,
        nodes_[hint].forward0,
        kHead,
        nodes_[kHead].forward0,
        key,
        less_,
        [this](Offset o) -> std::atomic<uint64_t> & { return nodes_[o].forward0; },
        [this](Offset o) -> const Key & { return nodes_[o].key; },
        [](Offset) { return false; },
        [this](Offset o) { enqueue_pending_unlink(o); },
        predecessor);
  }

  void enqueue_pending_unlink(Offset offset) const {
    stack_push(pending_head_, offset, [this](Offset o, Offset next) {
      // Mark bit stays set: clearing it would let a stale unmarked-expecting CAS from before
      // this node was removed corrupt this now-repurposed "next pending" value.
      nodes_[o].forward0.store(pack_forward(next, true), std::memory_order_relaxed);
    });
  }

  [[nodiscard]] auto drain_pending_unlinks() const -> Offset {
    return pending_head_.exchange(kNullOffset, std::memory_order_acq_rel);
  }

  // Linked via next_checkpoint_unlink, never forward0, which must keep pointing at each
  // node's real successor until checkpoint() confirms the splice is safe. The queued flag
  // guards against double-enqueue when put() resurrects a TombstonedLinked node and a later
  // remove() re-tombstones it.
  void enqueue_pending_checkpoint_unlink(Offset offset) const {
    bool expected_unqueued = false;
    if (!nodes_[offset].pending_checkpoint_unlink_queued.compare_exchange_strong(
            expected_unqueued, true, std::memory_order_acq_rel)) {
      return;
    }
    stack_push(pending_checkpoint_unlink_, offset, [this](Offset o, Offset next) {
      nodes_[o].next_checkpoint_unlink.store(next, std::memory_order_relaxed);
    });
  }

  // Tagged head (marked_offset.hpp) rules out ABA on this Treiber stack.
  void free_push(Offset offset) {
    uint64_t old_head = free_head_.load(std::memory_order_relaxed);
    for (;;) {
      nodes_[offset].forward0.store(pack_forward(tagged_offset(old_head), true), std::memory_order_relaxed);
      const uint64_t new_head = pack_tagged(offset, tagged_generation(old_head) + 1);
      if (free_head_.compare_exchange_weak(old_head, new_head, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return;
      }
    }
  }

  auto free_pop() -> Offset {
    uint64_t old_head = free_head_.load(std::memory_order_acquire);
    for (;;) {
      const Offset offset = tagged_offset(old_head);
      if (offset == 0) return kNullOffset;  // empty: offset 0 (kHead) is never freed
      const Offset next = forward_offset(nodes_[offset].forward0.load(std::memory_order_relaxed));
      const uint64_t new_head = pack_tagged(next, tagged_generation(old_head) + 1);
      if (free_head_.compare_exchange_weak(old_head, new_head, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return offset;
      }
    }
  }

  auto allocate() -> Offset {
    const Offset reused = free_pop();
    if (reused != kNullOffset) return reused;

    const Offset next = high_water_mark_.fetch_add(1, std::memory_order_relaxed);
    if (next >= capacity_slots_) {
      high_water_mark_.fetch_sub(1, std::memory_order_relaxed);
      return kNullOffset;
    }
    return next;
  }

  void physically_unlink_best_effort(const Key &key, Offset node_offset) {
    NodeState expected = NodeState::kTombstonedLinked;
    // A genuine state change (not a same-value CAS): a concurrent put() racing to resurrect
    // this node holds `expected` from before this transition, so its own CAS correctly fails
    // and retries instead of resurrecting a node this call just committed to splicing out.
    if (!nodes_[node_offset].state.compare_exchange_strong(
            expected, NodeState::kTombstonedUnlinked, std::memory_order_acq_rel)) {
      return;
    }

    marked_list_mark_for_deletion<Offset, OffsetMarkedTraits>(
        node_offset, [this](Offset o) -> std::atomic<uint64_t> & { return nodes_[o].forward0; });

    Offset predecessor = kNullOffset;
    static_cast<void>(find_at_or_after(key, &predecessor));
  }

  struct UpperSearchResult {
    Offset level0_hint = kHead;
  };

  [[nodiscard]] auto upper_word(UpperNode *pred, int level) const -> std::atomic<uint64_t> & {
    return pred == nullptr ? upper_heads_[level - 1] : pred->forwards[level - 1];
  }

  // An UpperNode carries no deletion state of its own — it's nothing but a search hint for a
  // Level 0 record, so this borrows Level 0's tombstone state directly.
  [[nodiscard]] auto is_upper_node_dead(UpperNode *node) const -> bool {
    return nodes_[node->durable_offset].state.load(std::memory_order_acquire) != NodeState::kLive;
  }

  // Whoever's splice brings levels_remaining to 0 unlinked this node from its last list and
  // hands it to the reclaim queue (a concurrent reader may still be mid-traversal through it).
  void on_upper_splice(UpperNode *node) const {
    if (node->levels_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      enqueue_pending_upper_delete(node);
    }
  }

  // Cascading top-down walk across all upper levels: at each level, positions from the
  // predecessor found one level up rather than that level's own head. Returns the level-1
  // predecessor. When `out_anchors` is non-null, also records each level <= `height`'s
  // incoming predecessor into it — link_upper_levels()'s way of seeding every level it will
  // insert into without re-walking from scratch per level.
  auto cascade_upper_levels(const Key &key, UpperNode **out_anchors, int height) const -> UpperNode * {
    UpperNode *pred = nullptr;
    for (int level = kDefaultMaxLevel; level >= 1; --level) {
      if (out_anchors != nullptr && level <= height) {
        out_anchors[level - 1] = pred;
      }
      UpperNode *found_pred = nullptr;
      static_cast<void>(find_upper_at_level_from(pred, key, level, &found_pred));
      pred = found_pred;
    }
    return pred;
  }

  [[nodiscard]] auto search_upper_levels(const Key &key) const -> UpperSearchResult {
    UpperSearchResult result;
    if (UpperNode *pred = cascade_upper_levels(key, nullptr, 0); pred != nullptr) {
      result.level0_hint = pred->durable_offset;
    }
    return result;
  }

  // The one-level search cascade_upper_levels and link_upper_levels both need: stops at the
  // first node whose key is >= `key`. `start` is the predecessor found one level up (or null),
  // never a fresh search from this level's own head.
  [[nodiscard]] auto find_upper_at_level_from(UpperNode *start,
                                              const Key &key,
                                              int level,
                                              UpperNode **out_pred) const -> UpperNode * {
    return marked_list_find<UpperNode *, PointerMarkedTraits<UpperNode>>(
        start,
        upper_word(start, level),
        nullptr,
        upper_heads_[level - 1],
        key,
        less_,
        [level](UpperNode *n) -> std::atomic<uint64_t> & { return n->forwards[level - 1]; },
        [this](UpperNode *n) -> const Key & { return nodes_[n->durable_offset].key; },
        [this](UpperNode *n) { return is_upper_node_dead(n); },
        [this](UpperNode *n) { on_upper_splice(n); },
        out_pred);
  }

  // Single-level search for one specific durable_offset, not just any node with a matching
  // key: remove()'s upper-level cleanup must target the exact occurrence, since a different
  // occurrence of the same key can already have been removed and reinserted elsewhere.
  [[nodiscard]] auto find_upper_node_at_level(Offset target_offset,
                                              const Key &key,
                                              int level,
                                              UpperNode **out_pred) const -> UpperNode * {
    UpperNode *pred = nullptr;
    UpperNode *current = marked_ptr_value<UpperNode>(upper_heads_[level - 1].load(std::memory_order_acquire));
    while (current != nullptr && !less_(key, nodes_[current->durable_offset].key)) {
      if (current->durable_offset == target_offset) {
        *out_pred = pred;
        return current;
      }
      pred = current;
      current = marked_ptr_value<UpperNode>(current->forwards[level - 1].load(std::memory_order_acquire));
    }
    *out_pred = pred;
    return nullptr;
  }

  void link_upper_levels(Offset fresh, const Key &key, int height) {
    if (nodes_[fresh].state.load(std::memory_order_acquire) != NodeState::kLive) {
      // A concurrent remove() may have already tombstoned `fresh` before this runs. Skipping
      // the insert is safe: Level 0 still finds the key without an upper-level entry, and
      // is_upper_node_dead() backstops any rare entry that slips past this check anyway.
      return;
    }
    UpperNode *node = allocate_upper_node(fresh, height);

    std::array<UpperNode *, kDefaultMaxLevel> anchors{};
    cascade_upper_levels(key, anchors.data(), height);

    // Bottom-up insert into levels 1..height (ASCS insert order), reusing each level's cached
    // anchor across CAS retries — usually still a shortcut, correct even when stale.
    for (int level = 1; level <= height; ++level) {
      UpperNode *hint = anchors[static_cast<size_t>(level - 1)];
      for (;;) {
        UpperNode *found_pred = nullptr;
        UpperNode *successor = find_upper_at_level_from(hint, key, level, &found_pred);
        node->forwards[level - 1].store(pack_marked_ptr(successor, false), std::memory_order_relaxed);
        uint64_t expected = pack_marked_ptr(successor, false);
        const uint64_t desired = pack_marked_ptr(node, false);
        if (upper_word(found_pred, level).compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
          break;
        }
      }
    }
  }

  // Eager top-down cleanup of one node's upper-level entries — an optimization, not a
  // correctness requirement: is_upper_node_dead() guarantees any level this misses still gets
  // marked, spliced, and reclaimed the next time a search passes through it.
  void unlink_upper_levels(const Key &key, Offset target_offset) {
    for (int level = kDefaultMaxLevel; level >= 1; --level) {
      UpperNode *pred = nullptr;
      UpperNode *current = find_upper_node_at_level(target_offset, key, level, &pred);
      if (current == nullptr) {
        continue;
      }
      marked_list_mark_for_deletion<UpperNode *, PointerMarkedTraits<UpperNode>>(
          current, [level](UpperNode *n) -> std::atomic<uint64_t> & { return n->forwards[level - 1]; });
      const uint64_t successor_raw = current->forwards[level - 1].load(std::memory_order_acquire);
      uint64_t expected = pack_marked_ptr(current, false);
      const uint64_t desired = pack_marked_ptr(marked_ptr_value<UpperNode>(successor_raw), false);
      if (upper_word(pred, level).compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
        on_upper_splice(current);
      }
    }
  }

  void enqueue_pending_upper_delete(UpperNode *node) const {
    stack_push(pending_upper_deletes_, node, [](UpperNode *n, UpperNode *next) {
      n->next_pending.store(next, std::memory_order_relaxed);
    });
  }

  std::filesystem::path path_;
  MmapFile file_;
  DurableNode<Key, Value> *nodes_;
  size_t capacity_slots_;
  std::atomic<Offset> high_water_mark_{2};
  Compare less_{};
  LevelGenerator level_generator_;
  // mutable: search functions are logically const (they only ever refine a hint) but
  // physically mutate these via helping splices and reclaim-queue pushes.
  mutable std::array<std::atomic<uint64_t>, kDefaultMaxLevel> upper_heads_{};
  mutable std::atomic<UpperNode *> pending_upper_deletes_{nullptr};

  mutable std::array<EpochSlot, 2> epoch_slots_{};
  mutable std::atomic<uint64_t> epoch_{0};
  std::mutex epoch_mutex_;
  mutable std::atomic<Offset> pending_head_{kNullOffset};
  std::atomic<uint64_t> free_head_{0};

  // Epoch of the last successful checkpoint() — distinct from epoch_, which reclaim() also
  // bumps for EBR purposes.
  mutable std::atomic<uint64_t> last_published_epoch_{0};
  mutable std::atomic<Offset> pending_checkpoint_unlink_{kNullOffset};
};

}  // namespace pskiplist
