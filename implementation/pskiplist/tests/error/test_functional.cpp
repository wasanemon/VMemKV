#include <doctest/doctest.h>

#include <cstdint>
#include <pskiplist/pskiplist.hpp>

using pskiplist::PSkipList;

TEST_CASE("get on a missing key returns nullopt") {
  PSkipList<int> skiplist(16);
  CHECK_FALSE(skiplist.get(42).has_value());
}

TEST_CASE("remove on a key that was never present returns false") {
  PSkipList<int> skiplist(16);
  CHECK_FALSE(skiplist.remove(42));
}

TEST_CASE("remove twice on the same key returns false the second time") {
  PSkipList<int> skiplist(16);
  REQUIRE(skiplist.put(42, 100));
  CHECK(skiplist.remove(42));
  CHECK_FALSE(skiplist.remove(42));
}

TEST_CASE("get returns nullopt after remove") {
  PSkipList<int> skiplist(16);
  REQUIRE(skiplist.put(42, 100));
  REQUIRE(skiplist.remove(42));
  CHECK_FALSE(skiplist.get(42).has_value());
}

TEST_CASE("put fails once capacity is exhausted, without corrupting existing entries") {
  PSkipList<int> skiplist(3);
  REQUIRE(skiplist.put(1, 10));
  REQUIRE(skiplist.put(2, 20));
  REQUIRE(skiplist.put(3, 30));
  CHECK_FALSE(skiplist.put(4, 40));

  CHECK(skiplist.get(1) == 10);
  CHECK(skiplist.get(2) == 20);
  CHECK(skiplist.get(3) == 30);
  CHECK_FALSE(skiplist.get(4).has_value());
}

TEST_CASE("updating an existing key never consumes new capacity") {
  PSkipList<int> skiplist(1);
  REQUIRE(skiplist.put(1, 10));
  CHECK_FALSE(skiplist.put(2, 20));
  for (int i = 0; i < 10; ++i) {
    REQUIRE(skiplist.put(1, static_cast<uint64_t>(i)));
  }
  CHECK(skiplist.get(1) == 9);
}
