// test_defrag.cpp -- T2 defragment (storage reclamation) tests.
//
// Uses Baseline (all-offset, no inline) with a 64MiB T2 capacity (8 segments) so segment
// accounting, victim selection, relocation, and hole-punching are all exercisable at small
// scale. 1KiB/2KiB values keep fsync counts modest while still freezing whole segments.

#include <doctest/doctest.h>
#include <sys/stat.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <vmemkv/vmemkv.hpp>

#include "test_support.hpp"

namespace {

using Store = vmemkv::StoreAdapter<vmemkv::VMemKVImpl<vmemkv::Config<>>>;
constexpr uint64_t kTestCapacityBytes = 64ULL * 1024 * 1024;

auto make_store(const std::filesystem::path &path) -> std::unique_ptr<Store> {
  return std::make_unique<Store>(path, kTestCapacityBytes);
}

auto make_key(size_t index) -> std::string { return "k" + std::to_string(100000 + index); }

auto pattern_value(size_t value_bytes, size_t index) -> std::string {
  std::string value(value_bytes, '\0');
  uint64_t state = static_cast<uint64_t>(index) * 0x9E3779B97F4A7C15ULL + 1;
  for (size_t off = 0; off < value_bytes; off += sizeof(uint64_t)) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    std::memcpy(value.data() + off, &state, std::min(sizeof(uint64_t), value_bytes - off));
  }
  return value;
}

// Record math shared with the implementation: header 20B, 8-byte aligned records, 16-byte
// hint granularity in T1 payloads.
auto hint_bytes(size_t key_bytes, size_t value_bytes) -> uint64_t {
  const uint64_t aligned = (sizeof(ValueRecordHeader) + key_bytes + value_bytes + 7) & ~uint64_t{7};
  return (aligned / 16) * 16;
}

auto file_blocks(const std::filesystem::path &path) -> uint64_t {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(st.st_blocks);
}

}  // namespace

TEST_CASE("defrag accounting tracks append, overwrite, and delete exactly") {
  const auto path = vmemkv_test::ScopedTempPath("vmemkv_defrag", vmemkv_test::remove_store_files);
  auto store = make_store(path);
  constexpr size_t kKeys = 1000;
  constexpr size_t kVal1k = 1024;
  constexpr size_t kVal2k = 2048;
  const uint64_t per_record = hint_bytes(make_key(0).size(), kVal1k);

  store->bulk_load(kKeys, [](size_t i) { return make_key(i); }, [](size_t i) { return pattern_value(kVal1k, i); });
  CHECK(store->get_statistics().t2_live_bytes == kKeys * per_record);

  // Same-size updates go in place: accounting unchanged.
  for (size_t i = 0; i < 100; ++i) {
    CHECK(store->update(make_key(i), pattern_value(kVal1k, 100000 + i)));
  }
  CHECK(store->get_statistics().t2_live_bytes == kKeys * per_record);

  // Growing updates take the append path: old hint out, new hint in.
  const uint64_t per_record_big = hint_bytes(make_key(0).size(), kVal2k);
  for (size_t i = 0; i < 100; ++i) {
    CHECK(store->update(make_key(i), pattern_value(kVal2k, 200000 + i)));
  }
  CHECK(store->get_statistics().t2_live_bytes == (kKeys - 100) * per_record + 100 * per_record_big);

  // Deletes drop their hints.
  for (size_t i = 0; i < 100; ++i) {
    CHECK(store->remove(make_key(500 + i)));
  }
  CHECK(store->get_statistics().t2_live_bytes == (kKeys - 200) * per_record + 100 * per_record_big);
}

