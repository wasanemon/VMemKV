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
#include "pskiplist/detail/packed_value.hpp"
#include "pskiplist/detail/upper_node.hpp"

namespace pskiplist {

template <typename Key, typename Compare = std::less<Key>>
  requires SkipListKey<Key> && SkipListCompare<Compare, Key>
class PSkipList {
 public:
  // `capacity_bytes` is fixed for the lifetime of the mapping (2.6節) — sized generously
  // up front, since a sparse file only consumes disk for pages actually written. The data
  // file at `path` is mutated in place via MAP_SHARED; it is not rewritten on checkpoint.
  explicit PSkipList(const std::filesystem::path &path, size_t capacity_bytes)
      : path_(path),
        file_(path, capacity_bytes),
        nodes_(static_cast<DurableNode<Key> *>(file_.data())),
        capacity_slots_(file_.size() / sizeof(DurableNode<Key>)),
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

  // The upper levels (2.4節) are plain heap objects with no owning smart pointer, so they
  // need explicit cleanup. Single-threaded, like the rest of destruction (5章) — the
  // caller guarantees no concurrent access by this point, so no EBR is needed here.
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

  [[nodiscard]] auto get(const Key &key) const -> std::optional<uint64_t> {
    const EpochToken token(epoch_slots_, epoch_, EpochRole::kReader);
    Offset predecessor = kNullOffset;
    const Offset current = find_at_or_after(key, &predecessor, search_upper_levels(key).level0_hint);
    if (current == kNullOffset || current == kTail || !keys_equal(nodes_[current].key, key)) {
      return std::nullopt;
    }
    const PackedValue v(nodes_[current].value.load(std::memory_order_acquire));
    if (v.state() != NodeState::kLive) return std::nullopt;
    return v.payload();
  }

  [[nodiscard]] auto put(const Key &key, uint64_t payload) -> bool {
    const EpochToken token(epoch_slots_, epoch_, EpochRole::kWriter);
    const Offset hint = search_upper_levels(key).level0_hint;
    Offset fresh = kNullOffset;
    for (;;) {
      Offset predecessor = kNullOffset;
      const Offset existing = find_at_or_after(key, &predecessor, hint);
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
        nodes_[fresh].epoch.store(epoch_.load(std::memory_order_acquire), std::memory_order_relaxed);
      }
      nodes_[fresh].forward0.store(pack_forward(existing, false), std::memory_order_relaxed);

      uint64_t expected_next = pack_forward(existing, false);
      const uint64_t desired_next = pack_forward(fresh, false);
      if (nodes_[predecessor].forward0.compare_exchange_strong(expected_next, desired_next, std::memory_order_acq_rel)) {
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
      uint64_t expected = nodes_[current].value.load(std::memory_order_acquire);
      const PackedValue v(expected);
      if (v.state() != NodeState::kLive) return false;
      const uint64_t desired = v.with_state(NodeState::kTombstonedLinked).raw();
      if (nodes_[current].value.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
        break;
      }
    }

    unlink_upper_levels(key, current);
    physically_unlink_best_effort(key, current);
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
      const PackedValue v(node.value.load(std::memory_order_acquire));
      if (v.state() == NodeState::kLive) {
        callback(node.key, v.payload());
      }
      current = forward_offset(node.forward0.load(std::memory_order_acquire));
    }
  }

