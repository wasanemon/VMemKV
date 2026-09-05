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
// clustered around one region of key space, instead of one node per key -- see
// high_level_design.md 2.4.1 for why. No splits or merges: `primary_key`/`primary_offset` fix
// a chunk's outer-linked-list position for its entire lifetime; a full chunk's overflow just
// gets a new chunk spliced in beside it, subdividing its territory going forward.
template <typename Key>
struct UpperChunk {
  static constexpr int kCapacity = 32;

  struct Entry {
    Key key{};
    Offset durable_offset{kNullOffset};
    // published separates slot claim from visibility: readers trust an entry only once they
    // observe published == true (acquire). live tracks removal; slots are never freed individually.
    std::atomic<bool> published{false};
    std::atomic<bool> live{false};
  };

  // Fixed at chunk creation for its whole lifetime -- a routing coordinate, not a liveness claim.
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

// Chunks are never individually destructed (every field is trivially destructible); releasing
// the owning block's refcount is the entire cleanup needed (see UpperArena::release()).
template <typename Key>
inline void deallocate_upper_chunk(UpperArena &arena, UpperChunk<Key> *chunk) {
  arena.release(chunk->owning_block);
}

}  // namespace pskiplist
