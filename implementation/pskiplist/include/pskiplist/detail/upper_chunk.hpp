#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

#include "pskiplist/detail/marked_offset.hpp"
#include "pskiplist/detail/marked_pointer.hpp"
#include "pskiplist/detail/upper_arena.hpp"

namespace pskiplist {

// A volatile, DRAM-only skip-list node holding up to kCapacity (key, durable_offset) entries
// clustered around the same region of key space, instead of the classic "one node per key"
// skip-list design: a plain one-key-per-node skip list pays one random, likely cache-cold
// memory access per hop, which dominates Get/Scan cost at scale (measured: ~40+ such accesses
// per Get at 5M keys, even after moving the node's own memory into an arena -- see
// upper_arena.hpp -- since arena-allocation order only correlates with key order for a sorted
// bulk load, not for steady-state random-order inserts). Packing several nearby keys into one
// arena-resident, contiguous chunk turns most of those hops into a single cache-friendly linear
// scan over already-fetched memory instead.
//
// No splits or merges: `primary_key`/`primary_offset` fix a chunk's position in the outer
// linked list for its entire lifetime (see skiplist.hpp's cascade_upper_levels(), which compares
// chunks by `primary_key` exactly as it used to compare single-key nodes). Every key whose
// value falls in this chunk's "territory" -- between its primary_key and the next chunk's --
// is packed into one of its remaining entry slots via a lock-free claim (try_insert_into_chunk()
// in skiplist.hpp), no linked-list mutation needed. Once a chunk's capacity is exhausted, the
// next key in its territory instead gets its own brand new chunk, spliced in via the same
// CAS-based per-level insertion a fresh chunk always uses -- which incidentally subdivides the
// full chunk's territory going forward. Older entries already packed into the full chunk are
// never rebalanced or moved. This means chunk occupancy is uneven over time under heavy
// churn -- an accepted tradeoff for never needing an in-place split/merge algorithm.
template <typename Key>
struct UpperChunk {
  static constexpr int kCapacity = 32;

  struct Entry {
    Key key{};
    Offset durable_offset{kNullOffset};
    // published: separates a slot's claim (an atomic fetch_add on `claimed`) from making it
    // visible to readers -- a reader only trusts an entry once it observes published == true
    // (acquire); by that point the plain (non-atomic) key/durable_offset writes that happened
    // before the release store are guaranteed visible. `live` starts false and is set true only
    // once published; it flips back to false on removal but the slot itself is never reclaimed
    // individually -- see UpperChunk's own live_count for whole-chunk reclaim.
    std::atomic<bool> published{false};
    std::atomic<bool> live{false};
  };

  // Fixed at chunk creation, for this chunk's entire lifetime, independent of whether that
  // specific key is later removed -- this is purely a routing coordinate for the outer linked
  // list, not a claim that the primary entry is still live.
  Key primary_key{};
  Offset primary_offset{kNullOffset};

  std::atomic<int> claimed{0};     // next entry-slot index to hand out, capped at kCapacity
  std::atomic<int> live_count{0};  // entries not yet removed; the chunk unlinks once this hits 0
  int height = 0;
  std::atomic<int> levels_remaining{0};
  std::atomic<UpperChunk *> next_pending{nullptr};
  UpperArena::Block *owning_block = nullptr;
  Entry entries[kCapacity];
  std::atomic<uint64_t> forwards[1];  // actually `height` entries — see allocate_upper_chunk()
};

template <typename Key>
[[nodiscard]] inline auto allocate_upper_chunk(UpperArena &arena,
                                               Offset primary_offset,
                                               const Key &primary_key,
                                               int height) -> UpperChunk<Key> * {
  using Chunk = UpperChunk<Key>;
  const size_t bytes = sizeof(Chunk) + sizeof(std::atomic<uint64_t>) * static_cast<size_t>(height - 1);
  typename UpperArena::Block *block = nullptr;
  std::byte *memory = arena.allocate(bytes, &block);
  auto *chunk = reinterpret_cast<Chunk *>(memory);
  ::new (&chunk->primary_key) Key(primary_key);
  ::new (&chunk->primary_offset) Offset(primary_offset);
  ::new (&chunk->claimed) std::atomic<int>(1);  // slot 0 is the primary entry, claimed up front
  ::new (&chunk->live_count) std::atomic<int>(1);
  chunk->height = height;
  ::new (&chunk->levels_remaining) std::atomic<int>(height);
  ::new (&chunk->next_pending) std::atomic<Chunk *>(nullptr);
  chunk->owning_block = block;
  for (int i = 0; i < Chunk::kCapacity; ++i) {
    ::new (&chunk->entries[i]) typename Chunk::Entry();
  }
  chunk->entries[0].key = primary_key;
  chunk->entries[0].durable_offset = primary_offset;
  chunk->entries[0].live.store(true, std::memory_order_relaxed);
  chunk->entries[0].published.store(true, std::memory_order_release);
  for (int level = 0; level < height; ++level) {
    ::new (&chunk->forwards[level]) std::atomic<uint64_t>(pack_marked_ptr<Chunk>(nullptr, false));
  }
  return chunk;
}

// Arena-backed chunks are never individually destructed -- every field is a trivially
// destructible atomic or a trivially-copyable Key/Offset (see concepts.hpp's SkipListKey), so
// releasing the owning block's reference count (which may, once it hits zero on an
// already-retired block, queue that block's whole buffer for freeing -- see
// UpperArena::release()) is the entire cleanup this needs.
template <typename Key>
inline void deallocate_upper_chunk(UpperArena &arena, UpperChunk<Key> *chunk) {
  arena.release(chunk->owning_block);
}

}  // namespace pskiplist
