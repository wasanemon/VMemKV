// Single-threaded functional correctness tests for pskiplist::PSkipList.
// See ../high_level_design.md 6 章 for the overall test strategy this suite is part of
// (concurrency-safety and crash-consistency are covered separately; this file only exercises
// sequential behavior).
#include <doctest/doctest.h>

#include <cstdint>
#include <pskiplist/pskiplist.hpp>
#include <vector>

using pskiplist::NodeState;
using pskiplist::PackedValue;
using pskiplist::PSkipList;

TEST_CASE("PackedValue: state and payload round-trip through the same 64-bit word") {
  const auto v = PackedValue::live(12345);
  CHECK(v.state() == NodeState::kLive);
  CHECK(v.payload() == 12345);

  const auto tombstoned = v.with_state(NodeState::kTombstonedLinked);
  CHECK(tombstoned.state() == NodeState::kTombstonedLinked);
  CHECK(tombstoned.payload() == 12345);  // Payload survives a state-only change.

  const auto unlinked = tombstoned.with_state(NodeState::kTombstonedUnlinked);
  CHECK(unlinked.state() == NodeState::kTombstonedUnlinked);
  CHECK(unlinked.payload() == 12345);
}

TEST_CASE("get: missing key returns nullopt") {
  PSkipList<int> skiplist(16);
  CHECK_FALSE(skiplist.get(42).has_value());
}

TEST_CASE("put then get: round-trips the value") {
  PSkipList<int> skiplist(16);
  REQUIRE(skiplist.put(42, 100));
  const auto value = skiplist.get(42);
  REQUIRE(value.has_value());
  CHECK(*value == 100);
}

TEST_CASE("put on an existing key updates the value in place, not a duplicate") {
  PSkipList<int> skiplist(16);
  REQUIRE(skiplist.put(42, 100));
  REQUIRE(skiplist.put(42, 200));
  const auto value = skiplist.get(42);
  REQUIRE(value.has_value());
  CHECK(*value == 200);

  std::vector<std::pair<int, uint64_t>> seen;
  skiplist.scan(0, 1000, [&](int key, uint64_t val) { seen.emplace_back(key, val); });
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].first == 42);
  CHECK(seen[0].second == 200);
}

TEST_CASE("remove: returns false for a key that was never present") {
  PSkipList<int> skiplist(16);
  CHECK_FALSE(skiplist.remove(42));
}

TEST_CASE("remove: returns true once, then false for a second removal of the same key") {
  PSkipList<int> skiplist(16);
  REQUIRE(skiplist.put(42, 100));
  CHECK(skiplist.remove(42));
  CHECK_FALSE(skiplist.remove(42));
}

TEST_CASE("get: returns nullopt after remove") {
  PSkipList<int> skiplist(16);
  REQUIRE(skiplist.put(42, 100));
  REQUIRE(skiplist.remove(42));
  CHECK_FALSE(skiplist.get(42).has_value());
}

TEST_CASE("put after remove resurrects the same slot rather than creating a duplicate") {
  PSkipList<int> skiplist(16);
  REQUIRE(skiplist.put(42, 100));
  REQUIRE(skiplist.remove(42));
  REQUIRE(skiplist.put(42, 300));

  const auto value = skiplist.get(42);
  REQUIRE(value.has_value());
  CHECK(*value == 300);

  // high_level_design.md 4.1 章: put() on a tombstoned-linked node reuses that node's slot
  // instead of allocating a fresh one, so exactly one live entry for the key must exist.
  std::vector<int> seen_keys;
  skiplist.scan(0, 1000, [&](int key, uint64_t) { seen_keys.push_back(key); });
  REQUIRE(seen_keys.size() == 1);
  CHECK(seen_keys[0] == 42);
}

TEST_CASE("scan: visits live keys in ascending order within [begin, end)") {
  PSkipList<int> skiplist(64);
  for (int key : {50, 10, 30, 20, 40, 5, 60}) {
    REQUIRE(skiplist.put(key, static_cast<uint64_t>(key) * 10));
  }

  std::vector<int> seen;
  skiplist.scan(10, 50, [&](int key, uint64_t val) {
    seen.push_back(key);
    CHECK(val == static_cast<uint64_t>(key) * 10);
  });

  const std::vector<int> expected = {10, 20, 30, 40};  // 50 and 5/60 excluded by [10, 50).
  CHECK(seen == expected);
}

TEST_CASE("scan: skips tombstoned entries") {
  PSkipList<int> skiplist(64);
  for (int key : {10, 20, 30, 40}) {
    REQUIRE(skiplist.put(key, static_cast<uint64_t>(key)));
  }
  REQUIRE(skiplist.remove(20));

  std::vector<int> seen;
  skiplist.scan(0, 1000, [&](int key, uint64_t) { seen.push_back(key); });
  const std::vector<int> expected = {10, 30, 40};
  CHECK(seen == expected);
}

TEST_CASE("put: fails once capacity is exhausted, without corrupting existing entries") {
  PSkipList<int> skiplist(3);  // 3 usable node slots (head/tail are separate).
  REQUIRE(skiplist.put(1, 10));
  REQUIRE(skiplist.put(2, 20));
  REQUIRE(skiplist.put(3, 30));
  CHECK_FALSE(skiplist.put(4, 40));  // Capacity exhausted (2.6 章): reports failure, no crash.

  CHECK(skiplist.get(1) == 10);
  CHECK(skiplist.get(2) == 20);
  CHECK(skiplist.get(3) == 30);
  CHECK_FALSE(skiplist.get(4).has_value());
}

TEST_CASE("put: updating an existing key never consumes new capacity") {
  PSkipList<int> skiplist(1);
  REQUIRE(skiplist.put(1, 10));
  CHECK_FALSE(skiplist.put(2, 20));  // No capacity left for a second distinct key.
  for (int i = 0; i < 10; ++i) {
    REQUIRE(skiplist.put(1, static_cast<uint64_t>(i)));  // Updates reuse the same slot.
  }
  CHECK(skiplist.get(1) == 9);
}
