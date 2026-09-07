// test_t1_index.cpp — Isolated correctness tests for the standalone T1Index type.
//
// These tests exercise vmemkv::T1Index directly (no VMemKVImpl/T2FlatFile/WAL involvement):
// get/put round-trips, reorganize()'s merge + chk_writer snapshot, checkpoint-adopt via
// load_sorted_region_from_checkpoint(), and a concurrency stress test that regresses the
// with_epoch_guard() fix (put/get_with_hash/append_size racing reorganize()).

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <t1_index/t1_index.hpp>
#include <thread>
#include <vector>
#include <vmemkv/config.hpp>

#include "test_support.hpp"

namespace {

using vmemkv_test::to_span;

// 256-entry append region -- small enough that reorganize() triggers (both explicitly in tests
// and implicitly in the stress test) without needing thousands of puts per run.
using TinyAppendConfig = vmemkv_test::TinyAppendConfig<8>;

using TestIndex = vmemkv::T1Index<TinyAppendConfig>;

// Heap-allocate: the append-region hash index embeds a large bucket array unsuited to the stack.
auto make_index() -> std::unique_ptr<TestIndex> { return std::make_unique<TestIndex>(); }

// Builds an EntrySnapshot the same way put()/reorganize() would (same prefix truncation, same
// hash) since load_sorted_region_from_checkpoint() bypasses put()'s own hashing.
auto make_entry(const std::string &key_string, TestIndex::Payload payload) -> TestIndex::EntrySnapshot {
  TestIndex::Key key{};
  std::memcpy(key.data(), key_string.data(), std::min(key_string.size(), key.size()));
  return TestIndex::EntrySnapshot{key, payload, t1_detail::hash_full_key(to_span(key_string))};
}

using vmemkv_test::per_entry_offset_mapper;

}  // namespace

TEST_CASE("T1Index: get on empty index returns STORE_NOT_FOUND") {
  auto idx = make_index();
  CHECK(idx->get(to_span("missing")) == vmemkv::STORE_NOT_FOUND);
}

TEST_CASE("T1Index: put then get round-trips the payload") {
  auto idx = make_index();
  CHECK(idx->put(to_span("a"), 42) == TestIndex::PutResult::Applied);
  CHECK(idx->get(to_span("a")) == 42U);
}

TEST_CASE("T1Index: put on an existing key overwrites in place, does not grow the append region") {
  auto idx = make_index();
  CHECK(idx->put(to_span("a"), 1) == TestIndex::PutResult::Applied);
  CHECK(idx->put(to_span("a"), 2) == TestIndex::PutResult::Applied);
  CHECK(idx->get(to_span("a")) == 2U);
  CHECK(idx->append_size() == 1);
}

TEST_CASE("T1Index: put with STORE_NOT_FOUND tombstones the key") {
  auto idx = make_index();
  CHECK(idx->put(to_span("a"), 1) == TestIndex::PutResult::Applied);
  CHECK(idx->put(to_span("a"), vmemkv::STORE_NOT_FOUND) == TestIndex::PutResult::Applied);
  CHECK(idx->get(to_span("a")) == vmemkv::STORE_NOT_FOUND);
}

TEST_CASE("T1Index: append region reports AppendRegionFull once capacity is exhausted") {
  auto idx = make_index();
  const size_t append_cap = idx->append_capacity();
  size_t applied = 0;
  for (size_t i = 0; i < append_cap + 1; ++i) {
    const std::string key = "k" + std::to_string(i);
    if (idx->put(to_span(key), static_cast<uint64_t>(i)) == TestIndex::PutResult::AppendRegionFull) {
      break;
    }
    ++applied;
  }
  CHECK(applied == append_cap);
}

