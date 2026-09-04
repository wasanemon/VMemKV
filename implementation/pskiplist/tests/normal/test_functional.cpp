#include <doctest/doctest.h>

#include <cstdint>
#include <pskiplist/pskiplist.hpp>
#include <vector>

#include "support/temp_file.hpp"

using pskiplist::PSkipList;
using pskiplist_test::capacity_bytes_for_nodes;
using pskiplist_test::TempFile;

// Regression test: two earlier designs each reserved part of `value`'s bit space for node
// state (first 2 packed bits, then 2 reserved sentinel values) and both silently mishandled
// some legitimate payloads. Now that state lives in its own field (durable_node.hpp), `Value`
// has no reserved values at all -- every uint64_t, including all-1-bits, must round-trip.
TEST_CASE("put/get round-trips full-width payloads, including all-1-bits") {
  TempFile tmp("put_get_full_width_payload");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(16));
  const uint64_t kHighBitsSet = 0xC000000000000001ULL;
  REQUIRE(skiplist.put(1, kHighBitsSet));
  CHECK(skiplist.get(1) == kHighBitsSet);
  REQUIRE(skiplist.put(1, ~uint64_t{0}));
  CHECK(skiplist.get(1) == ~uint64_t{0});
}

// Value is a real template parameter (mirrors Key) backed by a seqlock, not a single CAS'd
// word -- any trivially-copyable, default-constructible struct works, not just uint64_t.
TEST_CASE("put/get round-trips a custom struct Value") {
  struct Pair {
    uint64_t a = 0;
    uint32_t b = 0;
    auto operator==(const Pair &) const -> bool = default;
  };
  TempFile tmp("put_get_struct_value");
  PSkipList<int, Pair> skiplist(tmp.path(), capacity_bytes_for_nodes<int, Pair>(16));
  REQUIRE(skiplist.put(1, Pair{42, 7}));
  const auto found = skiplist.get(1);
  REQUIRE(found.has_value());
  CHECK(*found == Pair{42, 7});
  REQUIRE(skiplist.put(1, Pair{100, 200}));
  CHECK(skiplist.get(1) == Pair{100, 200});
  REQUIRE(skiplist.remove(1));
  CHECK_FALSE(skiplist.get(1).has_value());
}

TEST_CASE("put then get round-trips the value") {
  TempFile tmp("put_get");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(16));
  REQUIRE(skiplist.put(42, 100));
  CHECK(skiplist.get(42) == 100);
}

TEST_CASE("put on an existing key updates in place, not a duplicate") {
  TempFile tmp("put_update");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(16));
  REQUIRE(skiplist.put(42, 100));
  REQUIRE(skiplist.put(42, 200));
  CHECK(skiplist.get(42) == 200);

  std::vector<int> seen;
  skiplist.scan(0, 1000, [&](int key, uint64_t) { seen.push_back(key); });
  CHECK(seen == std::vector<int>{42});
}

TEST_CASE("put after remove resurrects the same slot") {
  TempFile tmp("put_after_remove");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(16));
  REQUIRE(skiplist.put(42, 100));
  REQUIRE(skiplist.remove(42));
  REQUIRE(skiplist.put(42, 300));
  CHECK(skiplist.get(42) == 300);

  std::vector<int> seen;
  skiplist.scan(0, 1000, [&](int key, uint64_t) { seen.push_back(key); });
  CHECK(seen == std::vector<int>{42});
}

TEST_CASE("scan visits live keys in ascending order within [begin, end)") {
  TempFile tmp("scan_order");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(64));
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
  TempFile tmp("scan_skips_tombstoned");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(64));
  for (int key : {10, 20, 30, 40}) {
    REQUIRE(skiplist.put(key, static_cast<uint64_t>(key)));
  }
  REQUIRE(skiplist.remove(20));

  std::vector<int> seen;
  skiplist.scan(0, 1000, [&](int key, uint64_t) { seen.push_back(key); });
  CHECK(seen == std::vector<int>{10, 30, 40});
}

TEST_CASE("put after remove, checkpoint, and reclaim of a different key reuses the slot") {
  // Physical unlink is deferred until checkpoint() confirms the removal is durable (crash-
  // consistency: reusing the slot any earlier could destroy a still-checkpointed value a
  // crash-then-recover should be able to fall back to) — so checkpoint() must run between
  // remove() and reclaim() for the slot to become reusable.
  TempFile tmp("reclaim_reuses_slot");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(1));
  REQUIRE(skiplist.put(1, 10));
  REQUIRE(skiplist.remove(1));
  REQUIRE(skiplist.checkpoint());
  skiplist.reclaim();
  CHECK(skiplist.put(2, 20));
  CHECK(skiplist.get(2) == 20);
  CHECK_FALSE(skiplist.get(1).has_value());
}
