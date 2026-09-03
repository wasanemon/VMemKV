// Concurrency stress tests for pskiplist::PSkipList.
//
// high_level_design.md 2.3/3 章 (physical reclaim and the epoch/EBR system) are not
// implemented yet -- see pskiplist.hpp's header comment. That specifically means no offset is
// ever reused, so there is no ABA/use-after-free hazard to guard against yet, and it is already
// meaningful to stress-test the put/remove/get CAS logic (4.1 章) under real concurrency. These
// tests are expected to be run under ThreadSanitizer as well as normally (6 章).
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <pskiplist/pskiplist.hpp>
#include <random>
#include <thread>
#include <vector>

using pskiplist::PSkipList;

TEST_CASE("concurrent put: disjoint key ranges from multiple threads all land correctly") {
  constexpr int kThreads = 8;
  constexpr int kKeysPerThread = 500;
  PSkipList<int> skiplist(kThreads * kKeysPerThread);

  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&skiplist, t] {
      const int base = t * kKeysPerThread;
      for (int i = 0; i < kKeysPerThread; ++i) {
        const int key = base + i;
        REQUIRE(skiplist.put(key, static_cast<uint64_t>(key) * 2));
      }
    });
  }
  for (auto &worker : workers) worker.join();

  for (int t = 0; t < kThreads; ++t) {
    const int base = t * kKeysPerThread;
    for (int i = 0; i < kKeysPerThread; ++i) {
      const int key = base + i;
      const auto value = skiplist.get(key);
      REQUIRE(value.has_value());
      CHECK(*value == static_cast<uint64_t>(key) * 2);
    }
  }

  // The concurrent inserts must not have corrupted level 0's ordering invariant (2.2 章).
  int previous = -1;
  int count = 0;
  skiplist.scan(0, kThreads * kKeysPerThread, [&](int key, uint64_t) {
    CHECK(key > previous);
    previous = key;
    ++count;
  });
  CHECK(count == kThreads * kKeysPerThread);
}

// Many threads race put()/remove() on a small, shared set of keys -- this is the scenario
// high_level_design.md 4.1 章's CAS arbitration (resurrect vs. physical unlink, concurrent
// updates vs. remove) exists for. There is no single correct final state (it depends on
// scheduling), so this test checks invariants instead of exact values: no crash/UB, and
// get()/scan() stay mutually consistent throughout.
TEST_CASE("concurrent put/remove on shared keys: no corruption, get() and scan() stay consistent") {
  constexpr int kThreads = 8;
  constexpr int kSharedKeys = 8;
  constexpr int kIterationsPerThread = 2000;
  // Capacity is deliberately generous, not tight: repeatedly racing to insert an
  // absent shared key means every losing thread's pre-allocated node is leaked
  // (no reclaim yet, 2.3 章 / pskiplist.hpp's header comment) -- this test is about
  // get()/scan() consistency under contention, not about exercising capacity
  // exhaustion (that is covered separately in test_basic.cpp).
  PSkipList<int> skiplist(kThreads * kIterationsPerThread);

  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&skiplist, &failed, t] {
      std::mt19937 rng(static_cast<unsigned>(t) + 1);
      std::uniform_int_distribution<int> key_dist(0, kSharedKeys - 1);
      std::uniform_int_distribution<int> op_dist(0, 1);
      for (int i = 0; i < kIterationsPerThread && !failed.load(std::memory_order_relaxed); ++i) {
        const int key = key_dist(rng);
        if (op_dist(rng) == 0) {
          if (!skiplist.put(key, static_cast<uint64_t>(t) * 1'000'000 + static_cast<uint64_t>(i))) {
            failed.store(true, std::memory_order_relaxed);  // Unexpected: capacity is fixed and reused.
          }
        } else {
          (void)skiplist.remove(key);  // Return value is racy by design; not checked here.
        }
      }
    });
  }
  for (auto &worker : workers) worker.join();

  CHECK_FALSE(failed.load());

  std::vector<int> scanned_keys;
  skiplist.scan(0, kSharedKeys, [&](int key, uint64_t) { scanned_keys.push_back(key); });
  for (int key = 0; key < kSharedKeys; ++key) {
    const auto value = skiplist.get(key);
    const bool in_scan = std::find(scanned_keys.begin(), scanned_keys.end(), key) != scanned_keys.end();
    CHECK(value.has_value() == in_scan);
  }
}