TEST_CASE("T1Index: reorganize merges append region into sorted region, keeps live entries readable") {
  auto idx = make_index();
  constexpr int key_count = 100;
  for (int i = 0; i < key_count; ++i) {
    CHECK(idx->put(to_span("k" + std::to_string(i)), static_cast<uint64_t>(i)) == TestIndex::PutResult::Applied);
  }
  CHECK(idx->append_size() == static_cast<size_t>(key_count));

  idx->reorganize(per_entry_offset_mapper([](uint64_t payload, uint64_t /*hash*/) { return payload; }));

  CHECK(idx->append_size() == 0);
  for (int i = 0; i < key_count; ++i) {
    CHECK(idx->get(to_span("k" + std::to_string(i))) == static_cast<uint64_t>(i));
  }
}

TEST_CASE("T1Index: reorganize drops tombstoned entries") {
  auto idx = make_index();
  CHECK(idx->put(to_span("a"), 1) == TestIndex::PutResult::Applied);
  CHECK(idx->put(to_span("b"), 2) == TestIndex::PutResult::Applied);
  CHECK(idx->put(to_span("a"), vmemkv::STORE_NOT_FOUND) == TestIndex::PutResult::Applied);

  idx->reorganize(per_entry_offset_mapper([](uint64_t payload, uint64_t /*hash*/) { return payload; }));

  CHECK(idx->get(to_span("a")) == vmemkv::STORE_NOT_FOUND);
  CHECK(idx->get(to_span("b")) == 2U);
}

TEST_CASE("T1Index: reorganize's offset_mapper remaps every live payload") {
  auto idx = make_index();
  CHECK(idx->put(to_span("a"), 10) == TestIndex::PutResult::Applied);
  CHECK(idx->put(to_span("b"), 20) == TestIndex::PutResult::Applied);

  idx->reorganize(per_entry_offset_mapper([](uint64_t payload, uint64_t /*hash*/) { return payload + 1000; }));

  CHECK(idx->get(to_span("a")) == 1010U);
  CHECK(idx->get(to_span("b")) == 1020U);
}

TEST_CASE("T1Index: reorganize's chk_writer sees exactly the merged, sorted, offset-mapped live entries") {
  auto idx = make_index();
  CHECK(idx->put(to_span("b"), 2) == TestIndex::PutResult::Applied);
  CHECK(idx->put(to_span("a"), 1) == TestIndex::PutResult::Applied);
  CHECK(idx->put(to_span("c"), 3) == TestIndex::PutResult::Applied);
  // Tombstoned before reorganize: must be excluded from what chk_writer observes.
  CHECK(idx->put(to_span("c"), vmemkv::STORE_NOT_FOUND) == TestIndex::PutResult::Applied);

  std::vector<TestIndex::EntrySnapshot> observed;
  idx->reorganize(
      per_entry_offset_mapper([](uint64_t payload, uint64_t /*hash*/) { return payload + 100; }),
      [&](std::span<const TestIndex::EntrySnapshot> merged) { observed.assign(merged.begin(), merged.end()); });

  REQUIRE(observed.size() == 2);
  // key_prefix ascending: "a" < "b".
  CHECK(observed[0].payload_bits == 101U);
  CHECK(observed[1].payload_bits == 102U);
}

TEST_CASE("T1Index: load_sorted_region_from_checkpoint adopts entries as the live sorted region") {
  auto idx = make_index();
  const std::vector<TestIndex::EntrySnapshot> entries = {make_entry("a", 111), make_entry("b", 222)};

  idx->load_sorted_region_from_checkpoint(entries);

  CHECK(idx->get(to_span("a")) == 111U);
  CHECK(idx->get(to_span("b")) == 222U);
  CHECK(idx->append_size() == 0);
}

TEST_CASE("T1Index: load_sorted_region_from_checkpoint replaces any previously loaded sorted region") {
  auto idx = make_index();
  idx->load_sorted_region_from_checkpoint(std::vector<TestIndex::EntrySnapshot>{make_entry("a", 1)});
  CHECK(idx->get(to_span("a")) == 1U);

  idx->load_sorted_region_from_checkpoint(std::vector<TestIndex::EntrySnapshot>{make_entry("a", 2)});
  CHECK(idx->get(to_span("a")) == 2U);
}