TEST_CASE("defrag cycle relocates, punches, and preserves every live record") {
  const auto path = vmemkv_test::ScopedTempPath("vmemkv_defrag", vmemkv_test::remove_store_files);
  auto store = make_store(path);
  // 4100 x ~2KiB records ~= 8.6MiB: segment 0 frozen full, segment 1 partial after checkpoint.
  constexpr size_t kKeys = 4100;
  constexpr size_t kValBytes = 2048;
  constexpr size_t kDelete = 2870;  // 70%: first segment goes majority-garbage.
  store->bulk_load(kKeys, [](size_t i) { return make_key(i); }, [](size_t i) { return pattern_value(kValBytes, i); });
  store->checkpoint();
  for (size_t i = 0; i < kDelete; ++i) {
    CHECK(store->remove(make_key(i)));
  }
  const uint64_t blocks_loaded = file_blocks(vmemkv::derive_t2_chk_path(path));
  auto stats0 = store->get_statistics();
  MESSAGE("after load+checkpoint+delete: blocks=" << blocks_loaded << " live=" << stats0.t2_live_bytes);
  const uint64_t blocks_before = blocks_loaded;

  bool punched_observed = false;
  for (int attempt = 0; attempt < 4 && !punched_observed; ++attempt) {
    CHECK(store->defragment());
    {
      auto st = store->get_statistics();
      MESSAGE("cycle: moved=" << st.last_defrag_moved_bytes << " punched=" << st.last_defrag_punched_bytes << " cycles="
                              << st.defrag_cycle_count << " blocks=" << file_blocks(vmemkv::derive_t2_chk_path(path)));
      if (st.last_defrag_punched_bytes > 0) {
        punched_observed = true;
      }
    }
  }
  MESSAGE("final blocks=" << file_blocks(vmemkv::derive_t2_chk_path(path)) << " before=" << blocks_before);

  // Every survivor reads back exactly (relocated or not).
  for (size_t i = kDelete; i < kKeys; ++i) {
    const auto got = vmemkv_test::get_optional_bytes(store, make_key(i));
    REQUIRE(got.has_value());
    CHECK(vmemkv_test::span_to_string(vmemkv_test::as_span(*got)) == pattern_value(kValBytes, i));
  }
  for (size_t i = 0; i < kDelete; ++i) {
    CHECK_FALSE(vmemkv_test::get_optional_bytes(store, make_key(i)).has_value());
  }
  // Live accounting is exact regardless of who moved what (including the background worker).
  const uint64_t per_record = hint_bytes(make_key(0).size(), kValBytes);
  CHECK(store->get_statistics().t2_live_bytes == (kKeys - kDelete) * per_record);

  if (!punched_observed) {
    MESSAGE("hole punch unsupported on this filesystem; physical-reclamation assertions skipped");
  } else {
    CHECK(file_blocks(vmemkv::derive_t2_chk_path(path)) < blocks_before);
  }
}

TEST_CASE("defrag state survives checkpoint plus restart") {
  const auto path = vmemkv_test::ScopedTempPath("vmemkv_defrag", vmemkv_test::remove_store_files);
  constexpr size_t kKeys = 4100;
  constexpr size_t kValBytes = 2048;
  constexpr size_t kDelete = 2870;
  {
    auto store = make_store(path);
    store->bulk_load(kKeys, [](size_t i) { return make_key(i); }, [](size_t i) { return pattern_value(kValBytes, i); });
    store->checkpoint();
    for (size_t i = 0; i < kDelete; ++i) {
      CHECK(store->remove(make_key(i)));
    }
    CHECK(store->defragment());
    store->checkpoint();
  }
  {
    auto store = make_store(path);
    for (size_t i = kDelete; i < kKeys; ++i) {
      const auto got = vmemkv_test::get_optional_bytes(store, make_key(i));
      REQUIRE(got.has_value());
      CHECK(vmemkv_test::span_to_string(vmemkv_test::as_span(*got)) == pattern_value(kValBytes, i));
    }
    const uint64_t per_record = hint_bytes(make_key(0).size(), kValBytes);
    CHECK(store->get_statistics().t2_live_bytes == (kKeys - kDelete) * per_record);
  }
}

