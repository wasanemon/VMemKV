#include <doctest/doctest.h>

#include <cstdint>
#include <pskiplist/pskiplist.hpp>
#include <vector>

using pskiplist::NodeState;
using pskiplist::PackedValue;
using pskiplist::PSkipList;

TEST_CASE("PackedValue round-trips state and payload") {
  const auto v = PackedValue::live(12345);
  CHECK(v.state() == NodeState::kLive);
  CHECK(v.payload() == 12345);

  const auto tombstoned = v.with_state(NodeState::kTombstonedLinked);
  CHECK(tombstoned.state() == NodeState::kTombstonedLinked);
  CHECK(tombstoned.payload() == 12345);
}

TEST_CASE("put then get round-trips the value") {
  PSkipList<int> skiplist(16);
  REQUIRE(skiplist.put(42, 100));
  CHECK(skiplist.get(42) == 100);
}

TEST_CASE("put on an existing key updates in place, not a duplicate") {
  PSkipList<int> skiplist(16);
  REQUIRE(skiplist.put(42, 100));
  REQUIRE(skiplist.put(42, 200));
  CHECK(skiplist.get(42) == 200);

  std::vector<int> seen;
  skiplist.scan(0, 1000, [&](int key, uint64_t) { seen.push_back(key); });
  CHECK(seen == std::vector<int>{42});
}

TEST_CASE("put after remove resurrects the same slot") {
  PSkipList<int> skiplist(16);
  REQUIRE(skiplist.put(42, 100));
  REQUIRE(skiplist.remove(42));
  REQUIRE(skiplist.put(42, 300));
  CHECK(skiplist.get(42) == 300);

  std::vector<int> seen;
  skiplist.scan(0, 1000, [&](int key, uint64_t) { seen.push_back(key); });
  CHECK(seen == std::vector<int>{42});
}

TEST_CASE("scan visits live keys in ascending order within [begin, end)") {
  PSkipList<int> skiplist(64);
  for (int key : {50, 10, 30, 20, 40, 5, 60}) {
    REQUIRE(skiplist.put(key, static_cast<uint64_t>(key) * 10));
  }

  std::vector<int> seen;
  skiplist.scan(10, 50, [&](int key, uint64_t val) {
    seen.push_back(key);
    CHECK(val == static_cast<uint64_t>(key) * 10);
  });
  CHECK(seen == std::vector<int>{10, 20, 30, 40});
}

TEST_CASE("scan skips tombstoned entries") {
  PSkipList<int> skiplist(64);
  for (int key : {10, 20, 30, 40}) {
    REQUIRE(skiplist.put(key, static_cast<uint64_t>(key)));
  }
  REQUIRE(skiplist.remove(20));

  std::vector<int> seen;
  skiplist.scan(0, 1000, [&](int key, uint64_t) { seen.push_back(key); });
  CHECK(seen == std::vector<int>{10, 30, 40});
}

TEST_CASE("put after remove and reclaim of a different key reuses the slot") {
  PSkipList<int> skiplist(1);
  REQUIRE(skiplist.put(1, 10));
  REQUIRE(skiplist.remove(1));
  skiplist.reclaim();
  CHECK(skiplist.put(2, 20));
  CHECK(skiplist.get(2) == 20);
  CHECK_FALSE(skiplist.get(1).has_value());
}
