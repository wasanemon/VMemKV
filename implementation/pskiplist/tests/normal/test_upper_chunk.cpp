#include <doctest/doctest.h>

#include <pskiplist/pskiplist.hpp>

using pskiplist::allocate_upper_chunk;
using pskiplist::deallocate_upper_chunk;
using pskiplist::marked_ptr_marked;
using pskiplist::marked_ptr_value;
using pskiplist::pack_marked_ptr;
using pskiplist::UpperArena;
using pskiplist::UpperChunk;

TEST_CASE("a freshly allocated chunk has its primary entry published and null, unmarked forward pointers") {
  UpperArena arena;
  UpperChunk<int> *chunk = allocate_upper_chunk(arena, 7, 42, 5);
  CHECK(chunk->primary_offset == 7);
  CHECK(chunk->primary_key == 42);
  CHECK(chunk->height == 5);
  CHECK(chunk->levels_remaining.load() == 5);
  CHECK(chunk->claimed.load() == 1);
  CHECK(chunk->live_count.load() == 1);
  CHECK(chunk->entries[0].key == 42);
  CHECK(chunk->entries[0].durable_offset == 7);
  CHECK(chunk->entries[0].published.load());
  CHECK(chunk->entries[0].live.load());
  for (int i = 1; i < UpperChunk<int>::kCapacity; ++i) {
    CHECK_FALSE(chunk->entries[i].published.load());
  }
  for (int level = 0; level < 5; ++level) {
    const uint64_t raw = chunk->forwards[level].load();
    CHECK(marked_ptr_value<UpperChunk<int>>(raw) == nullptr);
    CHECK_FALSE(marked_ptr_marked(raw));
  }
  deallocate_upper_chunk(arena, chunk);
}

TEST_CASE("forward pointers at each level are independently settable") {
  UpperArena arena;
  UpperChunk<int> *a = allocate_upper_chunk(arena, 1, 10, 3);
  UpperChunk<int> *b = allocate_upper_chunk(arena, 2, 20, 1);

  a->forwards[0].store(pack_marked_ptr(b, false));
  a->forwards[2].store(pack_marked_ptr(a, false));  // a level can point back to the chunk itself

  CHECK(marked_ptr_value<UpperChunk<int>>(a->forwards[0].load()) == b);
  CHECK(marked_ptr_value<UpperChunk<int>>(a->forwards[1].load()) == nullptr);
  CHECK(marked_ptr_value<UpperChunk<int>>(a->forwards[2].load()) == a);

  deallocate_upper_chunk(arena, a);
  deallocate_upper_chunk(arena, b);
}

TEST_CASE("the mark bit is independent of the pointer value") {
  UpperArena arena;
  UpperChunk<int> *a = allocate_upper_chunk(arena, 1, 10, 1);
  UpperChunk<int> *b = allocate_upper_chunk(arena, 2, 20, 1);

  a->forwards[0].store(pack_marked_ptr(b, true));

  CHECK(marked_ptr_value<UpperChunk<int>>(a->forwards[0].load()) == b);
  CHECK(marked_ptr_marked(a->forwards[0].load()));

  deallocate_upper_chunk(arena, a);
  deallocate_upper_chunk(arena, b);
}

TEST_CASE("a height-1 chunk works the same as any other height") {
  UpperArena arena;
  UpperChunk<int> *chunk = allocate_upper_chunk(arena, 99, 42, 1);
  CHECK(chunk->height == 1);
  CHECK(chunk->levels_remaining.load() == 1);
  CHECK(marked_ptr_value<UpperChunk<int>>(chunk->forwards[0].load()) == nullptr);
  deallocate_upper_chunk(arena, chunk);
}

TEST_CASE("additional entries can be claimed and published into a chunk's remaining capacity") {
  UpperArena arena;
  UpperChunk<int> *chunk = allocate_upper_chunk(arena, 0, 100, 1);

  const int slot = chunk->claimed.fetch_add(1);
  REQUIRE(slot < UpperChunk<int>::kCapacity);
  chunk->live_count.fetch_add(1);
  chunk->entries[slot].key = 105;
  chunk->entries[slot].durable_offset = 1;
  chunk->entries[slot].live.store(true);
  chunk->entries[slot].published.store(true);

  CHECK(chunk->entries[1].key == 105);
  CHECK(chunk->entries[1].durable_offset == 1);
  CHECK(chunk->entries[1].published.load());
  CHECK(chunk->live_count.load() == 2);

  deallocate_upper_chunk(arena, chunk);
}

TEST_CASE("claiming beyond kCapacity is detectable by the caller") {
  UpperArena arena;
  UpperChunk<int> *chunk = allocate_upper_chunk(arena, 0, 100, 1);
  chunk->claimed.store(UpperChunk<int>::kCapacity);

  const int slot = chunk->claimed.fetch_add(1);
  CHECK(slot >= UpperChunk<int>::kCapacity);

  deallocate_upper_chunk(arena, chunk);
}
