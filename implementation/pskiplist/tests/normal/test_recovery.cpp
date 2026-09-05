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

TEST_CASE("an in-place update to an existing key after the last checkpoint is not recovered") {
  TempFile tmp("recover_update_not_checkpointed");
  const size_t bytes = capacity_bytes_for_nodes<int>(64);
  {
    PSkipList<int> skiplist(tmp.path(), bytes);
    REQUIRE(skiplist.put(1, 100));
    REQUIRE(skiplist.checkpoint());
    REQUIRE(skiplist.put(1, 999));  // updates the existing key — never checkpointed
  }

  PSkipList<int> reopened(tmp.path(), bytes);
  CHECK(reopened.get(1) == 100);
}

TEST_CASE("removing a checkpointed key and reusing its slot doesn't lose it if never checkpointed") {
  TempFile tmp("recover_remove_reuse_not_checkpointed");
  const size_t bytes = capacity_bytes_for_nodes<int>(64);
  {
    PSkipList<int> skiplist(tmp.path(), bytes);
    REQUIRE(skiplist.put(1, 100));
    REQUIRE(skiplist.checkpoint());
    REQUIRE(skiplist.remove(1));    // never checkpointed after this
    REQUIRE(skiplist.put(2, 200));  // may reuse key 1's freed slot — also never checkpointed
  }

  PSkipList<int> reopened(tmp.path(), bytes);
  CHECK(reopened.get(1) == 100);
  CHECK_FALSE(reopened.get(2).has_value());
}

// Regression test for checkpointed_state (durable_node.hpp): reverting `value` to
// checkpointed_value during recover() no longer implies reverting state too, now that they're
// separate fields instead of one packed word — this exercises exactly the crash window
// checkpointed_state exists to cover (remove() then put() resurrecting the same key, all after
// the last checkpoint and never re-checkpointed): the key must come back fully live with its
// original pre-removal value, not stuck half-tombstoned.
TEST_CASE("a remove-then-resurrect after the last checkpoint reverts to the pre-removal value") {
  TempFile tmp("recover_remove_then_resurrect_not_checkpointed");
  const size_t bytes = capacity_bytes_for_nodes<int>(64);
  {
    PSkipList<int> skiplist(tmp.path(), bytes);
    REQUIRE(skiplist.put(1, 100));
    REQUIRE(skiplist.checkpoint());
    REQUIRE(skiplist.remove(1));
    REQUIRE(skiplist.put(1, 999));  // resurrects the same key — never checkpointed after this
  }

  PSkipList<int> reopened(tmp.path(), bytes);
  const auto val = reopened.get(1);
  REQUIRE(val.has_value());
  CHECK(*val == 100);           // reverted to the checkpointed (pre-removal) value, fully live
  CHECK(reopened.put(1, 200));  // still a normal, live, updatable key
  CHECK(reopened.get(1) == 200);
}

TEST_CASE("data checkpointed under a high epoch survives a later session with a lower one") {
  // epoch_ is an in-process counter that starts at 0 every time the file is reopened; it
  // must resume from the manifest's published epoch, not restart from zero, or a later
  // session's checkpoint() — which necessarily publishes its own low epoch — makes
  // recover() wrongly trim data an earlier session already durably checkpointed.
  TempFile tmp("recover_epoch_continuity");
  const size_t bytes = capacity_bytes_for_nodes<int>(64);
  {
    PSkipList<int> skiplist(tmp.path(), bytes);
    for (int i = 0; i < 5; ++i) {
      REQUIRE(skiplist.checkpoint());
    }
    REQUIRE(skiplist.put(1, 100));  // stamped with this session's now-advanced epoch
    REQUIRE(skiplist.checkpoint());
  }
  {
    PSkipList<int> reopened(tmp.path(), bytes);
    REQUIRE(reopened.get(1) == 100);
    REQUIRE(reopened.put(2, 200));  // this session's epoch_ restarts at 0
    REQUIRE(reopened.checkpoint());
  }

  PSkipList<int> twice_reopened(tmp.path(), bytes);
  CHECK(twice_reopened.get(1) == 100);
  CHECK(twice_reopened.get(2) == 200);
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

// Large enough that the geometric level distribution (p=1/4) all but guarantees many nodes
// reach level 2+, exercising recover()'s upper-level rebuild (§2.4) over a non-trivial
// chain rather than just the 1-2 node cases above.
TEST_CASE("reopening a large corpus recovers every live key and stays queryable") {
  TempFile tmp("recover_large_corpus");
  constexpr int kCount = 2000;
  const size_t bytes = capacity_bytes_for_nodes<int>(kCount + 1);
  {
    PSkipList<int> skiplist(tmp.path(), bytes);
    for (int i = 0; i < kCount; ++i) {
      REQUIRE(skiplist.put(i, static_cast<uint64_t>(i)));
    }
    for (int i = 0; i < kCount; i += 3) {
      REQUIRE(skiplist.remove(i));
    }
    REQUIRE(skiplist.checkpoint());
  }

  PSkipList<int> reopened(tmp.path(), bytes);
  for (int i = 0; i < kCount; ++i) {
    if (i % 3 == 0) {
      CHECK_FALSE(reopened.get(i).has_value());
    } else {
      CHECK(reopened.get(i) == static_cast<uint64_t>(i));
    }
  }
  std::vector<int> seen;
  reopened.scan(0, kCount, [&](int key, uint64_t value) {
    seen.push_back(key);
    CHECK(value == static_cast<uint64_t>(key));
  });
  CHECK(seen.size() == kCount - (kCount + 2) / 3);
  for (size_t i = 1; i < seen.size(); ++i) {
    CHECK(seen[i - 1] < seen[i]);
  }

  // The rebuilt upper levels must stay correct under further mutation, not just be
  // queryable once right after recovery.
  REQUIRE(reopened.put(kCount, 999));
  CHECK(reopened.get(kCount) == 999);
  REQUIRE(reopened.remove(1));
  CHECK_FALSE(reopened.get(1).has_value());
}
