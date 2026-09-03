#include <doctest/doctest.h>

#include <cstdint>
#include <pskiplist/pskiplist.hpp>
#include <stdexcept>

#include "support/temp_file.hpp"

using pskiplist::PSkipList;
using pskiplist_test::capacity_bytes_for_nodes;
using pskiplist_test::TempFile;

TEST_CASE("get on a missing key returns nullopt") {
  TempFile tmp("get_missing");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(16));
  CHECK_FALSE(skiplist.get(42).has_value());
}

TEST_CASE("remove on a key that was never present returns false") {
  TempFile tmp("remove_missing");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(16));
  CHECK_FALSE(skiplist.remove(42));
}

TEST_CASE("remove twice on the same key returns false the second time") {
  TempFile tmp("remove_twice");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(16));
  REQUIRE(skiplist.put(42, 100));
  CHECK(skiplist.remove(42));
  CHECK_FALSE(skiplist.remove(42));
}

TEST_CASE("get returns nullopt after remove") {
  TempFile tmp("get_after_remove");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(16));
  REQUIRE(skiplist.put(42, 100));
  REQUIRE(skiplist.remove(42));
  CHECK_FALSE(skiplist.get(42).has_value());
}

TEST_CASE("put fails once capacity is exhausted, without corrupting existing entries") {
  TempFile tmp("capacity_exhausted");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(3));
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
  TempFile tmp("update_no_new_capacity");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(1));
  REQUIRE(skiplist.put(1, 10));
  CHECK_FALSE(skiplist.put(2, 20));
  for (int i = 0; i < 10; ++i) {
    REQUIRE(skiplist.put(1, static_cast<uint64_t>(i)));
  }
  CHECK(skiplist.get(1) == 9);
}

TEST_CASE("constructing with capacity too small for the head/tail sentinels throws") {
  TempFile tmp("capacity_too_small");
  CHECK_THROWS_AS((PSkipList<int>(tmp.path(), 1)), std::invalid_argument);
}
