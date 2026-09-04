#pragma once

#include <atomic>
#include <cstdint>

namespace pskiplist {

// A generic lock-free "find, helping to unlink any marked node along the way" search over a
// singly-linked list whose nodes are addressed by `Identity` (an Offset into a fixed array
// for Level 0, or a raw pointer for an upper level — see marked_offset.hpp / marked_pointer.hpp)
// and whose per-node "next" pointer is packed together with a deletion mark bit into one
// atomic<uint64_t> word (Traits::pack/value/is_marked). This is the one idea both layers
// share: a single CAS on a predecessor's word simultaneously validates "the successor is
// still what I read" and "the predecessor itself hasn't been marked out from under me",
// because both facts live in the same word.
//
// `start_id`/`start_word` is where the walk begins — typically a caller-supplied search hint
// pointing past most of the list. If that word turns out to already be marked, its offset/
// pointer bits can no longer be trusted at all (they may already have been repurposed for a
// pending-reclaim queue by the time this reads them), so the walk restarts from
// `fallback_id`/`fallback_word` instead — a caller-guaranteed-live anchor (a list head) —
// rather than retrying the same possibly-permanently-marked start forever. Callers with no
// meaningful hint (Level N's per-level searches always start at a list head, which is never
// itself deleted) just pass the same id/word twice; the fallback path is then unreachable,
// not merely harmless.
//
// `is_dead` is a second, independent deletion signal a node can carry *outside* its own
// word — Level N has no per-node source of truth of its own (an UpperNode is nothing but a
// search hint), so it borrows Level 0's tombstone state via this hook instead of maintaining
// a redundant one; Level 0 has no such external signal, so it always passes a hook that
// returns false. A node flagged dead this way but not yet structurally marked gets marked
// right here (idempotent — a no-op if a concurrent caller already did) before falling into
// the same help-splice-and-restart path as a node found already marked, so a zombie gets
// caught by whichever search encounters it first, however late.
//
// On encountering a node whose own word is marked, this helps splice it out via one CAS on
// its predecessor's word and, if that CAS wins, reports the removed identity via `on_splice`
// — then restarts the whole walk from the start (not fallback), since the predecessor used up
// to that point can no longer be trusted. Stops and returns the first node whose key is not
// less than `key`, or Traits::null_id() if the list runs out first; `*out_pred` receives that
// result's immediate predecessor.
template <typename Identity, typename Traits, typename Key, typename Less, typename NextWordFn, typename KeyFn,
          typename IsDeadFn, typename OnSpliceFn>
[[nodiscard]] auto marked_list_find(Identity start_id, std::atomic<uint64_t> &start_word, Identity fallback_id,
                                     std::atomic<uint64_t> &fallback_word, const Key &key, Less less,
                                     NextWordFn next_word, KeyFn key_of, IsDeadFn is_dead, OnSpliceFn on_splice,
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
      uint64_t current_raw = next_word(current).load(std::memory_order_acquire);
      if (!Traits::is_marked(current_raw) && is_dead(current)) {
        marked_list_mark_for_deletion<Identity, Traits>(current, next_word);
        current_raw = next_word(current).load(std::memory_order_acquire);
      }
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

// Marks `id`'s own word for logical deletion, preserving whatever successor it currently
// names — the first half of removing a node, done once before any splice is attempted.
// Retries against concurrent inserts that change the successor while leaving mark unset;
// a no-op (mark already set) if another thread got there first.
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

}  // namespace pskiplist
