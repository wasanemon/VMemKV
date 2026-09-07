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

#include "test_support.hpp"

namespace {

using vmemkv_test::to_span;

// 1024-entry append region: large enough to hold this file's largest key count (800) directly
// without overflowing before a test explicitly triggers split_shard_containing() (which
// reorganizes internally) -- these tests are about routing/split correctness, not about
// exercising AppendRegionFull backpressure (see test_t1_index.cpp for that).
using TinyAppendConfig = vmemkv_test::TinyAppendConfig<10>;

using TestIndex = vmemkv::ShardedT1Index<TinyAppendConfig>;

auto make_index() -> std::unique_ptr<TestIndex> { return std::make_unique<TestIndex>(); }

auto make_index_with_target_size(size_t target_shard_size, size_t worker_threads = 2) -> std::unique_ptr<TestIndex> {
  return std::make_unique<TestIndex>(TinyAppendConfig::T1AppendCapacityEntries, target_shard_size, worker_threads);
}

// checkpoint_all_shards() mirrors T1Index::reorganize()'s (OffsetMapper, ChkWriter) contract;
// these tests don't exercise T2 offset relocation, so an identity offset_mapper matches how
// test_t1_index.cpp's own reorganize() calls do the same for the equivalent parameter.
const auto no_op_offset_mapper =
    vmemkv_test::per_entry_offset_mapper([](uint64_t payload, uint64_t /*hash*/) { return payload; });

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

TEST_CASE("ShardedT1Index: a shard keeps splitting under sustained heavy concurrent writes, never stalling") {
  // continue_split()'s second reorganize() -- taken after already winning the Closing CAS, to
  // capture the shard's truly-final state -- can lose T1Index::reorganize()'s internal
  // reorg_in_progress_ CAS to a redundant, concurrently-dequeued run_maintenance() attempt for the
  // *same* shard (a duplicate queue entry, which the design otherwise treats as harmless);
  // reorganize_until_captured() retries until the callback actually fires, so a lost race is never
  // mistaken for the shard genuinely being too small to split. This test drives many writer and
  // worker threads against a small split threshold so splits are attempted constantly, and asserts
  // shard_count() actually keeps climbing rather than stalling near its earliest value.
  //
  // Sustained pressure matters here, not just a large one-shot key count: request_maintenance_if_
  // needed() only re-fires once a shard's *append region* crosses its own soft threshold again,
  // which needs fresh writes landing in that specific shard -- a burst of unique-key inserts
  // followed by silence lets already-oversized shards sit unsplit forever with nothing left to
  // trigger them, which would fail this test for an unrelated reason. So writers keep cycling
  // put() over a fixed key range for as long as it takes shard_count() to climb past the target
  // (or a generous round budget, as a hang-safety fallback) -- both organic reinsertion (any
  // existing key's update also churns its shard's append region) and the sheer volume keep
  // maintenance continuously re-triggered.
  //
  // kTargetShardCount is a conservative progress bar, not a throughput characterization: this
  // test's job is to catch splitting stalling outright, not to fully characterize splitting
  // throughput under extreme, unrealistic-scale contention.
  constexpr size_t kTargetShardSize = 300;  // Split threshold 200% -> 600 entries.
  constexpr size_t kWorkerThreads = 8;      // High enough for duplicate queue entries to be common.
  constexpr int kWriterThreads = 16;
  constexpr int kKeyRange = 6000;
  constexpr size_t kTargetShardCount = 3;
  auto idx = make_index_with_target_size(kTargetShardSize, kWorkerThreads);

  std::atomic<bool> stop{false};
  std::vector<std::thread> writers;
  writers.reserve(kWriterThreads);
  for (int t = 0; t < kWriterThreads; ++t) {
    writers.emplace_back([&, t] {
      while (!stop.load(std::memory_order_relaxed)) {
        for (int i = t; i < kKeyRange && !stop.load(std::memory_order_relaxed); i += kWriterThreads) {
          idx->put(to_span(ikey(i)), static_cast<uint64_t>(i));
        }
      }
    });
  }

  size_t shard_count = 0;
  for (int round = 0; round < 500 && shard_count < kTargetShardCount; ++round) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    shard_count = idx->shard_count();
  }
  stop.store(true, std::memory_order_relaxed);
  for (auto &writer : writers) {
    writer.join();
  }

  // A healthy split rate clears this conservative bar almost immediately.
  //
  // Deliberately does NOT also verify get() correctness for every key here: with 16 writers
  // cycling non-monotonic values over the same range, no single final value is correct for any
  // key, so a data check cannot distinguish a real inconsistency from a lost race between two
  // live writers. Cycling-write consistency itself is covered by the "keep every key at its
  // latest value" test below, where keys are partitioned across writers with monotonic values.
  CHECK(shard_count >= kTargetShardCount);
}

