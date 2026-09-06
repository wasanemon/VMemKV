// test_sharded_t1_index.cpp — Correctness tests for ShardedT1Index: routing, scan across
// shard boundaries, the split protocol (Closing -> reorganize -> Split), and a concurrency
// stress test that regresses the forwarding-pointer race a split introduces (get/put/scan
// racing split_shard_containing()).

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <t1_index/sharded_t1_index.hpp>
#include <thread>
#include <vector>
#include <vmemkv/config.hpp>

namespace {

// Small append-region capacity, but large enough to hold this file's largest key count (800)
// directly without overflowing before a test explicitly triggers split_shard_containing()
// (which reorganizes internally) -- these tests are about routing/split correctness, not about
// exercising AppendRegionFull backpressure (see test_t1_index.cpp for that).
struct TinyAppendConfig : vmemkv::Config<> {
  static constexpr size_t T1AppendCapacityLog2 = 10;  // 1024-entry append region.
  static constexpr size_t T1AppendCapacityEntries = size_t{1} << T1AppendCapacityLog2;
};

using TestIndex = vmemkv::ShardedT1Index<TinyAppendConfig>;

auto to_span(const std::string &key_string) -> std::span<const std::byte> {
  return std::span<const std::byte>(reinterpret_cast<const std::byte *>(key_string.data()), key_string.size());
}

auto make_index() -> std::unique_ptr<TestIndex> { return std::make_unique<TestIndex>(); }

auto make_index_with_target_size(size_t target_shard_size, size_t worker_threads = 2) -> std::unique_ptr<TestIndex> {
  return std::make_unique<TestIndex>(TinyAppendConfig::T1AppendCapacityEntries, target_shard_size, worker_threads);
}

// checkpoint_all_shards() mirrors T1Index::reorganize()'s (OffsetMapper, ChkWriter) contract;
// these tests don't exercise T2 offset relocation, so a no-op offset_mapper matches how
// test_t1_index.cpp's own reorganize() calls do the same for the equivalent parameter.
auto no_op_offset_mapper(std::span<TestIndex::EntrySnapshot> /*merged*/) -> void {}

// Sortable, fixed-width keys ("k" + zero-padded index) so directory boundary comparisons and
// scan range order match numeric order.
auto ikey(int i) -> std::string {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "k%08d", i);
  return std::string(buf);
}

}  // namespace

TEST_CASE("ShardedT1Index: get on empty index returns STORE_NOT_FOUND") {
  auto idx = make_index();
  CHECK(idx->get(to_span("missing")) == vmemkv::STORE_NOT_FOUND);
}

TEST_CASE("ShardedT1Index: put then get round-trips, starts at a single shard") {
  auto idx = make_index();
  CHECK(idx->shard_count() == 1);
  CHECK(idx->put(to_span("a"), 42) == TestIndex::PutResult::Applied);
  CHECK(idx->get(to_span("a")) == 42U);
  CHECK(idx->shard_count() == 1);
}

TEST_CASE("ShardedT1Index: put with STORE_NOT_FOUND tombstones the key") {
  auto idx = make_index();
  CHECK(idx->put(to_span("a"), 1) == TestIndex::PutResult::Applied);
  CHECK(idx->put(to_span("a"), vmemkv::STORE_NOT_FOUND) == TestIndex::PutResult::Applied);
  CHECK(idx->get(to_span("a")) == vmemkv::STORE_NOT_FOUND);
}

TEST_CASE("ShardedT1Index: scan on a single shard returns all live entries in order") {
  auto idx = make_index();
  constexpr int kCount = 50;
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }
  std::vector<uint64_t> seen;
  const size_t matched = idx->scan(to_span(ikey(0)),
                                   to_span(ikey(kCount - 1)),
                                   [&](auto /*key*/, auto payload, auto /*hash*/) { seen.push_back(payload); });
  CHECK(matched == static_cast<size_t>(kCount));
  REQUIRE(seen.size() == static_cast<size_t>(kCount));
  CHECK(std::is_sorted(seen.begin(), seen.end()));
}

TEST_CASE("ShardedT1Index: split_shard_containing doubles the shard count and preserves all keys") {
  auto idx = make_index();
  constexpr int kCount = 400;
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }

  idx->split_shard_containing(to_span(ikey(0)));
  CHECK(idx->shard_count() == 2);

  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->get(to_span(ikey(i))) == static_cast<uint64_t>(i));
  }
}