TEST_CASE("defrag cycle relocating more than the WAL ring holds still completes") {
  // Bulk past one full 8MiB segment, delete 60% (every victim segment clears the 50%
  // garbage bar), freeze with a checkpoint, then relocate ~12k live records in one
  // cycle -- well past the 4096-slot WAL ring. Reserves must drain incrementally: with
  // no other thread in await_durable(), a whole-cycle batch wedges reserve() forever.
  const auto path = vmemkv_test::ScopedTempPath("vmemkv_defrag", vmemkv_test::remove_store_files);
  auto store = make_store(path);
  constexpr size_t kKeys = 30000;
  constexpr size_t kValBytes = 256;
  store->bulk_load(kKeys, [](size_t i) { return make_key(i); }, [](size_t i) { return pattern_value(kValBytes, i); });
  for (size_t i = 0; i < kKeys; ++i) {
    if (i % 10 < 6) {
      CHECK(store->remove(make_key(i)));
    }
  }
  store->checkpoint();
  const uint64_t cycles_before = store->get_statistics().defrag_cycle_count;
  CHECK(store->defragment());
  CHECK(store->get_statistics().defrag_cycle_count > cycles_before);
  for (size_t i = 0; i < kKeys; i += 97) {
    const auto got = vmemkv_test::get_optional_bytes(store, make_key(i));
    if (i % 10 < 6) {
      CHECK_FALSE(got.has_value());
    } else {
      REQUIRE(got.has_value());
      CHECK(vmemkv_test::span_to_string(vmemkv_test::as_span(*got)) == pattern_value(kValBytes, i));
    }
  }
}

TEST_CASE("defrag runs concurrently with updates without losing writes") {
  const auto path = vmemkv_test::ScopedTempPath("vmemkv_defrag", vmemkv_test::remove_store_files);
  auto store = make_store(path);
  constexpr size_t kKeys = 2000;
  constexpr size_t kValBytes = 256;
  store->bulk_load(kKeys, [](size_t i) { return make_key(i); }, [](size_t i) { return pattern_value(kValBytes, i); });
  store->checkpoint();

  constexpr int kWriters = 4;
  // 10 rounds (20k updates) keep several seconds of overlap with the background
  // worker's 1s poll plus the manual cycles below; larger counts only add
  // fsync-bound wall time (each update awaits WAL durability) without new races.
  constexpr int kRounds = 10;
  std::vector<std::thread> writers;
  for (int t = 0; t < kWriters; ++t) {
    writers.emplace_back([&, t] {
      for (int r = 0; r < kRounds; ++r) {
        for (size_t i = static_cast<size_t>(t); i < kKeys; i += kWriters) {
          // Same-size updates (in-place path) plus a steady drizzle of deletes for garbage.
          if (i % 7 == 0 && r == 0) {
            store->remove(make_key(i));
          } else {
            store->update(make_key(i), pattern_value(kValBytes, 1000000 + static_cast<size_t>(t) * 10000 + r));
          }
        }
      }
    });
  }
  // Force several cycles while writers run; single-flight serializes with the background worker.
  for (int i = 0; i < 6; ++i) {
    store->defragment();
  }
  for (auto &th : writers) {
    th.join();
  }
  CHECK(store->defragment());

  // Deleted keys stay absent; every other key holds exactly its writer's last value.
  for (size_t i = 0; i < kKeys; ++i) {
    const int owner = static_cast<int>(i % kWriters);
    const auto got = vmemkv_test::get_optional_bytes(store, make_key(i));
    if (i % 7 == 0) {
      CHECK_FALSE(got.has_value());
    } else {
      REQUIRE(got.has_value());
      CHECK(vmemkv_test::span_to_string(vmemkv_test::as_span(*got)) ==
            pattern_value(kValBytes, 1000000 + static_cast<size_t>(owner) * 10000 + (kRounds - 1)));
    }
  }
}