  void reclaim() {
    const std::lock_guard<std::mutex> epoch_lock(epoch_mutex_);

    // Take exclusive ownership of every offset currently pending unlink; nothing else
    // will touch these nodes' forward0 links until free_push() below repurposes them.
    Offset pending_head = drain_pending_unlinks();
    UpperNode *pending_upper = pending_upper_deletes_.exchange(nullptr, std::memory_order_acq_rel);

    // Physical-reclaim safety needs every possible holder of a stale offset — reader or
    // writer — to have drained, unlike checkpoint()'s writer-only wait (3章).
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

  // Synchronous and blocking. Any put()/remove() that had already returned before this
  // call started is durable once this returns true; whether one that started during the
  // call is durable is unspecified. Concurrent checkpoint() calls are serialized on
  // epoch_mutex_. Only writers are waited for — a long-running scan() never blocks this.
  [[nodiscard]] auto checkpoint() -> bool {
    const std::lock_guard<std::mutex> epoch_lock(epoch_mutex_);

    const uint64_t published_epoch = epoch_.load(std::memory_order_acquire);
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    auto &old_slot = epoch_slots_[published_epoch & 1];
    while (old_slot.writers.load(std::memory_order_acquire) != 0) {
      std::this_thread::yield();
    }

    file_.sync();
    write_manifest(path_, published_epoch, high_water_mark_.load(std::memory_order_acquire));
    return true;
  }

 private:
  static constexpr Offset kHead = 0;
  static constexpr Offset kTail = 1;

  // Traits for marked_list.hpp's generic algorithms, binding them to Offset identities and
  // Level 0's existing (offset, mark bit) packing (marked_offset.hpp) unchanged.
  struct OffsetMarkedTraits {
    static constexpr auto pack(Offset id, bool marked) -> uint64_t { return pack_forward(id, marked); }
    static constexpr auto value(uint64_t raw) -> Offset { return forward_offset(raw); }
    static constexpr auto is_marked(uint64_t raw) -> bool { return forward_marked(raw); }
    static constexpr auto null_id() -> Offset { return kTail; }
  };

  void initialize_fresh() {
    ::new (&nodes_[kHead]) DurableNode<Key>();
    ::new (&nodes_[kTail]) DurableNode<Key>();
    nodes_[kHead].forward0.store(pack_forward(kTail, false), std::memory_order_relaxed);
  }

  // Called only when the backing file already had content. No manifest means no
  // checkpoint() ever completed for this file, so nothing in it is trusted — start fresh
  // exactly as a brand-new file would. Otherwise, walk Level 0 from head, trusting nodes
  // in encounter order only while their epoch stamp is <= the manifest's: a node past
  // that point may have been concurrently written by a writer checkpoint() didn't wait
  // for, so neither it nor anything reachable only through it is trustworthy, and the
  // walk stops there, splicing the chain to end at that point. Every allocated slot
  // (2章) below the manifest's high_water_mark that the walk didn't reach — whether never
  // linked in, or unlinked-but-not-yet-reclaimed before the crash — becomes free (2.3節's
  // mark-and-sweep). high_water_mark_ rolls back to the manifest's value: any allocation
  // racing the checkpoint that isn't captured by it is simply not recovered.
  void recover() {
    const auto manifest = read_manifest(path_);
    if (!manifest.has_value()) {
      initialize_fresh();
      return;
    }

    const uint64_t threshold = manifest->epoch;
    const uint64_t recovered_high_water_mark = manifest->high_water_mark;

    std::vector<bool> reached(recovered_high_water_mark, false);
    Offset pred = kHead;
    Offset current = forward_offset(nodes_[kHead].forward0.load(std::memory_order_relaxed));
    while (current != kTail) {
      if (current >= recovered_high_water_mark ||
          nodes_[current].epoch.load(std::memory_order_relaxed) > threshold) {
        nodes_[pred].forward0.store(pack_forward(kTail, false), std::memory_order_relaxed);
        break;
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
  }

  [[nodiscard]] auto keys_equal(const Key &a, const Key &b) const -> bool {
    return !less_(a, b) && !less_(b, a);
  }

  // Traverses to the first node with key >= `key`, helping to physically unlink any
  // marked (logically deleted) node it encounters along the way (4.2節). A helping CAS
  // that wins pushes the unlinked node to the pending-unlink queue; on any CAS outcome
  // touching a marked node the whole search restarts from head, since a stale predecessor
  // can no longer be trusted.
  //
  // `hint` seeds the first attempt's starting point instead of kHead — typically a
  // predecessor found via the upper levels (2.4節), letting this skip most of the Level 0
  // scan. A hint's key is safe to trust as < `key` forever (keys never change once set),
  // but it must be re-verified as still unmarked immediately before use: if it's since
  // been unlinked, its forward0 no longer means "live successor" (4.2節), and any retry
  // (from a restart below) always falls back to kHead.
  [[nodiscard]] auto find_at_or_after(const Key &key, Offset *predecessor, Offset hint = kHead) const -> Offset {
    return marked_list_find<Offset, OffsetMarkedTraits>(
        hint, nodes_[hint].forward0, kHead, nodes_[kHead].forward0, key, less_,
        [this](Offset o) -> std::atomic<uint64_t> & { return nodes_[o].forward0; },
        [this](Offset o) -> const Key & { return nodes_[o].key; }, [](Offset) { return false; },
        [this](Offset o) { enqueue_pending_unlink(o); }, predecessor);
  }

  // Lock-free multi-producer stack: any number of unlinkers push concurrently, and
  // reclaim() drains the whole thing with a single atomic exchange. There's no per-item
  // pop here, so there's no ABA hazard to guard against — unlike the free list below.
  void enqueue_pending_unlink(Offset offset) const {
    Offset old_head = pending_head_.load(std::memory_order_relaxed);
    do {
      // Keep the mark bit set: this word is already frozen against any stale unmarked-
      // expecting CAS from a thread that read this node before it was ever removed (4.2節)
      // — clearing it here would let such a CAS coincidentally match this repurposed
      // "next pending" value and corrupt the chain.
      nodes_[offset].forward0.store(pack_forward(old_head, true), std::memory_order_relaxed);
    } while (!pending_head_.compare_exchange_weak(old_head, offset, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed));
  }

  [[nodiscard]] auto drain_pending_unlinks() const -> Offset {
    return pending_head_.exchange(kNullOffset, std::memory_order_acq_rel);
  }

  // Lock-free Treiber stack with a tagged head (marked_offset.hpp) to rule out ABA: a
  // freed node's own forward0 becomes the "next free" link, safe to repurpose since
  // nothing reaches it through the live chain anymore.
  void free_push(Offset offset) {
    uint64_t old_head = free_head_.load(std::memory_order_relaxed);
    for (;;) {
      // Marked for the same reason as enqueue_pending_unlink above.
      nodes_[offset].forward0.store(pack_forward(tagged_offset(old_head), true), std::memory_order_relaxed);
      const uint64_t new_head = pack_tagged(offset, tagged_generation(old_head) + 1);
      if (free_head_.compare_exchange_weak(old_head, new_head, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
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
      if (free_head_.compare_exchange_weak(old_head, new_head, std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
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
    uint64_t expected = nodes_[node_offset].value.load(std::memory_order_acquire);
    const PackedValue v(expected);
    if (v.state() != NodeState::kTombstonedLinked) return;
    const uint64_t desired = v.with_state(NodeState::kTombstonedUnlinked).raw();
    if (!nodes_[node_offset].value.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
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

  // Level N has no deletion signal of its own — an UpperNode is nothing but a search hint
  // for a Level 0 record, so "is this entry logically gone" borrows Level 0's own tombstone
  // state directly rather than duplicating it. See marked_list_find's `is_dead` parameter.
  [[nodiscard]] auto is_upper_node_dead(UpperNode *node) const -> bool {
    return PackedValue(nodes_[node->durable_offset].value.load(std::memory_order_acquire)).state() !=
           NodeState::kLive;
  }

  // The other half of an UpperNode's removal: whoever's splice brings levels_remaining to 0
  // has just unlinked it from the last list it participated in, and is the one who hands it
  // to the EBR registry shared with 3章 (a concurrent reader may still be mid-traversal
  // through it). Called from marked_list_find's on_splice hook — for any level, from any
  // thread's search, not just an explicit unlink_upper_levels() call — since a node can be
  // helped off of each of its levels independently and by whoever gets there first.
  void on_upper_splice(UpperNode *node) const {
    if (node->levels_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      enqueue_pending_upper_delete(node);
    }
  }

  // Cascading top-down search across all upper levels (2.4節): starts at the highest
  // level's head and, at each level, walks right as far as possible before dropping down
  // one level from the same horizontal position — the standard skip-list search, built on
  // marked_list_find (marked_list.hpp) once per level. Returns the durable_offset of the
  // level-1 predecessor as a Level 0 search hint.
  //
  // `pred` carried over from the level above is exactly the same kind of thing as Level 0's
  // find_at_or_after `hint`: usually a shortcut, but possibly marked by the time this level
  // reads it. Its fallback can't be itself (unlike find_upper_at_level below, which always
  // starts at a head and never needs one) — it must be this level's own head, mirroring
  // find_at_or_after's hint-falls-back-to-kHead.
  [[nodiscard]] auto search_upper_levels(const Key &key) const -> UpperSearchResult {
    UpperSearchResult result;
    UpperNode *pred = nullptr;
    for (int level = kDefaultMaxLevel; level >= 1; --level) {
      std::atomic<uint64_t> &word = pred == nullptr ? upper_heads_[level - 1] : pred->forwards[level - 1];
      std::atomic<uint64_t> &head = upper_heads_[level - 1];
      UpperNode *found_pred = nullptr;
      static_cast<void>(marked_list_find<UpperNode *, PointerMarkedTraits<UpperNode>>(
          pred, word, nullptr, head, key, less_,
          [level](UpperNode *n) -> std::atomic<uint64_t> & { return n->forwards[level - 1]; },
          [this](UpperNode *n) -> const Key & { return nodes_[n->durable_offset].key; },
          [this](UpperNode *n) { return is_upper_node_dead(n); }, [this](UpperNode *n) { on_upper_splice(n); },
          &found_pred));
      pred = found_pred;
    }
    if (pred != nullptr) {
      result.level0_hint = pred->durable_offset;
    }
    return result;
  }

  // Single-level search, independent of any other level — used by link's own per-level
  // positioning rather than the cascading multi-level search above. Stops at the first
  // node whose key is >= `key`, which is exactly where a new node with this key belongs.
  [[nodiscard]] auto find_upper_at_level(const Key &key, int level, UpperNode **out_pred) const -> UpperNode * {
    std::atomic<uint64_t> &head = upper_heads_[level - 1];
    return marked_list_find<UpperNode *, PointerMarkedTraits<UpperNode>>(
        nullptr, head, nullptr, head, key, less_,
        [level](UpperNode *n) -> std::atomic<uint64_t> & { return n->forwards[level - 1]; },
        [this](UpperNode *n) -> const Key & { return nodes_[n->durable_offset].key; },
        [this](UpperNode *n) { return is_upper_node_dead(n); }, [this](UpperNode *n) { on_upper_splice(n); },
        out_pred);
  }

  // Single-level search for one specific durable_offset, not just any node with a matching
  // key — remove()'s upper-level cleanup can't rely on key alone: an entirely different
  // occurrence of the same key can already have been removed and reinserted, landing a
  // different UpperNode at this same sorted position, so this scans past same-key entries
  // until the offset matches (or the key strictly exceeds `key`). Marking doesn't touch a
  // node's successor value (marked_list_mark_for_deletion), so it's safe to keep walking
  // through a marked node here without helping it off — some other search will.
  [[nodiscard]] auto find_upper_node_at_level(Offset target_offset, const Key &key, int level,
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

  // Bottom-up insert into levels 1..height (8章, ASCS insert order), lock-free: each level's
  // CAS validates "the successor is still what I read" and "the predecessor isn't itself
  // being marked out from under me" in one shot (marked_list.hpp), so a losing race just
  // means retrying that level's positioning search, never data loss or corruption.
  //
  // put()'s Level 0 CAS makes `fresh` live and reachable before this runs, so a concurrent
  // remove() on the same key can tombstone it before any of this has happened. Checking once
  // up front skips the insert entirely in the common case — Level 0's O(N) fallback still
  // finds the key correctly without an upper-level entry. It isn't airtight on its own
  // (remove() could still win on the very last level, after this check passed): the backstop
  // is is_upper_node_dead() above, which every future search checks for every node it visits
  // regardless of who created it or when — so a rare entry that slips past this check is
  // still guaranteed to be found, marked, and reclaimed by the next search that reaches it.
  void link_upper_levels(Offset fresh, const Key &key, int height) {
    const PackedValue v(nodes_[fresh].value.load(std::memory_order_acquire));
    if (v.state() != NodeState::kLive) {
      return;
    }
    UpperNode *node = allocate_upper_node(fresh, height);
    for (int level = 1; level <= height; ++level) {
      for (;;) {
        UpperNode *pred = nullptr;
        UpperNode *successor = find_upper_at_level(key, level, &pred);
        node->forwards[level - 1].store(pack_marked_ptr(successor, false), std::memory_order_relaxed);
        std::atomic<uint64_t> &pred_word = pred == nullptr ? upper_heads_[level - 1] : pred->forwards[level - 1];
        uint64_t expected = pack_marked_ptr(successor, false);
        const uint64_t desired = pack_marked_ptr(node, false);
        if (pred_word.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
          break;
        }
        // Predecessor changed (a racing insert/delete nearby, or it was itself just marked)
        // — reposition at this level and retry.
      }
    }
  }

  // Top-down eager cleanup of one specific node's upper-level entries (8章, ASCS delete
  // order) — an optimization, not a correctness requirement: is_upper_node_dead() (above)
  // guarantees any level this misses still gets marked, spliced, and reclaimed the next time
  // any search passes through it, exactly as if this had never been called. `target_offset`
  // identifies exactly which occurrence of `key` to remove — see find_upper_node_at_level().
  void unlink_upper_levels(const Key &key, Offset target_offset) {
    for (int level = kDefaultMaxLevel; level >= 1; --level) {
      UpperNode *pred = nullptr;
      UpperNode *current = find_upper_node_at_level(target_offset, key, level, &pred);
      if (current == nullptr) {
        continue;  // this occurrence never reached this level
      }
      marked_list_mark_for_deletion<UpperNode *, PointerMarkedTraits<UpperNode>>(
          current, [level](UpperNode *n) -> std::atomic<uint64_t> & { return n->forwards[level - 1]; });
      std::atomic<uint64_t> &pred_word = pred == nullptr ? upper_heads_[level - 1] : pred->forwards[level - 1];
      const uint64_t successor_raw = current->forwards[level - 1].load(std::memory_order_acquire);
      uint64_t expected = pack_marked_ptr(current, false);
      const uint64_t desired = pack_marked_ptr(marked_ptr_value<UpperNode>(successor_raw), false);
      if (pred_word.compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
        on_upper_splice(current);
      }
      // A failed splice here just means a concurrent change beat us to it (possibly already
      // helped by another search) — nothing to redo, the node is still marked either way.
    }
  }

  // Lock-free stack of UpperNodes awaiting deallocation, linked via their own dedicated
  // next_pending field (upper_node.hpp) — never forwards[], which a concurrent reader may
  // still legitimately dereference after this node is unlinked (see there for why).
  void enqueue_pending_upper_delete(UpperNode *node) const {
    UpperNode *old_head = pending_upper_deletes_.load(std::memory_order_relaxed);
    do {
      node->next_pending.store(old_head, std::memory_order_relaxed);
    } while (!pending_upper_deletes_.compare_exchange_weak(old_head, node, std::memory_order_acq_rel,
                                                             std::memory_order_relaxed));
  }

  std::filesystem::path path_;
  MmapFile file_;
  DurableNode<Key> *nodes_;
  size_t capacity_slots_;
  std::atomic<Offset> high_water_mark_{2};
  Compare less_{};
  LevelGenerator level_generator_;
  // Both mutable: search functions are logically const (they only ever refine a hint) but
  // physically mutate these via marked_list_find's helping splices and on_upper_splice's
  // reclaim-queue push, exactly as Level 0's `nodes_` (a pointer, not const-propagated
  // through `this`) already allows for forward0.
  mutable std::array<std::atomic<uint64_t>, kDefaultMaxLevel> upper_heads_{};
  mutable std::atomic<UpperNode *> pending_upper_deletes_{nullptr};

  mutable std::array<EpochSlot, 2> epoch_slots_{};
  mutable std::atomic<uint64_t> epoch_{0};
  std::mutex epoch_mutex_;
  mutable std::atomic<Offset> pending_head_{kNullOffset};
  std::atomic<uint64_t> free_head_{0};
};

}  // namespace pskiplist