TEST_CASE("ShardedT1Index: scan across a split boundary is gap-free, duplicate-free, and sorted") {
  auto idx = make_index();
  constexpr int kCount = 400;
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }
  idx->split_shard_containing(to_span(ikey(0)));
  REQUIRE(idx->shard_count() == 2);

  std::vector<uint64_t> seen;
  const size_t matched = idx->scan(to_span(ikey(0)),
                                   to_span(ikey(kCount - 1)),
                                   [&](auto /*key*/, auto payload, auto /*hash*/) { seen.push_back(payload); });
  CHECK(matched == static_cast<size_t>(kCount));
  REQUIRE(seen.size() == static_cast<size_t>(kCount));
  CHECK(std::is_sorted(seen.begin(), seen.end()));
  const std::set<uint64_t> unique_seen(seen.begin(), seen.end());
  CHECK(unique_seen.size() == seen.size());
}

TEST_CASE("ShardedT1Index: put after split routes to the correct new shard and is visible") {
  auto idx = make_index();
  constexpr int kCount = 400;
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }
  idx->split_shard_containing(to_span(ikey(0)));
  REQUIRE(idx->shard_count() == 2);

  // One key on each side of wherever the split landed.
  CHECK(idx->put(to_span(ikey(0)), 9000) == TestIndex::PutResult::Applied);
  CHECK(idx->put(to_span(ikey(kCount - 1)), 9001) == TestIndex::PutResult::Applied);
  CHECK(idx->get(to_span(ikey(0))) == 9000U);
  CHECK(idx->get(to_span(ikey(kCount - 1))) == 9001U);
}

TEST_CASE("ShardedT1Index: splitting an already-splitting shard is a single-flight no-op") {
  auto idx = make_index();
  constexpr int kCount = 400;
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }
  idx->split_shard_containing(to_span(ikey(0)));
  REQUIRE(idx->shard_count() == 2);
  // Calling again on a key whose shard was never re-split is a correctness no-op (the target
  // resolved by split_shard_containing() is now one of the fresh post-split shards).
  idx->split_shard_containing(to_span(ikey(0)));
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->get(to_span(ikey(i))) == static_cast<uint64_t>(i));
  }
}

TEST_CASE("ShardedT1Index: concurrent put/get/scan survive a racing split_shard_containing") {
  auto idx = make_index();
  constexpr int kCount = 800;
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }

  std::atomic<bool> stop{false};
  std::atomic<bool> saw_torn_read{false};

  // Writer: repeatedly overwrites every key with a new generation value, monotonically
  // increasing per key so readers can sanity-check "value only ever moves forward".
  std::thread writer([&] {
    uint64_t generation = 1;
    while (!stop.load(std::memory_order_relaxed)) {
      for (int i = 0; i < kCount && !stop.load(std::memory_order_relaxed); ++i) {
        idx->put(to_span(ikey(i)), generation * 10000 + static_cast<uint64_t>(i));
      }
      ++generation;
    }
  });

  // Readers: get() every key, scan() the whole range.
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < kCount; ++i) {
          const uint64_t val = idx->get(to_span(ikey(i)));
          if (val != vmemkv::STORE_NOT_FOUND && val % 10000 != static_cast<uint64_t>(i)) {
            saw_torn_read.store(true, std::memory_order_relaxed);
          }
        }
        size_t total = 0;
        idx->scan(to_span(ikey(0)), to_span(ikey(kCount - 1)), [&](auto /*k*/, auto /*p*/, auto /*h*/) { ++total; });
        if (total > static_cast<size_t>(kCount)) {
          // More matches than distinct keys ever inserted -- would indicate a duplicate from a
          // botched split (same key returned via two different leaves).
          saw_torn_read.store(true, std::memory_order_relaxed);
        }
      }
    });
  }

  // Splitter: repeatedly splits whichever shard currently owns key 0, a few times, with small
  // pauses so readers/writers actually observe the split in flight.
  std::thread splitter([&] {
    for (int round = 0; round < 5; ++round) {
      idx->split_shard_containing(to_span(ikey(0)));
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  splitter.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop.store(true, std::memory_order_relaxed);
  writer.join();
  for (auto &reader : readers) {
    reader.join();
  }

  CHECK_FALSE(saw_torn_read.load());
  CHECK(idx->shard_count() >= 2);
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->get(to_span(ikey(i))) != vmemkv::STORE_NOT_FOUND);
  }
}

TEST_CASE("ShardedT1Index: background worker automatically splits an oversized shard") {
  // target_shard_size=50 -> split threshold 100 (T1ShardSplitThresholdPercent=200%); kCount=600
  // both exceeds that and crosses TinyAppendConfig's soft threshold (50% of 1024), so the
  // background worker picks it up without any explicit split_shard_containing() call.
  auto idx = make_index_with_target_size(/*target_shard_size=*/50);
  constexpr int kCount = 600;
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }

  bool split_happened = false;
  for (int attempt = 0; attempt < 200 && !split_happened; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    split_happened = idx->shard_count() > 1;
  }
  CHECK(split_happened);

  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->get(to_span(ikey(i))) == static_cast<uint64_t>(i));
  }
}

