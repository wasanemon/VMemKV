#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <pskiplist/pskiplist.hpp>
#include <random>
#include <thread>
#include <vector>

#include "support/temp_file.hpp"

using pskiplist::PSkipList;
using pskiplist_test::capacity_bytes_for_nodes;
using pskiplist_test::TempFile;

TEST_CASE("concurrent put with disjoint key ranges all land correctly") {
  constexpr int kThreads = 8;
  constexpr int kKeysPerThread = 500;
  TempFile tmp("disjoint_put");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(static_cast<size_t>(kThreads) * kKeysPerThread));

  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&skiplist, t] {
      const int base = t * kKeysPerThread;
      for (int i = 0; i < kKeysPerThread; ++i) {
        REQUIRE(skiplist.put(base + i, static_cast<uint64_t>(base + i) * 2));
      }
    });
  }
  for (auto &worker : workers) worker.join();

  for (int t = 0; t < kThreads; ++t) {
    const int base = t * kKeysPerThread;
    for (int i = 0; i < kKeysPerThread; ++i) {
      CHECK(skiplist.get(base + i) == static_cast<uint64_t>(base + i) * 2);
    }
  }

  int previous = -1;
  int count = 0;
  skiplist.scan(0, kThreads * kKeysPerThread, [&](int key, uint64_t) {
    CHECK(key > previous);
    previous = key;
    ++count;
  });
  CHECK(count == kThreads * kKeysPerThread);
}

TEST_CASE("concurrent put/remove on shared keys keeps get() and scan() consistent") {
  constexpr int kThreads = 8;
  constexpr int kSharedKeys = 8;
  constexpr int kIterationsPerThread = 2000;
  TempFile tmp("shared_keys");
  PSkipList<int> skiplist(tmp.path(),
                          capacity_bytes_for_nodes<int>(static_cast<size_t>(kThreads) * kIterationsPerThread));

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
            failed.store(true, std::memory_order_relaxed);
          }
        } else {
          (void)skiplist.remove(key);
        }
      }
    });
  }
  for (auto &worker : workers) worker.join();

  CHECK_FALSE(failed.load());

  std::vector<int> scanned_keys;
  skiplist.scan(0, kSharedKeys, [&](int key, uint64_t) { scanned_keys.push_back(key); });
  for (int key = 0; key < kSharedKeys; ++key) {
    const bool in_scan = std::find(scanned_keys.begin(), scanned_keys.end(), key) != scanned_keys.end();
    CHECK(skiplist.get(key).has_value() == in_scan);
  }
}

TEST_CASE("reclaim() running concurrently with put/remove/get stays consistent") {
  constexpr int kThreads = 8;
  constexpr int kSharedKeys = 8;
  constexpr int kIterationsPerThread = 3000;
  TempFile tmp("reclaim_concurrent");
  PSkipList<int> skiplist(tmp.path(),
                          capacity_bytes_for_nodes<int>(static_cast<size_t>(kThreads) * kIterationsPerThread));

  std::atomic<bool> stop{false};
  std::thread reclaimer([&skiplist, &stop] {
    while (!stop.load(std::memory_order_relaxed)) {
      skiplist.reclaim();
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });

  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&skiplist, &failed, t] {
      std::mt19937 rng(static_cast<unsigned>(t) + 1);
      std::uniform_int_distribution<int> key_dist(0, kSharedKeys - 1);
      std::uniform_int_distribution<int> op_dist(0, 2);
      for (int i = 0; i < kIterationsPerThread && !failed.load(std::memory_order_relaxed); ++i) {
        const int key = key_dist(rng);
        switch (op_dist(rng)) {
          case 0:
            if (!skiplist.put(key, static_cast<uint64_t>(t) * 1'000'000 + static_cast<uint64_t>(i))) {
              failed.store(true, std::memory_order_relaxed);
            }
            break;
          case 1:
            (void)skiplist.remove(key);
            break;
          default:
            (void)skiplist.get(key);
            break;
        }
      }
    });
  }
  for (auto &worker : workers) worker.join();
  stop.store(true, std::memory_order_relaxed);
  reclaimer.join();

  CHECK_FALSE(failed.load());

  skiplist.reclaim();
  std::vector<int> scanned_keys;
  skiplist.scan(0, kSharedKeys, [&](int key, uint64_t) { scanned_keys.push_back(key); });
  for (int key = 0; key < kSharedKeys; ++key) {
    const bool in_scan = std::find(scanned_keys.begin(), scanned_keys.end(), key) != scanned_keys.end();
    CHECK(skiplist.get(key).has_value() == in_scan);
  }
}

TEST_CASE("concurrent remove of a dense adjacent key range fully reclaims capacity") {
  constexpr int kKeys = 4000;
  constexpr int kThreads = 8;
  TempFile tmp("dense_adjacent_remove");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(kKeys));

  for (int i = 0; i < kKeys; ++i) {
    REQUIRE(skiplist.put(i, static_cast<uint64_t>(i)));
  }

  std::atomic<int> next_key{0};
  std::vector<std::thread> workers;
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&skiplist, &next_key] {
      for (;;) {
        const int key = next_key.fetch_add(1, std::memory_order_relaxed);
        if (key >= kKeys) break;
        CHECK(skiplist.remove(key));
      }
    });
  }
  for (auto &worker : workers) worker.join();

  std::vector<int> scanned;
  skiplist.scan(0, kKeys, [&](int key, uint64_t) { scanned.push_back(key); });
  CHECK(scanned.empty());

  // Physical unlink is deferred until checkpoint() confirms the removals are durable — see
  // the "put after remove, checkpoint, and reclaim" test in test_functional.cpp.
  REQUIRE(skiplist.checkpoint());
  skiplist.reclaim();

  for (int i = 0; i < kKeys; ++i) {
    CHECK(skiplist.put(kKeys + i, static_cast<uint64_t>(i)));
  }
  int count = 0;
  skiplist.scan(kKeys, 2 * kKeys, [&](int, uint64_t) { ++count; });
  CHECK(count == kKeys);
}