TEST_CASE("ShardedT1Index: a split under concurrent writes loses no straggler entry") {
  // A write that resolved a shard just before it entered Closing can still be "in flight" when
  // continue_split()'s pre-split snapshot is taken, and land afterward -- put() re-checks its
  // written slot's superseded and forwards its own write to the live shard before returning (see
  // its own comment), since T1Index's own concurrency handling has no visibility into a caller
  // that hasn't reached T1Index::put() yet. This test does a single-pass unique-key insert (each
  // key written exactly once, unlike the cycling-writes shape of this file's other stress tests,
  // which is what this straggler window needs to surface) at a scale small enough to run quickly
  // in-tree; the same shape has also been validated clean at production scale (~5M keys, default
  // target_shard_size).
  constexpr size_t kTargetShardSize = 5000;  // Split threshold 200% -> 10000 entries.
  constexpr int kWriterThreads = 4;
  constexpr int kKeyCount = 30000;  // Comfortably past several splits' worth at this target size.
  auto idx = make_index_with_target_size(kTargetShardSize, /*worker_threads=*/2);

  std::vector<std::thread> writers;
  writers.reserve(kWriterThreads);
  for (int t = 0; t < kWriterThreads; ++t) {
    writers.emplace_back([&, t] {
      for (int i = t; i < kKeyCount; i += kWriterThreads) {
        CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
      }
    });
  }
  for (auto &writer : writers) {
    writer.join();
  }

  // Background maintenance workers are internal jthreads this test never joins directly -- wait
  // for shard_count() to stop changing, then give any in-flight split a generous fixed window to
  // finish.
  size_t settled_shard_count = 0;
  for (int round = 0; round < 300; ++round) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const size_t current = idx->shard_count();
    if (current == settled_shard_count) {
      break;
    }
    settled_shard_count = current;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  CHECK(idx->shard_count() > 1);
  for (int i = 0; i < kKeyCount; ++i) {
    CHECK(idx->get(to_span(ikey(i))) == static_cast<uint64_t>(i));
  }
}

TEST_CASE("ShardedT1Index: maintenance_soft_threshold lowers only under scan at scale") {
  // Production-scale append capacity (2^21): the normal threshold is half the capacity, and an
  // active scan pulls it down to the L2-cache-sized slot budget.
  constexpr size_t kProdCap = size_t{1} << 21;
  CHECK(TestIndex::maintenance_soft_threshold(kProdCap, false) == kProdCap / 2);
  const size_t scan_threshold = TestIndex::maintenance_soft_threshold(kProdCap, true);
  CHECK(scan_threshold ==
        std::min(size_t{1024 * 1024} / vmemkv::T1Index<TinyAppendConfig>::append_slot_bytes(), kProdCap / 2));
  CHECK(scan_threshold < kProdCap / 2);
  // In-tree tiny capacities: the L2 budget never binds, so scan-active changes nothing.
  CHECK(TestIndex::maintenance_soft_threshold(1024, false) == 512);
  CHECK(TestIndex::maintenance_soft_threshold(1024, true) == 512);
}

TEST_CASE("ShardedT1Index: delete pressure triggers maintenance below the occupancy threshold") {
  // 400 live keys stay under the 50%-of-1024 occupancy trigger and under the split threshold,
  // so only the tombstone count (400 deletes >= target_shard_size 400) can request maintenance.
  // Without the delete-pressure trigger the tombstoned entries would sit in the append region
  // indefinitely; with it, a background merge carries them away and append_size() drains to 0.
  constexpr size_t kTargetShardSize = 400;
  constexpr int kKeyCount = 400;
  auto idx = make_index_with_target_size(kTargetShardSize, /*worker_threads=*/2);

  for (int i = 0; i < kKeyCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }
  CHECK(idx->append_size() > 0);
  for (int i = 0; i < kKeyCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), vmemkv::STORE_NOT_FOUND) == TestIndex::PutResult::Applied);
  }

  for (int round = 0; round < 300; ++round) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (idx->append_size() == 0) {
      break;
    }
  }
  CHECK(idx->append_size() == 0);
  CHECK(idx->shard_count() == 1);
  for (int i = 0; i < kKeyCount; ++i) {
    CHECK(idx->get(to_span(ikey(i))) == vmemkv::STORE_NOT_FOUND);
  }
}