TEST_CASE("ShardedT1Index: concurrent inserts trigger multiple automatic splits without losing data") {
  auto idx = make_index_with_target_size(/*target_shard_size=*/100);
  constexpr int kThreads = 4;
  constexpr int kPerThread = 500;

  std::vector<std::thread> writers;
  for (int t = 0; t < kThreads; ++t) {
    writers.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        const int key_id = (t * kPerThread) + i;
        CHECK(idx->put(to_span(ikey(key_id)), static_cast<uint64_t>(key_id)) == TestIndex::PutResult::Applied);
      }
    });
  }
  for (auto &writer : writers) {
    writer.join();
  }

  // Let any still-queued maintenance drain before asserting on shard_count()/contents.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  CHECK(idx->shard_count() > 1);
  constexpr int kTotalKeys = kThreads * kPerThread;
  for (int i = 0; i < kTotalKeys; ++i) {
    CHECK(idx->get(to_span(ikey(i))) == static_cast<uint64_t>(i));
  }

  std::vector<uint64_t> seen;
  const size_t matched = idx->scan(to_span(ikey(0)),
                                   to_span(ikey(kTotalKeys - 1)),
                                   [&](auto /*key*/, auto payload, auto /*hash*/) { seen.push_back(payload); });
  CHECK(matched == static_cast<size_t>(kTotalKeys));
  REQUIRE(seen.size() == static_cast<size_t>(kTotalKeys));
  const std::set<uint64_t> unique_seen(seen.begin(), seen.end());
  CHECK(unique_seen.size() == seen.size());
}

TEST_CASE("ShardedT1Index: checkpoint_all_shards captures every live key across all shards") {
  auto idx = make_index();
  constexpr int kCount = 400;
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }
  idx->split_shard_containing(to_span(ikey(0)));
  REQUIRE(idx->shard_count() == 2);

  std::vector<std::vector<TestIndex::EntrySnapshot>> per_shard;
  const std::vector<TestIndex::Key> boundaries = idx->checkpoint_all_shards(
      no_op_offset_mapper,
      [&](std::span<const TestIndex::EntrySnapshot> merged) { per_shard.emplace_back(merged.begin(), merged.end()); });

  REQUIRE(boundaries.size() == 1);
  REQUIRE(per_shard.size() == 2);
  size_t total_entries = 0;
  for (const auto &shard_entries : per_shard) {
    total_entries += shard_entries.size();
  }
  CHECK(total_entries == static_cast<size_t>(kCount));
}

TEST_CASE("ShardedT1Index: recovers via load_from_checkpoint into a fresh instance") {
  auto idx = make_index();
  constexpr int kCount = 400;
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }
  idx->split_shard_containing(to_span(ikey(0)));
  REQUIRE(idx->shard_count() == 2);

  std::vector<std::vector<TestIndex::EntrySnapshot>> per_shard;
  const std::vector<TestIndex::Key> boundaries = idx->checkpoint_all_shards(
      no_op_offset_mapper,
      [&](std::span<const TestIndex::EntrySnapshot> merged) { per_shard.emplace_back(merged.begin(), merged.end()); });

  auto recovered = make_index();
  recovered->load_from_checkpoint(boundaries, per_shard);

  CHECK(recovered->shard_count() == 2);
  for (int i = 0; i < kCount; ++i) {
    CHECK(recovered->get(to_span(ikey(i))) == static_cast<uint64_t>(i));
  }

  // The recovered instance is fully live: further put/get/split work normally.
  CHECK(recovered->put(to_span(ikey(kCount)), 999) == TestIndex::PutResult::Applied);
  CHECK(recovered->get(to_span(ikey(kCount))) == 999U);
}