// put(), get_with_hash(), and append_size() must all register in active_epochs_ (via
// with_epoch_guard()) for their whole duration, or reorganize()'s wait_until_epoch() has no way
// to know they are still using the buffers it is about to delete. Hammering reorganize()
// concurrently with all three is the most direct way to exercise that.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("T1Index: concurrent put/get_with_hash/append_size survive racing reorganize") {
  auto idx = make_index();
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reorganize_count{0};

  constexpr int writer_thread_count = 4;
  constexpr int reader_thread_count = 4;
  constexpr int iterations_per_writer = 1500;

  std::thread reorganizer([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      idx->reorganize(per_entry_offset_mapper([](uint64_t payload, uint64_t /*hash*/) { return payload; }));
      reorganize_count.fetch_add(1, std::memory_order_relaxed);
    }
  });

  std::vector<std::thread> readers;
  readers.reserve(reader_thread_count);
  for (int reader_id = 0; reader_id < reader_thread_count; ++reader_id) {
    readers.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        (void)idx->get_with_hash(to_span(std::string("w0_0")));
        (void)idx->append_size();
      }
    });
  }

  std::vector<std::thread> writers;
  writers.reserve(writer_thread_count);
  for (int writer_id = 0; writer_id < writer_thread_count; ++writer_id) {
    writers.emplace_back([&, writer_id]() {
      for (int i = 0; i < iterations_per_writer; ++i) {
        const std::string key = "w" + std::to_string(writer_id) + "_" + std::to_string(i);
        while (idx->put(to_span(key), static_cast<uint64_t>(i)) == TestIndex::PutResult::AppendRegionFull) {
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto &writer : writers) {
    writer.join();
  }
  stop.store(true, std::memory_order_relaxed);
  reorganizer.join();
  for (auto &reader : readers) {
    reader.join();
  }

  CHECK(reorganize_count.load() > 0);
  for (int writer_id = 0; writer_id < writer_thread_count; ++writer_id) {
    const std::string key = "w" + std::to_string(writer_id) + "_" + std::to_string(iterations_per_writer - 1);
    CHECK(idx->get(to_span(key)) == static_cast<uint64_t>(iterations_per_writer - 1));
  }
}

// reorganize()'s merge dedups a sorted-region entry against an append-region entry for the same
// key by comparing *clean* hash (t1_detail::kCleanHashMask strips the inline-metadata bits an
// inline<->non-inline transition changes), so a put() racing resolve()'s "immutable-region
// bypass" (inserts a *new* active-region entry instead of updating the frozen one in place, to
// avoid a Lost Update) across such a transition is still recognized as the same logical key
// instead of surviving as two live entries.
struct TinyInlineConfig : vmemkv::Config<vmemkv::T1InlineValue> {
  static constexpr size_t T1AppendCapacityLog2 = 8;
  static constexpr size_t T1AppendCapacityEntries = size_t{1} << T1AppendCapacityLog2;
};
using InlineTestIndex = vmemkv::T1Index<TinyInlineConfig>;

TEST_CASE(
    "T1Index: inline-to-non-inline transition racing an in-flight reorganize does not create a "
    "duplicate entry") {
  auto idx = std::make_unique<InlineTestIndex>();

  // "k" starts out inline (an 8B-eligible value); its stored hash has the inline metadata bits set.
  REQUIRE(idx->put(to_span("k"), 111, /*is_inline=*/true, /*inline_size=*/8) == InlineTestIndex::PutResult::Applied);

  // reorg #1 freezes "k" into append_immutable_, then blocks inside offset_mapper (fires once,
  // since there's exactly one live entry), keeping it parked there.
  std::atomic<bool> reorg1_entered_offset_mapper{false};
  std::atomic<bool> proceed{false};
  std::thread reorg1([&] {
    idx->reorganize(per_entry_offset_mapper([&](uint64_t payload, uint64_t /*hash*/) {
      reorg1_entered_offset_mapper.store(true, std::memory_order_release);
      reorg1_entered_offset_mapper.notify_all();
      proceed.wait(false, std::memory_order_acquire);
      return payload;
    }));
  });

  reorg1_entered_offset_mapper.wait(false, std::memory_order_acquire);

  // "k" is only live in append_immutable_ now, so resolve()'s bypass must append a *new* entry
  // (with a different, non-inline stored hash) instead of updating the frozen one in place.
  REQUIRE(idx->put(to_span("k"), 222, /*is_inline=*/false) == InlineTestIndex::PutResult::Applied);

  proceed.store(true, std::memory_order_release);
  proceed.notify_all();
  reorg1.join();

  // sorted_region now has (k, inline hash); append_active_ separately holds the bypass-created
  // (k, non-inline hash) entry, still unmerged.
  CHECK(idx->get(to_span("k")) == 222U);

  // reorg #2 merges both generations; exactly one (the latest, 222) should survive if the merge
  // correctly recognizes them as the same logical key.
  std::vector<InlineTestIndex::EntrySnapshot> merged_out;
  idx->reorganize(
      per_entry_offset_mapper([](uint64_t payload, uint64_t /*hash*/) { return payload; }),
      [&](std::span<const InlineTestIndex::EntrySnapshot> merged) { merged_out.assign(merged.begin(), merged.end()); });

  const auto k_prefix = t1_detail::prefix_from_bytes(to_span("k"));
  const auto count_for_k = std::ranges::count(merged_out, k_prefix, &InlineTestIndex::EntrySnapshot::key);
  CHECK(count_for_k == 1);
  CHECK(idx->get(to_span("k")) == 222U);
}

// put()'s in-place update path (ResolvedSlot::store_hash()) mutates a live, published
// SortedSlot's atomic hash concurrently with reorganize()'s merge loop (and
// find_sorted()/get_with_hash()) reading it. Unlike the stress test above (which only inserts
// brand-new keys, never exercising store_hash()), this test updates pre-seeded sorted-region keys
// continuously while reorganize() runs concurrently. Best run under ThreadSanitizer.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("T1Index: concurrent updates to already-sorted keys survive racing reorganize") {
  auto idx = make_index();
  constexpr int key_count = 64;
  for (int i = 0; i < key_count; ++i) {
    REQUIRE(idx->put(to_span("k" + std::to_string(i)), 0) == TestIndex::PutResult::Applied);
  }
  // Moves every key into sorted_region, so put()'s in-place update reaches store_hash() on a
  // SortedSlot -- the path under test.
  idx->reorganize(per_entry_offset_mapper([](uint64_t payload, uint64_t /*hash*/) { return payload; }));
  REQUIRE(idx->append_size() == 0);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> reorganize_count{0};

  std::thread reorganizer([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      idx->reorganize(per_entry_offset_mapper([](uint64_t payload, uint64_t /*hash*/) { return payload; }));
      reorganize_count.fetch_add(1, std::memory_order_relaxed);
    }
  });

  constexpr int writer_thread_count = 4;
  constexpr int iterations_per_writer = 2000;
  std::vector<std::thread> writers;
  writers.reserve(writer_thread_count);
  for (int writer_id = 0; writer_id < writer_thread_count; ++writer_id) {
    writers.emplace_back([&, writer_id]() {
      for (int i = 0; i < iterations_per_writer; ++i) {
        const std::string key = "k" + std::to_string(i % key_count);
        const auto value = static_cast<uint64_t>((writer_id << 20) | i);
        CHECK(idx->put(to_span(key), value) == TestIndex::PutResult::Applied);
      }
    });
  }

  for (auto &writer : writers) {
    writer.join();
  }
  stop.store(true, std::memory_order_relaxed);
  reorganizer.join();

  CHECK(reorganize_count.load() > 0);
  for (int i = 0; i < key_count; ++i) {
    CHECK(idx->get(to_span("k" + std::to_string(i))) != vmemkv::STORE_NOT_FOUND);
  }
}

// resolve() returns "not found" for two unrelated reasons -- a key that's genuinely absent, and a
// deliberate bypass signal when the key's slot lives in a region a concurrent reorganize() just
// froze (see resolve()'s own comment on the immutable/sorted-frozen bypass). put()'s existing-key
// update path must distinguish them: tombstoning its own just-written value on the frozen-region
// case (rather than only on a genuine append-region hash-collision displacement) would briefly
// make get()/scan() report a live key as absent, in the window between the tombstone and the
// bypass insert landing. A dedicated reader thread polling get() is required to catch this:
// checking only after every writer/reorganizer thread joins (as the sibling "survive racing
// reorganize" tests above do) never observes the transient window.
TEST_CASE("T1Index: get() never observes a live key as absent while a concurrent update races reorganize") {
  auto idx = make_index();
  constexpr int key_count = 64;
  for (int i = 0; i < key_count; ++i) {
    REQUIRE(idx->put(to_span("k" + std::to_string(i)), 0) == TestIndex::PutResult::Applied);
  }

  std::atomic<bool> stop{false};
  std::atomic<bool> saw_missing{false};

  std::thread reorganizer([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      idx->reorganize(per_entry_offset_mapper([](uint64_t payload, uint64_t /*hash*/) { return payload; }));
    }
  });

  std::thread writer([&]() {
    uint64_t value = 1;
    while (!stop.load(std::memory_order_relaxed)) {
      for (int i = 0; i < key_count; ++i) {
        idx->put(to_span("k" + std::to_string(i)), value);
      }
      ++value;
    }
  });

  std::vector<std::thread> readers;
  for (int r = 0; r < 4; ++r) {
    readers.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < key_count; ++i) {
          if (idx->get(to_span("k" + std::to_string(i))) == vmemkv::STORE_NOT_FOUND) {
            saw_missing.store(true, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true, std::memory_order_relaxed);
  writer.join();
  reorganizer.join();
  for (auto &reader : readers) {
    reader.join();
  }

  CHECK_FALSE(saw_missing.load());
}

// reorganize()'s merge loop takes an earlier snapshot of an already-sorted key's SortedSlot value
// and publishes a brand-new SortedRegion built from it, discarding the old one; put()'s in-place
// path must route through sorted_write_frozen_ (see WriteFrozenTier's own comment) -- the same
// bypass append_immutable_ has for the symmetric case -- so a write landing between the snapshot
// and the publish is never lost. One dedicated writer per key (so "last write" is well-defined)
// races a hammering reorganizer; the sleep inside offset_mapper widens reorganize()'s
// merge-to-publish window to something close to what checkpoint_internal()'s real msync()/file
// I/O costs in production.
TEST_CASE("T1Index: dedicated single writer per key survives racing reorganize with exact last value") {
  auto idx = make_index();
  constexpr int key_count = 8;
  constexpr int updates_per_key = 500;
  for (int i = 0; i < key_count; ++i) {
    REQUIRE(idx->put(to_span("k" + std::to_string(i)), 0) == TestIndex::PutResult::Applied);
  }
  idx->reorganize(per_entry_offset_mapper([](uint64_t payload, uint64_t) { return payload; }));

  std::atomic<bool> stop{false};
  std::thread reorganizer([&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      idx->reorganize(per_entry_offset_mapper([](uint64_t payload, uint64_t) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        return payload;
      }));
    }
  });

  std::vector<std::thread> writers;
  writers.reserve(key_count);
  for (int i = 0; i < key_count; ++i) {
    writers.emplace_back([&, i]() {
      for (int round = 1; round <= updates_per_key; ++round) {
        const auto value = static_cast<uint64_t>((i << 20) | round);
        REQUIRE(idx->put(to_span("k" + std::to_string(i)), value) == TestIndex::PutResult::Applied);
      }
    });
  }
  for (auto &writer : writers) {
    writer.join();
  }
  stop.store(true, std::memory_order_relaxed);
  reorganizer.join();

  for (int i = 0; i < key_count; ++i) {
    const auto expected = static_cast<uint64_t>((i << 20) | updates_per_key);
    CHECK(idx->get(to_span("k" + std::to_string(i))) == expected);
  }
}