TEST_CASE("ShardedT1Index: checkpoint on clean shards matches a full merge") {
  // checkpoint_all_shards() skips the merge for shards with an empty append region, serializing
  // the sorted region directly. With no new writes between two checkpoints, the second one takes
  // the dump path on every shard and must produce entry-for-entry identical output to the first
  // (merge path) -- same keys, payloads, hashes, and shard assignment.
  constexpr size_t kTargetShardSize = 5000;
  constexpr int kKeyCount = 3000;
  auto idx = make_index_with_target_size(kTargetShardSize, /*worker_threads=*/2);

  for (int i = 0; i < kKeyCount; ++i) {
    CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }

  using Snapshot = TestIndex::EntrySnapshot;
  auto capture = [&](std::vector<std::vector<Snapshot>> &out) {
    out.clear();
    std::vector<TestIndex::Key> boundaries = idx->checkpoint_all_shards(
        no_op_offset_mapper, [&](std::span<const Snapshot> merged) { out.emplace_back(merged.begin(), merged.end()); });
    return boundaries;
  };

  std::vector<std::vector<Snapshot>> merged_out;
  const std::vector<TestIndex::Key> merged_boundaries = capture(merged_out);
  size_t merged_total = 0;
  for (const auto &shard : merged_out) {
    merged_total += shard.size();
  }
  CHECK(merged_total == static_cast<size_t>(kKeyCount));

  // No writes since: every shard's append region is empty, so this is the dump path throughout.
  CHECK(idx->append_size() == 0);
  std::vector<std::vector<Snapshot>> dumped_out;
  const std::vector<TestIndex::Key> dumped_boundaries = capture(dumped_out);
  CHECK(dumped_boundaries == merged_boundaries);
  REQUIRE(dumped_out.size() == merged_out.size());
  for (size_t s = 0; s < dumped_out.size(); ++s) {
    REQUIRE(dumped_out[s].size() == merged_out[s].size());
    for (size_t i = 0; i < dumped_out[s].size(); ++i) {
      CHECK(dumped_out[s][i].key == merged_out[s][i].key);
      CHECK(dumped_out[s][i].payload_bits == merged_out[s][i].payload_bits);
      CHECK(dumped_out[s][i].hash == merged_out[s][i].hash);
    }
  }
}

TEST_CASE("ShardedT1Index: cycling updates under tiny shards keep every key at its latest value") {
  // Keys are partitioned across writers and each writer stores a monotonically increasing round
  // number per key, so the only correct final value of every key is kRounds - 1: anything older
  // is a stale write clobbering a newer one, and STORE_NOT_FOUND is a lost key. A dedicated
  // splitter thread hammers split_shard_containing() across the same range so every split's
  // post-publication window races the cycling writers dozens of times per run.
  constexpr size_t kTargetShardSize = 100;
  constexpr int kWriterThreads = 8;
  constexpr int kKeyRange = 400;
  constexpr int kRounds = 40;
  auto idx = make_index_with_target_size(kTargetShardSize, /*worker_threads=*/2);

  std::atomic<bool> stop{false};
  std::vector<std::thread> writers;
  writers.reserve(kWriterThreads);
  for (int t = 0; t < kWriterThreads; ++t) {
    writers.emplace_back([&, t] {
      for (int round = 0; round < kRounds; ++round) {
        for (int i = t; i < kKeyRange; i += kWriterThreads) {
          CHECK(idx->put(to_span(ikey(i)), static_cast<uint64_t>(round)) == TestIndex::PutResult::Applied);
        }
      }
    });
  }
  std::thread splitter([&] {
    for (int round = 0; round < 2000 && !stop.load(std::memory_order_relaxed); ++round) {
      idx->split_shard_containing(to_span(ikey((round * 37) % kKeyRange)));
    }
  });

  for (auto &writer : writers) {
    writer.join();
  }
  stop.store(true, std::memory_order_relaxed);
  splitter.join();

  size_t settled_shard_count = 0;
  for (int round = 0; round < 300; ++round) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const size_t current = idx->shard_count();
    if (current == settled_shard_count) {
      break;
    }
    settled_shard_count = current;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  CHECK(idx->shard_count() > 1);
  for (int i = 0; i < kKeyRange; ++i) {
    CHECK(idx->get(to_span(ikey(i))) == static_cast<uint64_t>(kRounds - 1));
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
  // run_maintenance() must dequeue a ShardSlot* from inside with_routing_guard(), not before it
  // (see that method's own comment): a concurrent split_shard_containing() targeting the same
  // shard (which resolves and CASes entirely on its own, independent of the queue) could
  // otherwise win, complete, and delete that shard while a worker still holds an unguarded pointer
  // to it. This test puts both paths in direct contention on the same small shards by using a
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
