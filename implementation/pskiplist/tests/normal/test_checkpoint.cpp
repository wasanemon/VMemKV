#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <pskiplist/pskiplist.hpp>
#include <random>
#include <thread>
#include <vector>

#include "support/temp_file.hpp"

using pskiplist::PSkipList;
using pskiplist::read_manifest;
using pskiplist_test::capacity_bytes_for_nodes;
using pskiplist_test::TempFile;

TEST_CASE("checkpoint() with no prior writes still succeeds") {
  TempFile tmp("checkpoint_empty");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(16));
  CHECK(skiplist.checkpoint());

  const auto header = read_manifest(tmp.path());
  REQUIRE(header.has_value());
  CHECK(header->high_water_mark == 2);  // only the head/tail sentinels allocated so far
}

TEST_CASE("checkpoint() publishes a manifest reflecting the current high_water_mark") {
  TempFile tmp("checkpoint_high_water_mark");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(16));
  for (int i = 0; i < 5; ++i) {
    REQUIRE(skiplist.put(i, static_cast<uint64_t>(i)));
  }
  CHECK(skiplist.checkpoint());

  const auto header = read_manifest(tmp.path());
  REQUIRE(header.has_value());
  CHECK(header->high_water_mark == 7);  // 2 sentinels + 5 inserted nodes
}

TEST_CASE("a later checkpoint() replaces the manifest with a newer epoch") {
  TempFile tmp("checkpoint_advances_epoch");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(16));
  CHECK(skiplist.checkpoint());
  const auto first = read_manifest(tmp.path());
  REQUIRE(first.has_value());

  REQUIRE(skiplist.put(1, 10));
  CHECK(skiplist.checkpoint());
  const auto second = read_manifest(tmp.path());
  REQUIRE(second.has_value());

  CHECK(second->epoch > first->epoch);
}

TEST_CASE("a long-running scan() does not block checkpoint()") {
  TempFile tmp("checkpoint_not_blocked_by_scan");
  PSkipList<int> skiplist(tmp.path(), capacity_bytes_for_nodes<int>(16));
  for (int i = 0; i < 10; ++i) {
    REQUIRE(skiplist.put(i, static_cast<uint64_t>(i)));
  }

  std::atomic<bool> scan_started{false};
  std::thread scanner([&] {
    skiplist.scan(0, 10, [&](int, uint64_t) {
      scan_started.store(true, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });
  });

  while (!scan_started.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }

  const auto start = std::chrono::steady_clock::now();
  CHECK(skiplist.checkpoint());
  const auto elapsed = std::chrono::steady_clock::now() - start;

  scanner.join();

  // The scan is still running (10 keys * 200ms > 1.8s remaining) when checkpoint()
  // starts; checkpoint() only waits on writers, so it must return long before the scan
  // does.
  CHECK(elapsed < std::chrono::milliseconds(500));
}

TEST_CASE("concurrent put/remove/checkpoint from multiple threads stays consistent") {
  constexpr int kThreads = 8;
  constexpr int kSharedKeys = 8;
  constexpr int kIterationsPerThread = 1500;
  TempFile tmp("checkpoint_concurrent");
  PSkipList<int> skiplist(tmp.path(),
                          capacity_bytes_for_nodes<int>(static_cast<size_t>(kThreads) * kIterationsPerThread));

  std::atomic<bool> stop{false};
  std::thread checkpointer([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      CHECK(skiplist.checkpoint());
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });

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
  stop.store(true, std::memory_order_relaxed);
  checkpointer.join();

  CHECK_FALSE(failed.load());
  CHECK(skiplist.checkpoint());
}