// resolve()'s immutable-region bypass makes every concurrent put() for a key parked in
// append_immutable_ take the "not found, insert new" path with no re-check against each other.
// LockFreeHashTable::publish_slot() must retire the slot a later racer's insert displaces from the
// hash index, or N racers on the same key would leave N-1 orphaned-but-live duplicate
// AppendSlots -- unreachable via get(), but still collected by collect_live_entries() into every
// future checkpoint, forever.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("T1Index: concurrent puts racing the same immutable-bypass window collapse to one entry") {
  // The race window is narrower than "reorganize() is paused": once any racer publishes its slot,
  // later racers' resolve() finds it and updates in place instead of hitting the bypass. A
  // spin-wait barrier plus many racers makes it likely several truly overlap; repeating with a
  // fresh key each round compounds that.
  constexpr int rounds = 8;
  constexpr int racer_count = 32;

  for (int round = 0; round < rounds; ++round) {
    auto idx = make_index();
    const std::string hot_key = "hot" + std::to_string(round);
    REQUIRE(idx->put(to_span(hot_key), 0) == TestIndex::PutResult::Applied);

    // reorg #1 freezes the key into append_immutable_ and blocks inside offset_mapper, giving every
    // racer below a window where resolve() is guaranteed to return not-found.
    std::atomic<bool> reorg1_entered_offset_mapper{false};
    std::atomic<bool> proceed{false};
    std::thread reorg1([&] {
      idx->reorganize(per_entry_offset_mapper([&](uint64_t payload, uint64_t /*hash*/) {
        reorg1_entered_offset_mapper.store(true, std::memory_order_release);
        reorg1_entered_offset_mapper.notify_all();
        proceed.wait(false, std::memory_order_acquire);
        return payload;
      }));
    });
    reorg1_entered_offset_mapper.wait(false, std::memory_order_acquire);

    std::atomic<int> ready_count{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> racers;
    racers.reserve(racer_count);
    for (int i = 0; i < racer_count; ++i) {
      racers.emplace_back([&, i]() {
        ready_count.fetch_add(1, std::memory_order_acq_rel);
        while (!go.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        CHECK(idx->put(to_span(hot_key), static_cast<uint64_t>(i + 1)) == TestIndex::PutResult::Applied);
      });
    }
    while (ready_count.load(std::memory_order_acquire) < racer_count) {
      std::this_thread::yield();
    }
    go.store(true, std::memory_order_release);
    for (auto &racer : racers) {
      racer.join();
    }

    proceed.store(true, std::memory_order_release);
    proceed.notify_all();
    reorg1.join();

    // append_active_ now holds up to racer_count bypass-created entries for hot_key -- at most one
    // may survive as live; the rest must have been retired as orphaned duplicates.
    CHECK(idx->get(to_span(hot_key)) != vmemkv::STORE_NOT_FOUND);

    std::vector<TestIndex::EntrySnapshot> merged_out;
    idx->reorganize(
        per_entry_offset_mapper([](uint64_t payload, uint64_t /*hash*/) { return payload; }),
        [&](std::span<const TestIndex::EntrySnapshot> merged) { merged_out.assign(merged.begin(), merged.end()); });

    const auto hot_prefix = t1_detail::prefix_from_bytes(to_span(hot_key));
    const auto count_for_hot = std::ranges::count(merged_out, hot_prefix, &TestIndex::EntrySnapshot::key);
    CHECK(count_for_hot == 1);
  }
}
