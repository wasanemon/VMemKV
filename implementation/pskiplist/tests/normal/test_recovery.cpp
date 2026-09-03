#include <doctest/doctest.h>

#include <cstdint>
#include <pskiplist/pskiplist.hpp>
#include <vector>

#include "support/temp_file.hpp"

using pskiplist::PSkipList;
using pskiplist_test::capacity_bytes_for_nodes;
using pskiplist_test::TempFile;

TEST_CASE("reopening after checkpoint() recovers all live keys") {
  TempFile tmp("recover_all_keys");
  const size_t bytes = capacity_bytes_for_nodes<int>(64);
  {
    PSkipList<int> skiplist(tmp.path(), bytes);
    for (int i = 0; i < 10; ++i) {
      REQUIRE(skiplist.put(i, static_cast<uint64_t>(i) * 10));
    }
    REQUIRE(skiplist.checkpoint());
  }

  PSkipList<int> reopened(tmp.path(), bytes);
  for (int i = 0; i < 10; ++i) {
    CHECK(reopened.get(i) == static_cast<uint64_t>(i) * 10);
  }
  std::vector<int> seen;
  reopened.scan(0, 10, [&](int key, uint64_t) { seen.push_back(key); });
  CHECK(seen.size() == 10);
}

TEST_CASE("writes after the last checkpoint are not recovered") {
  TempFile tmp("recover_only_checkpointed");
  const size_t bytes = capacity_bytes_for_nodes<int>(64);
  {
    PSkipList<int> skiplist(tmp.path(), bytes);
    REQUIRE(skiplist.put(1, 100));
    REQUIRE(skiplist.checkpoint());
    REQUIRE(skiplist.put(2, 200));  // never checkpointed — simulates a crash right here
  }

  PSkipList<int> reopened(tmp.path(), bytes);
  CHECK(reopened.get(1) == 100);
  CHECK_FALSE(reopened.get(2).has_value());
}

TEST_CASE("reopening a file with content but no manifest starts fresh") {
  TempFile tmp("recover_no_manifest");
  const size_t bytes = capacity_bytes_for_nodes<int>(64);
  {
    PSkipList<int> skiplist(tmp.path(), bytes);
    REQUIRE(skiplist.put(1, 100));
    // No checkpoint() at all — the file has content, but nothing was ever published.
  }

  PSkipList<int> reopened(tmp.path(), bytes);
  CHECK_FALSE(reopened.get(1).has_value());
  std::vector<int> seen;
  reopened.scan(0, 1000, [&](int key, uint64_t) { seen.push_back(key); });
  CHECK(seen.empty());
}

TEST_CASE("recovery frees removed keys' capacity for reuse") {
  TempFile tmp("recover_frees_removed_capacity");
  const size_t bytes = capacity_bytes_for_nodes<int>(1);
  {
    PSkipList<int> skiplist(tmp.path(), bytes);
    REQUIRE(skiplist.put(1, 10));
    REQUIRE(skiplist.remove(1));
    REQUIRE(skiplist.checkpoint());
  }

  PSkipList<int> reopened(tmp.path(), bytes);
  CHECK_FALSE(reopened.get(1).has_value());
  CHECK(reopened.put(2, 20));  // the single usable slot was freed by removing key 1
  CHECK(reopened.get(2) == 20);
}

TEST_CASE("recovered state can be extended with further writes and checkpoints") {
  TempFile tmp("recover_then_extend");
  const size_t bytes = capacity_bytes_for_nodes<int>(64);
  {
    PSkipList<int> skiplist(tmp.path(), bytes);
    REQUIRE(skiplist.put(1, 100));
    REQUIRE(skiplist.checkpoint());
  }
  {
    PSkipList<int> reopened(tmp.path(), bytes);
    REQUIRE(reopened.get(1) == 100);
    REQUIRE(reopened.put(2, 200));
    REQUIRE(reopened.checkpoint());
  }

  PSkipList<int> twice_reopened(tmp.path(), bytes);
  CHECK(twice_reopened.get(1) == 100);
  CHECK(twice_reopened.get(2) == 200);
}
