#pragma once

#include <algorithm>
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
#include "pskiplist/detail/upper_arena.hpp"
#include "pskiplist/detail/upper_chunk.hpp"

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
    // Individual deallocate_upper_chunk() calls below just decrement each node's owning arena
    // block's live_count (see upper_arena.hpp) -- upper_arena_'s own destructor (a member,
    // destroyed after this body runs) unconditionally frees every block's buffer regardless of
    // live_count, so walking these lists here is about correctness of *use* (nothing else may
    // still be touching these nodes, since the whole PSkipList is being destroyed) rather than
    // about reclaiming memory that would otherwise leak.
    UpperChunk<Key> *current = marked_ptr_value<UpperChunk<Key>>(upper_heads_[0].load(std::memory_order_relaxed));
    while (current != nullptr) {
      UpperChunk<Key> *next = marked_ptr_value<UpperChunk<Key>>(current->forwards[0].load(std::memory_order_relaxed));
      deallocate_upper_chunk(upper_arena_, current);
      current = next;
    }
    UpperChunk<Key> *pending = pending_upper_deletes_.load(std::memory_order_relaxed);
    while (pending != nullptr) {
      UpperChunk<Key> *next = pending->next_pending.load(std::memory_order_relaxed);
      deallocate_upper_chunk(upper_arena_, pending);
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
        link_upper_levels(fresh, key);
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

    unlink_from_upper_levels(key, current);
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
    UpperChunk<Key> *pending_upper = pending_upper_deletes_.exchange(nullptr, std::memory_order_acq_rel);

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
      UpperChunk<Key> *next = pending_upper->next_pending.load(std::memory_order_relaxed);
      deallocate_upper_chunk(upper_arena_, pending_upper);
      pending_upper = next;
    }

    // Only now -- after the epoch drain above has proven no writer still holds a `current_`-
    // derived reference into a since-retired arena block -- is it safe to actually free any
    // block whose live_count reached zero (see UpperArena::release()'s comment for the race
    // this ordering closes).
    upper_arena_.drain_pending_blocks();
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
      link_upper_levels(current, nodes_[current].key);
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

  [[nodiscard]] auto upper_word(UpperChunk<Key> *pred, int level) const -> std::atomic<uint64_t> & {
    return pred == nullptr ? upper_heads_[level - 1] : pred->forwards[level - 1];
  }

  // Whoever's splice brings levels_remaining to 0 unlinked this chunk from its last list and
  // hands it to the reclaim queue (a concurrent reader may still be mid-traversal through it).
  void on_upper_splice(UpperChunk<Key> *chunk) const {
    if (chunk->levels_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      enqueue_pending_upper_delete(chunk);
    }
  }

  // Cascading top-down walk across all upper levels, comparing chunks by their immutable
  // primary_key (set once at chunk creation — see upper_chunk.hpp): at each level, positions
  // from the predecessor found one level up rather than that level's own head. Returns the
  // level-1 predecessor: the chunk with the largest primary_key strictly less than `key`, or
  // nullptr if no chunk's primary_key is < `key`. This is the read-path *hint* predecessor
  // (its offset must never be >= `key`, since find_at_or_after only ever walks forward from a
  // hint) -- it is NOT the same thing as "the chunk owning key's territory" when `key` happens
  // to equal some chunk's own primary_key exactly; see find_owning_chunk() for that. When
  // `out_anchors` is non-null, also records each level <= `height`'s incoming predecessor into
  // it — link_upper_levels()'s way of seeding every level a brand new chunk will insert into
  // without re-walking from scratch per level. When `out_current` is non-null, receives the
  // level-1 search's own return value (the first chunk found with primary_key >= `key`, or
  // nullptr) -- find_owning_chunk() uses this to detect an exact primary_key match without a
  // second traversal.
  auto cascade_upper_levels(const Key &key,
                            UpperChunk<Key> **out_anchors,
                            int height,
                            UpperChunk<Key> **out_current = nullptr) const -> UpperChunk<Key> * {
    UpperChunk<Key> *pred = nullptr;
    UpperChunk<Key> *level1_current = nullptr;
    for (int level = kDefaultMaxLevel; level >= 1; --level) {
      if (out_anchors != nullptr && level <= height) {
        out_anchors[level - 1] = pred;
      }
      UpperChunk<Key> *found_pred = nullptr;
      level1_current = find_upper_at_level_from(pred, key, level, &found_pred);
      pred = found_pred;
    }
    if (out_current != nullptr) {
      *out_current = level1_current;
    }
    return pred;
  }

  // Finds the chunk that owns `key`'s territory for insertion/removal purposes: either a chunk
  // whose primary_key exactly equals `key` (key was itself the first key ever routed to this
  // territory), or -- if none -- the predecessor chunk whose primary_key is the largest one
  // still less than `key`. Distinct from cascade_upper_levels()'s own return value, which read
  // paths need to stay strictly less than `key` for use as a Level 0 search hint; insertion and
  // removal instead need the territory match itself, including the boundary case where `key`
  // *is* the territory's own anchor.
  [[nodiscard]] auto find_owning_chunk(const Key &key) const -> UpperChunk<Key> * {
    UpperChunk<Key> *current = nullptr;
    UpperChunk<Key> *pred = cascade_upper_levels(key, nullptr, 0, &current);
    if (current != nullptr && keys_equal(current->primary_key, key)) {
      return current;
    }
    return pred;
  }

  // Scans `chunk`'s up-to-kCapacity entries for the tightest usable Level 0 hint: the largest
  // published, still-live entry key strictly less than `key`. cascade_upper_levels() guarantees
  // chunk->primary_key < key (that's what makes `chunk` the returned predecessor in the first
  // place), so the primary entry is always a valid fallback candidate and this never returns
  // kNullOffset for a non-null chunk. Only `live` entries are considered: a dead entry's
  // durable_offset may already have been physically reclaimed and reused for an unrelated key
  // once its removal is checkpointed durable, so it is not safe as a hint even though its
  // (immutable) key value would still compare correctly.
  [[nodiscard]] auto best_hint_in_chunk(UpperChunk<Key> *chunk, const Key &key) const -> Offset {
    Offset best_offset = chunk->primary_offset;
    const Key *best_key = &chunk->primary_key;
    const int claimed_count = std::min(chunk->claimed.load(std::memory_order_acquire), UpperChunk<Key>::kCapacity);
    for (int i = 0; i < claimed_count; ++i) {
      const auto &entry = chunk->entries[i];
      if (!entry.published.load(std::memory_order_acquire) || !entry.live.load(std::memory_order_acquire)) {
        continue;
      }
      if (!less_(entry.key, key)) {
        continue;  // only keys strictly less than `key` are usable hints
      }
      if (less_(*best_key, entry.key)) {
        best_key = &entry.key;
        best_offset = entry.durable_offset;
      }
    }
    return best_offset;
  }

  [[nodiscard]] auto search_upper_levels(const Key &key) const -> UpperSearchResult {
    UpperSearchResult result;
    if (UpperChunk<Key> *pred = cascade_upper_levels(key, nullptr, 0); pred != nullptr) {
      result.level0_hint = best_hint_in_chunk(pred, key);
    }
    return result;
  }

  // The one-level search cascade_upper_levels and link_upper_levels both need: stops at the
  // first chunk whose primary_key is >= `key`. `start` is the predecessor found one level up
  // (or null), never a fresh search from this level's own head.
  [[nodiscard]] auto find_upper_at_level_from(UpperChunk<Key> *start,
                                              const Key &key,
                                              int level,
                                              UpperChunk<Key> **out_pred) const -> UpperChunk<Key> * {
    return marked_list_find<UpperChunk<Key> *, PointerMarkedTraits<UpperChunk<Key>>>(
        start,
        upper_word(start, level),
        nullptr,
        upper_heads_[level - 1],
        key,
        less_,
        [level](UpperChunk<Key> *c) -> std::atomic<uint64_t> & { return c->forwards[level - 1]; },
        [](UpperChunk<Key> *c) -> const Key & { return c->primary_key; },
        [this](UpperChunk<Key> *c) { on_upper_splice(c); },
        out_pred);
  }

  // Single-level search for one specific chunk (by pointer identity, not primary_key alone,
  // since a level can hold several chunks whose primary_keys all compare less than `key`):
  // retire_chunk()'s cleanup must target the exact chunk being retired.
  [[nodiscard]] auto find_upper_chunk_at_level(UpperChunk<Key> *target,
                                               const Key &key,
                                               int level,
                                               UpperChunk<Key> **out_pred) const -> UpperChunk<Key> * {
    UpperChunk<Key> *pred = nullptr;
    UpperChunk<Key> *current =
        marked_ptr_value<UpperChunk<Key>>(upper_heads_[level - 1].load(std::memory_order_acquire));
    while (current != nullptr && !less_(key, current->primary_key)) {
      if (current == target) {
        *out_pred = pred;
        return current;
      }
      pred = current;
      current = marked_ptr_value<UpperChunk<Key>>(current->forwards[level - 1].load(std::memory_order_acquire));
    }
    *out_pred = pred;
    return nullptr;
  }

  // Tries to pack (offset, key) into an existing chunk's remaining capacity -- the fast, common
  // path for a key whose territory already has a chunk with room: an atomic slot claim and a
  // published write, no linked-list mutation at all. Returns false if `chunk` is already full
  // (every slot already claimed), in which case the caller must fall back to creating and
  // splicing in a brand new chunk.
  [[nodiscard]] auto try_insert_into_chunk(UpperChunk<Key> *chunk, Offset offset, const Key &key) -> bool {
    const int slot = chunk->claimed.fetch_add(1, std::memory_order_acq_rel);
    if (slot >= UpperChunk<Key>::kCapacity) {
      chunk->claimed.fetch_sub(1, std::memory_order_acq_rel);
      return false;
    }
    chunk->live_count.fetch_add(1, std::memory_order_acq_rel);
    auto &entry = chunk->entries[slot];
    entry.key = key;
    entry.durable_offset = offset;
    entry.live.store(true, std::memory_order_relaxed);
    entry.published.store(true, std::memory_order_release);

    // Mirrors link_upper_levels()'s own self-heal below: a concurrent remove() may have
    // tombstoned `offset` while this call was still claiming/publishing its slot, and (since
    // this entry didn't exist in any chunk until the publish store above) that remove()'s own
    // scan of this chunk could not have found it yet. If so, retire this one entry ourselves --
    // the CAS ensures only one of {this self-heal, a genuinely concurrent remove() that now
    // finds the freshly published entry} actually decrements live_count.
    if (nodes_[offset].state.load(std::memory_order_acquire) != NodeState::kLive) {
      bool expected_live = true;
      if (entry.live.compare_exchange_strong(expected_live, false, std::memory_order_acq_rel) &&
          chunk->live_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        retire_chunk(chunk, key);
      }
    }
    return true;
  }

  void link_upper_levels(Offset fresh, const Key &key) {
    if (nodes_[fresh].state.load(std::memory_order_acquire) != NodeState::kLive) {
      // A concurrent remove() already tombstoned `fresh` before this began -- nothing to link.
      return;
    }

    if (UpperChunk<Key> *owner = find_owning_chunk(key); owner != nullptr) {
      if (try_insert_into_chunk(owner, fresh, key)) {
        return;  // fast path: packed into an existing chunk, no linked-list mutation needed
      }
    }

    // Slow path: the predecessor's chunk (if any) is full, or there is no chunk at all yet for
    // this territory -- create a fresh chunk seeded with just this one key and splice it into
    // the outer linked list, exactly as the original one-key-per-node design always did. This
    // incidentally subdivides whatever territory it lands in going forward; older entries
    // already packed into a full chunk are never rebalanced (see upper_chunk.hpp).
    const int height = level_generator_.next_level();
    UpperChunk<Key> *chunk = allocate_upper_chunk(upper_arena_, fresh, key, height);

    std::array<UpperChunk<Key> *, kDefaultMaxLevel> anchors{};
    cascade_upper_levels(key, anchors.data(), height);

    // Bottom-up insert into levels 1..height (ASCS insert order), reusing each level's cached
    // anchor across CAS retries — usually still a shortcut, correct even when stale.
    for (int level = 1; level <= height; ++level) {
      UpperChunk<Key> *hint = anchors[static_cast<size_t>(level - 1)];
      for (;;) {
        UpperChunk<Key> *found_pred = nullptr;
        UpperChunk<Key> *successor = find_upper_at_level_from(hint, key, level, &found_pred);
        chunk->forwards[level - 1].store(pack_marked_ptr(successor, false), std::memory_order_relaxed);
        uint64_t expected = pack_marked_ptr(successor, false);
        const uint64_t desired = pack_marked_ptr(chunk, false);
        if (upper_word(found_pred, level).compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
          break;
        }
      }
    }

    // A concurrent remove() may have tombstoned `fresh` at any point during the insertion loop
    // above; its own unlink attempt (unlink_from_upper_levels(), which needs to find this
    // chunk via the same cascade_upper_levels() search) could not have found a chunk that
    // hadn't finished being spliced in yet. Re-checking state now and self-healing closes that
    // race without needing any liveness signal cached separately: whichever of {this call,
    // remove()'s own call} finishes last is guaranteed to observe every level already inserted
    // (this call's insertion loop has just finished them all), so it alone is enough to catch
    // anything the other missed.
    if (nodes_[fresh].state.load(std::memory_order_acquire) != NodeState::kLive) {
      bool expected_live = true;
      if (chunk->entries[0].live.compare_exchange_strong(expected_live, false, std::memory_order_acq_rel) &&
          chunk->live_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        retire_chunk(chunk, key);
      }
    }
  }

  // Marks one specific entry (matched by durable_offset) inside the chunk responsible for
  // `key`'s territory as no longer live. A chunk's primary_key never changes, so the same
  // find_owning_chunk() routing link_upper_levels() used to place this key will always find the
  // same chunk. Once a chunk's last live entry is gone, retires the whole chunk from every level
  // it participates in (retire_chunk()) and queues it for EBR reclaim.
  void unlink_from_upper_levels(const Key &key, Offset target_offset) {
    UpperChunk<Key> *chunk = find_owning_chunk(key);
    if (chunk == nullptr) {
      return;  // defensive: every live key's chunk is found by the exact routing it was
               // inserted with, so this should not happen in practice.
    }
    const int claimed_count = std::min(chunk->claimed.load(std::memory_order_acquire), UpperChunk<Key>::kCapacity);
    for (int i = 0; i < claimed_count; ++i) {
      auto &entry = chunk->entries[i];
      if (!entry.published.load(std::memory_order_acquire) || entry.durable_offset != target_offset) {
        continue;
      }
      bool expected_live = true;
      if (entry.live.compare_exchange_strong(expected_live, false, std::memory_order_acq_rel) &&
          chunk->live_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        retire_chunk(chunk, key);
      }
      return;
    }
  }

  // Eager top-down splice of one whole chunk's outer-linked-list entries, called once every
  // entry it ever held has been removed (from unlink_from_upper_levels()/try_insert_into_chunk()/
  // link_upper_levels()'s self-heal, whichever happens to decrement live_count to 0). Idempotent
  // against a level another caller already spliced -- find_upper_chunk_at_level() simply won't
  // find it there any more.
  void retire_chunk(UpperChunk<Key> *target, const Key &key) {
    for (int level = kDefaultMaxLevel; level >= 1; --level) {
      UpperChunk<Key> *pred = nullptr;
      UpperChunk<Key> *current = find_upper_chunk_at_level(target, key, level, &pred);
      if (current == nullptr) {
        continue;
      }
      marked_list_mark_for_deletion<UpperChunk<Key> *, PointerMarkedTraits<UpperChunk<Key>>>(
          current, [level](UpperChunk<Key> *c) -> std::atomic<uint64_t> & { return c->forwards[level - 1]; });
      const uint64_t successor_raw = current->forwards[level - 1].load(std::memory_order_acquire);
      uint64_t expected = pack_marked_ptr(current, false);
      const uint64_t desired = pack_marked_ptr(marked_ptr_value<UpperChunk<Key>>(successor_raw), false);
      if (upper_word(pred, level).compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
        on_upper_splice(current);
      }
    }
  }

  void enqueue_pending_upper_delete(UpperChunk<Key> *chunk) const {
    stack_push(pending_upper_deletes_, chunk, [](UpperChunk<Key> *c, UpperChunk<Key> *next) {
      c->next_pending.store(next, std::memory_order_relaxed);
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
  mutable std::atomic<UpperChunk<Key> *> pending_upper_deletes_{nullptr};
  // Backs every UpperNode allocation (see upper_arena.hpp) -- only ever touched from mutating
  // contexts (link_upper_levels(), the destructor, reclaim()), so unlike the members above this
  // needs no `mutable`.
  UpperArena upper_arena_;

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