TEST_CASE("ShardedT1Index: checkpoint concurrent with put/get/scan/split loses no live key") {
  auto idx = make_index();
  constexpr int kCount = 800;
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }

  std::atomic<bool> stop{false};
  std::thread writer([&] {
    uint64_t generation = 1;
    while (!stop.load(std::memory_order_relaxed)) {
      for (int i = 0; i < kCount && !stop.load(std::memory_order_relaxed); ++i) {
        idx->put(to_span(ikey(i)), (generation * 10000) + static_cast<uint64_t>(i));
      }
      ++generation;
    }
  });
  std::thread splitter([&] {
    for (int round = 0; round < 5 && !stop.load(std::memory_order_relaxed); ++round) {
      idx->split_shard_containing(to_span(ikey(0)));
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
  });

  std::vector<std::vector<TestIndex::EntrySnapshot>> per_shard;
  std::vector<TestIndex::Key> boundaries;
  for (int round = 0; round < 5; ++round) {
    per_shard.clear();
    boundaries = idx->checkpoint_all_shards(no_op_offset_mapper, [&](std::span<const TestIndex::EntrySnapshot> merged) {
      per_shard.emplace_back(merged.begin(), merged.end());
    });
    CHECK(boundaries.size() + 1 == per_shard.size());
    size_t total_entries = 0;
    for (const auto &shard_entries : per_shard) {
      total_entries += shard_entries.size();
    }
    CHECK(total_entries == static_cast<size_t>(kCount));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  stop.store(true, std::memory_order_relaxed);
  splitter.join();
  writer.join();
}

TEST_CASE("ShardedT1Index: worker_threads=0 defers background maintenance until start_workers()") {
  // target_shard_size=50 (split threshold 100) with worker_threads=0: puts land normally and
  // cross the soft threshold, but with no worker running, nothing should split automatically --
  // this is the deferred-start contract a caller doing single-threaded setup (e.g. loading a
  // checkpoint) before it's safe for any background thread to touch this instance relies on.
  auto idx = std::make_unique<TestIndex>(TinyAppendConfig::T1AppendCapacityEntries,
                                         /*target_shard_size=*/50,
                                         /*worker_threads=*/0);
  constexpr int kCount = 600;
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(idx->shard_count() == 1);  // No worker running yet -- no automatic split could have happened.

  idx->start_workers(2);

  bool split_happened = false;
  for (int attempt = 0; attempt < 200 && !split_happened; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    split_happened = idx->shard_count() > 1;
  }
  CHECK(split_happened);
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->get(to_span(ikey(i))) == static_cast<uint64_t>(i));
  }
}

TEST_CASE(
    "ShardedT1Index: background-worker-driven splits survive a concurrent explicit "
    "split_shard_containing on the same shards") {
  // Regression test for a UAF where worker_loop() popped a ShardSlot* off the maintenance queue
  // *before* run_maintenance() entered with_routing_guard() -- a concurrent
  // split_shard_containing() targeting the same shard (which resolves and CASes entirely on its
  // own, independent of the queue) could win, complete, and delete that shard before the worker's
  // still-unguarded pointer was ever touched. Neither existing test combined real worker-driven
  // splitting with a concurrent explicit splitter on shards the workers are also organically
  // splitting: this one puts both paths in direct contention on the same small shards by using a
  // small target_shard_size (so background workers split constantly) alongside a splitter thread
  // hammering split_shard_containing() across the same key range.
  auto idx = make_index_with_target_size(/*target_shard_size=*/20, /*worker_threads=*/4);
  constexpr int kCount = 2000;

  std::atomic<bool> stop{false};

  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&, t] {
      for (int i = t; i < kCount; i += 4) {
        idx->put(to_span(ikey(i)), static_cast<uint64_t>(i));
      }
    });
  }

  // Splitter: repeatedly targets shards spread across the whole key range, directly contending
  // with whichever shards the background workers are simultaneously organically splitting.
  std::thread splitter([&] {
    int round = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      idx->split_shard_containing(to_span(ikey((round * 37) % kCount)));
      ++round;
    }
  });

  // Reader: exercises get()/scan() throughout, same as the sibling stress tests.
  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      for (int i = 0; i < kCount; i += 17) {
        idx->get(to_span(ikey(i)));
      }
      idx->scan(to_span(ikey(0)), to_span(ikey(kCount - 1)), [](auto /*k*/, auto /*p*/, auto /*h*/) {});
    }
  });

  for (auto &writer : writers) {
    writer.join();
  }
  // Let workers/splitter keep contending on the now-fully-populated index for a bit longer --
  // this is where organic worker-driven splits and the explicit splitter are most likely to race.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true, std::memory_order_relaxed);
  splitter.join();
  reader.join();

  CHECK(idx->shard_count() > 1);
  std::vector<uint64_t> seen;
  const size_t matched = idx->scan(to_span(ikey(0)), to_span(ikey(kCount - 1)), [&](auto /*key*/, auto payload, auto /*hash*/) {
    seen.push_back(payload);
  });
  CHECK(matched == static_cast<size_t>(kCount));
  REQUIRE(seen.size() == static_cast<size_t>(kCount));
  const std::set<uint64_t> unique_seen(seen.begin(), seen.end());
  CHECK(unique_seen.size() == seen.size());
  for (int i = 0; i < kCount; ++i) {
    CHECK(idx->get(to_span(ikey(i))) == static_cast<uint64_t>(i));
  }
}
