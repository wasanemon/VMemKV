#pragma once

#include <atomic>
#include <cstdint>

namespace pskiplist {

// Generic lock-free "find, helping to unlink any marked node along the way" search over a
// singly-linked list, shared by Level 0 (Identity = Offset) and the upper levels (Identity =
// UpperNode*). Each node's "next" pointer is packed with a deletion mark bit into one
// atomic<uint64_t> word (Traits::pack/value/is_marked), so a single CAS on a predecessor's
// word validates both "the successor is still what I read" and "the predecessor itself
// hasn't been marked out from under me".
//
// `start_id`/`start_word` is where the walk begins (a search hint); if it's already marked,
// the walk restarts from the guaranteed-live `fallback_id`/`fallback_word` (a list head)
// instead. Marking a node for deletion is entirely the caller's job (remove()'s
// unlink_upper_levels()/physically_unlink_best_effort() do this directly, with a
// retry-until-success loop — see marked_list_mark_for_deletion below) — this function only
// ever *discovers* an already-marked node (via `Traits::is_marked`, read straight off the
// node's own next-word, no other node touched) and helps splice it out, reporting the splice
// via `on_splice` on a winning CAS, then restarts from the start. Returns the first node with
// key >= `key`, or Traits::null_id() if the list runs out; `*out_pred` receives its
// predecessor.
template <typename Identity,
          typename Traits,
          typename Key,
          typename Less,
          typename NextWordFn,
          typename KeyFn,
          typename OnSpliceFn>
[[nodiscard]] auto marked_list_find(Identity start_id,
                                    std::atomic<uint64_t> &start_word,
                                    Identity fallback_id,
                                    std::atomic<uint64_t> &fallback_word,
                                    const Key &key,
                                    Less less,
                                    NextWordFn next_word,
                                    KeyFn key_of,
                                    OnSpliceFn on_splice,
                                    Identity *out_pred) -> Identity {
  Identity anchor_id = start_id;
  std::atomic<uint64_t> *anchor_word = &start_word;
  for (;;) {
    Identity pred = anchor_id;
    std::atomic<uint64_t> *pred_word = anchor_word;
    const uint64_t pred_raw = pred_word->load(std::memory_order_acquire);
    if (Traits::is_marked(pred_raw)) {
      anchor_id = fallback_id;
      anchor_word = &fallback_word;
      continue;
    }
    Identity current = Traits::value(pred_raw);
    for (;;) {
      if (current == Traits::null_id()) {
        *out_pred = pred;
        return current;
      }
      const uint64_t current_raw = next_word(current).load(std::memory_order_acquire);
      if (Traits::is_marked(current_raw)) {
        const Identity successor = Traits::value(current_raw);
        uint64_t expected = Traits::pack(current, false);
        const uint64_t desired = Traits::pack(successor, false);
        if (pred_word->compare_exchange_strong(expected, desired, std::memory_order_acq_rel)) {
          on_splice(current);
        }
        break;  // restart the whole walk from anchor_id/anchor_word — pred_word may now be stale
      }
      if (!less(key_of(current), key)) {
        *out_pred = pred;
        return current;
      }
      pred = current;
      pred_word = &next_word(current);
      current = Traits::value(current_raw);
    }
  }
}

// Marks `id`'s own word for logical deletion, preserving its current successor — the first
// half of removing a node, before any splice is attempted. No-op if already marked.
template <typename Identity, typename Traits, typename OwnWordFn>
void marked_list_mark_for_deletion(Identity id, OwnWordFn own_word) {
  uint64_t raw = own_word(id).load(std::memory_order_acquire);
  while (!Traits::is_marked(raw)) {
    const uint64_t desired = Traits::pack(Traits::value(raw), true);
    if (own_word(id).compare_exchange_strong(raw, desired, std::memory_order_acq_rel)) {
      break;
    }
  }
}

// Lock-free Treiber stack push: `value` becomes the new head, linked via `set_next(value,
// old_head)` (a callback rather than a plain word store, so a caller can repurpose an
// existing word — e.g. packing in a mark bit). No pop: every user drains the whole stack at
// once via `head.exchange(...)`, so there's no ABA hazard here (unlike the free list, which
// needs marked_offset.hpp's tagged pointer).
template <typename Identity, typename SetNextFn>
void stack_push(std::atomic<Identity> &head, Identity value, SetNextFn set_next) {
  Identity old_head = head.load(std::memory_order_relaxed);
  do {
    set_next(value, old_head);
  } while (!head.compare_exchange_weak(old_head, value, std::memory_order_acq_rel, std::memory_order_relaxed));
}

}  // namespace pskiplist
