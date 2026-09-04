// test_kv_store.cpp — Correctness tests for store implementations.
//
// Each scenario is an independent TEST_CASE_TEMPLATE instantiated for every
// store type.  A fresh store is constructed per TEST_CASE (via StoreFactory)
// so tests are fully isolated.

#include <doctest/doctest.h>
#include <unistd.h>

#include <atomic>
#include <checkpoint/checkpoint.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <rivals/rocksdb_store.hpp>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <vector>
#include <vmemkv/vmemkv.hpp>

#include "test_support.hpp"

// Verify that all major variants satisfy the C++20 KVStore concept
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKV_Baseline>);
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKVStore>);
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKV_RocksDB>);
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKV_RocksDBBlobDB>);
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKV_LMDB>);

namespace test_util {
template <typename StorePtr, typename Key>
auto get_sync(const StorePtr &store, const Key &key) -> uint64_t {
  uint64_t res = vmemkv::STORE_NOT_FOUND;
  store->get(key, [&](std::span<const std::byte> val) {
    res = 0;
    std::memcpy(&res, val.data(), std::min(val.size(), sizeof(uint64_t)));
  });
  return res;
}

// Decodes a scanned raw value span back to uint64_t (stored little-endian by ValueSerializer;
// see serializer.hpp).
inline auto decode_scanned_u64(std::span<const std::byte> val) -> uint64_t {
  uint64_t res = 0;
  std::memcpy(&res, val.data(), std::min(val.size(), sizeof(uint64_t)));
  return res;
}

template <typename StorePtr, typename Key>
auto get_bytes_sync(const StorePtr &store, const Key &key) -> std::optional<std::vector<std::byte>> {
  return vmemkv_test::get_optional_bytes(store, key);
}
}  // namespace test_util

namespace {
constexpr uint32_t kTestByteMask = 0xffU;
constexpr int kCustomKeyId = 42;
constexpr int kCustomKeyCategoryBase = 7;
constexpr uint64_t kRemovedValue = 7;
constexpr uint64_t kSmallValue = 100;
constexpr uint64_t kLargeValue = 200;
constexpr std::size_t kValue64Bytes = 64;
constexpr std::size_t kValue100Bytes = 100;
constexpr std::size_t kValue200Bytes = 200;
constexpr std::size_t kInlineValueBytes = 8;
constexpr std::size_t kShortInlineValueBytes = 5;
constexpr int kBitsPerByte = 8;
constexpr std::array<std::byte, kInlineValueBytes> kCustomKeySerializedTemplate{};
constexpr unsigned int kScanStart = 0U;
constexpr unsigned int kScanTen = 10U;
constexpr unsigned int kScanHundred = 100U;
constexpr unsigned int kScanUpperBound = 1000U;
constexpr std::size_t kLongValueBytes = 12;
}  // namespace

static auto as_string(const std::vector<std::byte> &bytes) -> std::string {
  if (bytes.empty()) {
    return {};
  }
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

template <typename Store>
struct StoreFactory;

// Helper to reserve temp path for T2 files
static auto reserve_temp_path() -> std::filesystem::path {
  const std::filesystem::path temp_path =
      vmemkv_test::reserve_unique_temp_path("vmemkv_kv", /*also_remove_wal_sibling=*/true);
  // Also clear leftover .manifest/.chkN.t1/.chkN.t2 files from a reused PID (seen when relaunching
  // this binary in a tight loop lets a later run adopt an earlier run's stale checkpoint via
  // load_checkpoint_if_present()). A single full suite run never hits this; cheap insurance.
  std::error_code ignored;
  const std::string manifest_prefix = temp_path.filename().string() + ".";
  for (const auto &entry : std::filesystem::directory_iterator(temp_path.parent_path(), ignored)) {
    if (entry.path().filename().string().starts_with(manifest_prefix)) {
      std::filesystem::remove(entry.path(), ignored);
    }
  }
  return temp_path;
}

template <typename Impl>
struct VMemKVDeleter {
  std::filesystem::path path;
  void operator()(vmemkv::StoreAdapter<Impl> *store_ptr) const noexcept {
    delete store_ptr;
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
    vmemkv::remove_wal_segments(vmemkv::derive_wal_path(path));
  }
};

template <typename Impl>
struct StoreFactory<vmemkv::StoreAdapter<Impl>> {
  static constexpr uint64_t kCapacityBytes = 8U << 20;  // 8 MiB
  // T1's own (pskiplist-backed) capacity: VMemKVImpl defaults this to a production-scale 4TB,
  // which is wasteful and slow to construct hundreds of times over in a test binary -- override
  // to something proportional to this factory's own tiny kCapacityBytes instead.
  static constexpr uint64_t kT1CapacityBytes = 16U << 20;  // 16 MiB

  static auto make() -> std::unique_ptr<vmemkv::StoreAdapter<Impl>, VMemKVDeleter<Impl>> {
    std::filesystem::path path = reserve_temp_path();
    if constexpr (std::is_same_v<Impl, ::RocksDBStore> || std::is_same_v<Impl, ::RocksDBBlobDBStore> ||
                  std::is_same_v<Impl, ::LMDBStore>) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      auto *store = new vmemkv::StoreAdapter<Impl>(path.string());
      return std::unique_ptr<vmemkv::StoreAdapter<Impl>, VMemKVDeleter<Impl>>(store, VMemKVDeleter<Impl>{path});
    } else {
      auto *store = new vmemkv::StoreAdapter<Impl>(path, kCapacityBytes, kT1CapacityBytes);
      return std::unique_ptr<vmemkv::StoreAdapter<Impl>, VMemKVDeleter<Impl>>(store, VMemKVDeleter<Impl>{path});
    }
  }
};

// ─── Store type lists ───────────────────────────────────────────────────────

// VMemKV 自体のバリエーション（Baseline, Cumulative Steps, Ablations, Inlining）
#define VMemKVStores \
  vmemkv::variants::VMemKV_Var0_Baseline, vmemkv::variants::VMemKV_Var1_Bloom, vmemkv::variants::VMemKV_Var2_Inline

// 競合バックエンドのバリエーション（RocksDBStore, LMDBStoreなど）
#ifdef ENABLE_ROCKSDB
#define RocksDBRivalStores , vmemkv::variants::VMemKV_RocksDB, vmemkv::variants::VMemKV_RocksDBBlobDB
#else
#define RocksDBRivalStores
#endif

#ifdef ENABLE_LMDB
#define LMDBRivalStores , vmemkv::variants::VMemKV_LMDB
#else
#define LMDBRivalStores
#endif

#define RivalStores RocksDBRivalStores LMDBRivalStores

#define STORE_TYPES VMemKVStores RivalStores
#define LONG_KEY_STORE_TYPES STORE_TYPES
#define LARGE_VALUE_STORE_TYPES STORE_TYPES

struct ReorgEveryWriteConfig : vmemkv::Config<> {
  // Partial override style: inherit all defaults and only tune reorg aggressiveness.
  static constexpr size_t T1ReorganizeSoftThresholdPercent = 1;
  static constexpr size_t T1ReorganizeHardThresholdPercent = 1;
};

static_assert(ReorgEveryWriteConfig::T1AppendCapacityEntries ==
              (size_t{1} << ReorgEveryWriteConfig::T1AppendCapacityLog2));

using VMemKV_ReorgEveryWrite = vmemkv::StoreAdapter<vmemkv::VMemKVImpl<ReorgEveryWriteConfig>>;

template <typename StoreHandle>
static void insert_sequential_u64_values(StoreHandle &store, int key_count) {
  for (int i = 0; i < key_count; ++i) {
    CHECK(store->insert("k" + std::to_string(i), static_cast<uint64_t>(i)));
  }
}

template <typename StoreHandle>
static void check_sequential_u64_values(StoreHandle &store, int key_count) {
  for (int i = 0; i < key_count; ++i) {
    CHECK(test_util::get_sync(store, "k" + std::to_string(i)) == static_cast<uint64_t>(i));
  }
}

template <typename StoreHandle>
static void check_hot_key_upgrade_is_visible(StoreHandle &store) {
  CHECK(store->insert("hot", std::string("a")));
  const std::string large_value(kValue64Bytes, 'x');
  CHECK(store->update("hot", large_value));
  const auto got = test_util::get_bytes_sync(store, "hot");
  REQUIRE(got.has_value());
  if (!got.has_value()) {
    return;
  }
  CHECK(as_string(got.value()) == large_value);
}

// ─── Test cases (one per scenario) ───────────────────────────────────────────

TEST_CASE_TEMPLATE("get on empty store returns STORE_NOT_FOUND", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  CHECK(test_util::get_sync(store, "x") == vmemkv::STORE_NOT_FOUND);
}

TEST_CASE_TEMPLATE("insert and get", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  CHECK(store->insert("a", 10));
  CHECK(test_util::get_sync(store, "a") == 10U);
}

TEST_CASE("sync reorganize-before-write is transparent") {
  auto store = StoreFactory<VMemKV_ReorgEveryWrite>::make();

  constexpr int key_count = 25000;
  insert_sequential_u64_values(store, key_count);

  // This update path requires append and should stay transparent even when
  // pre-write reorganize triggers very frequently.
  check_hot_key_upgrade_is_visible(store);

  check_sequential_u64_values(store, key_count);
}

TEST_CASE("partial config inheritance keeps required append-capacity fields") {
  auto store = StoreFactory<VMemKV_ReorgEveryWrite>::make();
  CHECK(ReorgEveryWriteConfig::T1AppendCapacityEntries == vmemkv::Config<>::T1AppendCapacityEntries);
  CHECK(store->insert("partial_cfg", 1));
  CHECK(test_util::get_sync(store, "partial_cfg") == 1U);
}

TEST_CASE_TEMPLATE("insert duplicate returns false, value unchanged", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  CHECK(store->insert("a", 10));
  CHECK_FALSE(store->insert("a", 99));
  CHECK(test_util::get_sync(store, "a") == 10U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE_TEMPLATE("integral keys use the templated convenience API", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  constexpr int key = 42;

  CHECK(store->insert(key, 10));
  CHECK(test_util::get_sync(store, key) == 10U);
  CHECK(store->update(key, 11));
  CHECK(test_util::get_sync(store, key) == 11U);
  CHECK(store->remove(key));
  CHECK(test_util::get_sync(store, key) == vmemkv::STORE_NOT_FOUND);
}

namespace test_adl {
struct CustomKey {
  int id;
  int category;
};

inline auto serialize_kvs(const CustomKey &custom_key) noexcept
    -> std::array<std::byte, kCustomKeySerializedTemplate.size()> {
  auto out = kCustomKeySerializedTemplate;
  auto uid = static_cast<uint32_t>(custom_key.id);
  auto ucat = static_cast<uint32_t>(custom_key.category);
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<std::byte>((uid >> ((3 - i) * kBitsPerByte)) & kTestByteMask);
    out[i + 4] = static_cast<std::byte>((ucat >> ((3 - i) * kBitsPerByte)) & kTestByteMask);
  }
  return out;
}
}  // namespace test_adl

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE_TEMPLATE("custom serializers (ADL) for user-defined types", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  test_adl::CustomKey key_one{kCustomKeyId, kCustomKeyCategoryBase};
  test_adl::CustomKey key_two{kCustomKeyId, kCustomKeyCategoryBase + 1};

  CHECK(store->insert(key_one, kSmallValue));
  CHECK(store->insert(key_two, kLargeValue));

  CHECK(test_util::get_sync(store, key_one) == kSmallValue);
  CHECK(test_util::get_sync(store, key_two) == kLargeValue);

  std::vector<uint64_t> results;
  const size_t scan_count =
      store->scan(key_one, key_two, [&](std::span<const std::byte>, std::span<const std::byte> value) {
        results.push_back(test_util::decode_scanned_u64(value));
      });
  REQUIRE(scan_count == 2U);
  REQUIRE(results.size() == 2);
  CHECK(results[0] == kSmallValue);
  CHECK(results[1] == kLargeValue);
}

// Regression test: a value that's bit-for-bit identical to T1's STORE_NOT_FOUND sentinel
// (~0ULL) used to be rejected outright by StoreAdapter::insert()/update() -- necessary at the
// time because inlining it would have made the entry indistinguishable from "not found" on every
// read path, but that rejection never covered bulk_load() (a real bug, fixed separately). Now
// that VMemKVImpl::try_make_inline_payload() itself declines to inline this exact value (routing
// it through the ordinary T2-record path instead, whose payload is an offset, never the raw value
// bytes), the value is fully storable and retrievable like any other -- verifies that here.
TEST_CASE_TEMPLATE("insert accepts a value equal to STORE_NOT_FOUND and it round-trips", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  CHECK(store->insert("a", vmemkv::STORE_NOT_FOUND));
  bool found = store->get("a", [](std::span<const std::byte> /*val*/) {});
  CHECK(found);
  CHECK(test_util::get_sync(store, "a") == vmemkv::STORE_NOT_FOUND);
}

TEST_CASE_TEMPLATE("update existing key", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  store->insert("a", 1);
  CHECK(store->update("a", 2));
  CHECK(test_util::get_sync(store, "a") == 2U);
}

TEST_CASE_TEMPLATE("update missing key returns false", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  CHECK_FALSE(store->update("z", 1));
}

// Same as the insert case above, for update().
TEST_CASE_TEMPLATE("update accepts a value equal to STORE_NOT_FOUND and it round-trips", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  store->insert("a", 1);
  CHECK(store->update("a", vmemkv::STORE_NOT_FOUND));
  CHECK(test_util::get_sync(store, "a") == vmemkv::STORE_NOT_FOUND);
}

// Regression test for the actual bug: unlike insert()/update(), StoreAdapter::bulk_load() never
// went through the sentinel-rejection guard above (it calls impl_.bulk_load_impl() directly), and
// VMemKVImpl::try_make_inline_payload()/T1Index::put() had no equivalent check of their own --
// so a bulk-loaded entry whose 8-byte value happened to equal STORE_NOT_FOUND was silently
// inlined and became permanently invisible to both get() and scan() (indistinguishable from a
// tombstone). Exercises the fix directly through bulk_load(), on the one variant where inlining
// is actually possible.
TEST_CASE("bulk_load: an entry whose value equals STORE_NOT_FOUND round-trips via get() and scan()") {
  auto store = StoreFactory<vmemkv::variants::VMemKV_Var2_Inline>::make();
  const std::string all_ff_value(8, '\xFF');
  store->bulk_load(
      1, [](std::size_t /*index*/) { return std::string("a"); }, [&](std::size_t /*index*/) { return all_ff_value; });

  bool found = store->get("a", [](std::span<const std::byte> /*val*/) {});
  CHECK(found);
  CHECK(test_util::get_sync(store, "a") == vmemkv::STORE_NOT_FOUND);

  size_t scan_hits = 0;
  std::ignore = store->scan("a", "a~", [&](std::span<const std::byte> /*key*/, std::span<const std::byte> value) {
    REQUIRE(value.size() == sizeof(uint64_t));
    uint64_t val_u64 = 0;
    std::memcpy(&val_u64, value.data(), sizeof(uint64_t));
    CHECK(val_u64 == vmemkv::STORE_NOT_FOUND);
    ++scan_hits;
  });
  CHECK(scan_hits == 1U);
}

TEST_CASE_TEMPLATE("remove existing key", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  store->insert("a", kRemovedValue);
  CHECK(store->remove("a"));
  CHECK(test_util::get_sync(store, "a") == vmemkv::STORE_NOT_FOUND);
}

TEST_CASE_TEMPLATE("remove missing key returns false", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  CHECK_FALSE(store->remove("z"));
}

TEST_CASE_TEMPLATE("re-insert after remove", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  store->insert("a", 1);
  store->remove("a");
  CHECK(store->insert("a", 2));
  CHECK(test_util::get_sync(store, "a") == 2U);
}

TEST_CASE_TEMPLATE("scan empty range returns 0", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  size_t entry_count = 0;
  store->insert("b", 1);
  entry_count = store->scan("a", "a", [](std::span<const std::byte>, std::span<const std::byte>) {});
  CHECK(entry_count == 0U);
}

TEST_CASE_TEMPLATE("scan returns all entries in range", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  store->insert("b", 2);
  store->insert("c", 3);
  store->insert("d", 4);
  std::unordered_set<uint64_t> values;
  size_t entry_count = store->scan("b", "d", [&](std::span<const std::byte>, std::span<const std::byte> value) {
    values.insert(test_util::decode_scanned_u64(value));
  });
  CHECK(entry_count == 3U);
  CHECK(values.count(2) == 1);
  CHECK(values.count(3) == 1);
  CHECK(values.count(4) == 1);
}

TEST_CASE_TEMPLATE("scan excludes removed entries", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  store->insert("a", 1);
  store->insert("b", 2);
  store->remove("a");
  const size_t entry_count = store->scan("a", "b", [](std::span<const std::byte>, std::span<const std::byte>) {});
  CHECK(entry_count == 1U);
}

TEST_CASE_TEMPLATE("reorganize: CRUD still works", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  store->insert("a", 1);
  store->insert("b", 2);
  store->reorganize();
  CHECK(test_util::get_sync(store, "a") == 1U);
  CHECK(test_util::get_sync(store, "b") == 2U);
  CHECK(store->insert("c", 3));
  const size_t entry_count = store->scan("a", "c", [](std::span<const std::byte>, std::span<const std::byte>) {});
  CHECK(entry_count == 3U);
}

// Regression test: a same-size update targeting a record in T2's "base" region (already
// checkpointed) must be redirected out-of-place instead of taking the in-place fast path -- see
// update_impl()'s base_boundary check and T2Memory::base_boundary's declaration. Base-region reads
// go through a seqlock-free path (7.9 節) that assumes the bytes never change again once a record
// is base-resident; an in-place write there would violate that assumption. If the redirect were
// missing or wrong, this test would observe Scan (base_mmap_scan-served) and Get (main-mmap-served)
// silently disagreeing on "key_a"'s value.
TEST_CASE("VMemKV: update after reorganize redirects out-of-place, Scan sees fresh value") {
  auto store = StoreFactory<vmemkv::variants::VMemKV_Baseline>::make();

  const std::string value_a(kValue200Bytes, 'a');
  const std::string value_b(kValue200Bytes, 'b');
  REQUIRE(store->insert("key_a", value_a));
  REQUIRE(store->insert("key_b", value_b));

  store->checkpoint();  // Advances base_boundary: both records now sit in the base region.

  // Same-size update: would take the in-place fast path if key_a's offset weren't in the base.
  const std::string value_a_updated(kValue200Bytes, 'A');
  REQUIRE(store->update("key_a", value_a_updated));

  std::map<std::string, std::string> seen;
  std::ignore = store->scan("key_a", "key_c", [&](std::span<const std::byte> key, std::span<const std::byte> value) {
    seen.emplace(std::string(reinterpret_cast<const char *>(key.data()), key.size()),
                 std::string(reinterpret_cast<const char *>(value.data()), value.size()));
  });

  REQUIRE(seen.count("key_a") == 1);
  CHECK(seen.at("key_a") == value_a_updated);
  REQUIRE(seen.count("key_b") == 1);
  CHECK(seen.at("key_b") == value_b);

  // Get (main mmap path) must agree with Scan (base_mmap_scan path) on the same key.
  const auto get_result = test_util::get_bytes_sync(store, "key_a");
  REQUIRE(get_result.has_value());
  CHECK(std::string(reinterpret_cast<const char *>(get_result->data()), get_result->size()) == value_a_updated);

  // A second checkpoint should absorb the out-of-place update into a new base region, and a
  // further same-size update should again be redirected rather than corrupting the new base.
  store->checkpoint();
  const std::string value_a_updated2(kValue200Bytes, 'Z');
  REQUIRE(store->update("key_a", value_a_updated2));
  std::map<std::string, std::string> seen2;
  std::ignore = store->scan("key_a", "key_c", [&](std::span<const std::byte> key, std::span<const std::byte> value) {
    seen2.emplace(std::string(reinterpret_cast<const char *>(key.data()), key.size()),
                  std::string(reinterpret_cast<const char *>(value.data()), value.size()));
  });
  REQUIRE(seen2.count("key_a") == 1);
  CHECK(seen2.at("key_a") == value_a_updated2);
}

// Stress/regression test: update() (base-region redirect decision) races scan() (base_mmap_scan
// read path) races repeated checkpoint() (moves base_boundary in place, no remap) -- the
// three-way race the base/tail split's correctness depends on. Run under
// ThreadSanitizer for direct race detection; also self-checks independent of TSan, since every
// value written here is `kStressValueBytes` copies of one repeated character, so a torn read shows
// up directly as a non-uniform byte value.
//
// kReorgCycles is capped well below what a "why not more?" instinct would suggest: this store's
// underlying reorganize_internal() has a separate, pre-existing, already-documented race (see its
// "KNOWN OPEN ISSUE" assert and test_kv_store's own deliberately-failing repro for it) that
// sustained concurrent reorganize+write pressure can trip. 60 cycles cleared 8/8 TSan runs without
// tripping it; raising this constant is likely to make this test flaky on that unrelated, unfixed
// issue rather than exercise more of the code this test actually targets.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("VMemKV: concurrent update+scan survive repeated checkpoint (stress)") {
  using TestStore = vmemkv::variants::VMemKV_Baseline;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  constexpr int kKeyCount = 50;
  constexpr std::size_t kStressValueBytes = 200;  // Non-inline; see other tests' same-size note.
  constexpr int kReorgCycles = 60;

  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), kStoreCapacityBytes);

  for (int i = 0; i < kKeyCount; ++i) {
    REQUIRE(store->insert("key" + std::to_string(i), std::string(kStressValueBytes, 'a')));
  }

  std::atomic<bool> stop{false};
  std::atomic<bool> corruption_found{false};

  std::thread updater([&] {
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> key_dist(0, kKeyCount - 1);
    uint64_t i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      const std::string key = "key" + std::to_string(key_dist(rng));
      const std::string value(kStressValueBytes, static_cast<char>('a' + (i++ % 26)));
      store->update(key, value);
    }
  });

  std::thread scanner([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      std::ignore =
          store->scan("key0", "key9", [&](std::span<const std::byte> /*key*/, std::span<const std::byte> value) {
            if (value.size() != kStressValueBytes) {
              corruption_found.store(true, std::memory_order_relaxed);
              return;
            }
            const auto expected = value[0];
            for (std::byte b : value) {
              if (b != expected) {
                corruption_found.store(true, std::memory_order_relaxed);
                break;
              }
            }
          });
    }
  });

  for (int i = 0; i < kReorgCycles; ++i) {
    store->checkpoint();
  }
  stop.store(true, std::memory_order_relaxed);
  updater.join();
  scanner.join();

  CHECK_FALSE(corruption_found.load());

  for (int i = 0; i < kKeyCount; ++i) {
    const auto final_value = test_util::get_bytes_sync(store, "key" + std::to_string(i));
    REQUIRE(final_value.has_value());
    CHECK(final_value->size() == kStressValueBytes);
  }
}

// Regression test for base_boundary coverage across repeated checkpoint() cycles: each cycle
// advances base_boundary to cover everything durabilized so far, not just whatever the first
// forced checkpoint() established. Without this, a long-running insert-only process would have
// Scan's base-region coverage frozen forever at the first checkpoint, even as the live corpus
// kept growing past it.
TEST_CASE("VMemKV: checkpoint() extends base_boundary coverage across repeated cycles") {
  using TestStore = vmemkv::variants::VMemKV_Baseline;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), kStoreCapacityBytes);

  auto padded_key = [](int i) { return "key" + std::string(i < 10 ? "0" : "") + std::to_string(i); };
  const std::string value(kValue200Bytes, 'a');
  for (int i = 0; i < 10; ++i) {
    REQUIRE(store->insert(padded_key(i), value));
  }
  store->checkpoint();  // First checkpoint: creates the T2 checkpoint file.

  const uint64_t boundary_after_first = store->impl().t2().get_memory()->base_boundary;
  CHECK(boundary_after_first > 0);
  CHECK(store->impl().t2().get_memory()->base_mmap_scan != nullptr);
  CHECK(store->impl().t2().get_memory()->base_mmap_scan_seq != nullptr);
  CHECK(store->impl().t2().get_memory()->read_fd >= 0);

  // Insert more, then checkpoint again -- base_boundary advances further in place.
  for (int i = 10; i < 20; ++i) {
    REQUIRE(store->insert(padded_key(i), value));
  }
  store->checkpoint();

  const uint64_t boundary_after_second = store->impl().t2().get_memory()->base_boundary;
  CHECK(boundary_after_second > boundary_after_first);

  // Confirm the newly-promoted range is actually reachable and correct via the base_mmap_scan
  // path (Scan), not just the always-correct fallback (Get).
  std::map<std::string, std::string> seen;
  std::ignore =
      store->scan(padded_key(0), "key19~", [&](std::span<const std::byte> key, std::span<const std::byte> val) {
        seen.emplace(std::string(reinterpret_cast<const char *>(key.data()), key.size()),
                     std::string(reinterpret_cast<const char *>(val.data()), val.size()));
      });
  for (int i = 0; i < 20; ++i) {
    REQUIRE(seen.count(padded_key(i)) == 1);
    CHECK(seen.at(padded_key(i)) == value);
  }
}

// Stress/regression test: update()'s in-place path racing repeated checkpoint() cycles -- a
// narrower companion to the "concurrent update+scan survive repeated checkpoint (stress)" test
// above, isolating checkpoint_internal()'s append-quiescence/msync machinery from that test's
// added scan-path race.
//
// Deliberately bounded (kOpsPerCycle updates/scans per cycle, spawned and joined once per
// checkpoint() call) rather than free-spinning update/scan threads for the whole test duration:
// a handful of concurrent attempts per cycle, 60 times over, is enough to give any race a fair
// chance without needing sustained throughput. Free-spinning threads were tried first and run into a
// separate, already-fixed bug instead (get_impl()/scan_impl() exposing torn, mid-write T2
// records to the caller's callback -- see the "Torn-read fix" comments in get_impl()/scan_impl()
// in vmemkv_impl.hpp): at millions of scan calls, that unrelated race in the ordinary
// seqlock-protected read path reproduces on its own, with zero checkpoint()/reorganize() activity
// at all, and would make this test flaky for a reason that has nothing to do with what it's
// actually trying to catch. Run under ThreadSanitizer for direct
// race detection where available; also self-checks independent of TSan via the same
// uniform-byte-value technique as the "...repeated reorganize" test above.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE(
    "VMemKV: concurrent update+scan survive repeated cheap checkpoint() "
    "promotions (stress)") {
  using TestStore = vmemkv::variants::VMemKV_Baseline;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  constexpr int kKeyCount = 50;
  constexpr std::size_t kStressValueBytes = 200;
  constexpr int kCheckpointCycles = 60;
  constexpr int kOpsPerCycle = 50;

  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), kStoreCapacityBytes);

  for (int i = 0; i < kKeyCount; ++i) {
    REQUIRE(store->insert("key" + std::to_string(i), std::string(kStressValueBytes, 'a')));
  }
  store->checkpoint();  // Establish an initial base region for the cheap path to keep promoting.

  std::atomic<bool> corruption_found{false};

  for (int cycle = 0; cycle < kCheckpointCycles; ++cycle) {
    std::thread updater([&, cycle] {
      std::mt19937 rng(static_cast<unsigned>(cycle));
      std::uniform_int_distribution<int> key_dist(0, kKeyCount - 1);
      for (int i = 0; i < kOpsPerCycle; ++i) {
        const std::string key = "key" + std::to_string(key_dist(rng));
        const std::string value(kStressValueBytes, static_cast<char>('a' + (i % 26)));
        store->update(key, value);
      }
    });
    std::thread scanner([&] {
      for (int i = 0; i < kOpsPerCycle; ++i) {
        std::ignore =
            store->scan("key0", "key9", [&](std::span<const std::byte> /*key*/, std::span<const std::byte> value) {
              if (value.size() != kStressValueBytes) {
                corruption_found.store(true, std::memory_order_relaxed);
                return;
              }
              const auto expected = value[0];
              for (std::byte b : value) {
                if (b != expected) {
                  corruption_found.store(true, std::memory_order_relaxed);
                  break;
                }
              }
            });
      }
    });

    // A fresh key each cycle so there's always new, not-yet-durable data for the cheap path to
    // actually promote (checkpoint() is a no-op promotion if nothing changed since the last one).
    // Runs on the main thread concurrently with updater/scanner above, joined only after.
    REQUIRE(store->insert("cycle" + std::to_string(cycle), std::string(kStressValueBytes, 'c')));
    store->checkpoint();

    updater.join();
    scanner.join();
  }

  CHECK_FALSE(corruption_found.load());

  for (int i = 0; i < kKeyCount; ++i) {
    const auto final_value = test_util::get_bytes_sync(store, "key" + std::to_string(i));
    REQUIRE(final_value.has_value());
    CHECK(final_value->size() == kStressValueBytes);
  }
}

// Edge case: checkpoint() called on a store that has never had a single insert (append_size()==0,
// bytes_used()==0 at checkpoint time). T2FlatFile's constructor unconditionally establishes a
// real (non-null) base_mmap_scan and a real (>=0) read_fd regardless of bytes_used, so those
// mappings exist even for a store built this way -- the incremental-promotion mechanism above
// always has something to extend, never a null mapping to special-case.
TEST_CASE("VMemKV: checkpoint() on an empty store still establishes base_mmap_scan/read_fd") {
  using TestStore = vmemkv::variants::VMemKV_Baseline;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), kStoreCapacityBytes);

  store->checkpoint();  // No-op promotion: zero live entries, base_boundary stays at 0.
  CHECK(store->impl().t2().get_memory()->base_mmap_scan != nullptr);
  CHECK(store->impl().t2().get_memory()->base_mmap_scan_seq != nullptr);
  CHECK(store->impl().t2().get_memory()->read_fd >= 0);
  CHECK(store->impl().t2().get_memory()->base_boundary == 0);

  const std::string value(kValue200Bytes, 'a');
  for (int i = 0; i < 10; ++i) {
    REQUIRE(store->insert("key" + std::to_string(i), value));
  }
  store->checkpoint();  // Cheap path now: base_mmap_scan/read_fd already existed, just needs promoting.

  CHECK(store->impl().t2().get_memory()->base_boundary > 0);

  std::map<std::string, std::string> seen;
  std::ignore = store->scan("key0", "key9~", [&](std::span<const std::byte> key, std::span<const std::byte> val) {
    seen.emplace(std::string(reinterpret_cast<const char *>(key.data()), key.size()),
                 std::string(reinterpret_cast<const char *>(val.data()), val.size()));
  });
  for (int i = 0; i < 10; ++i) {
    REQUIRE(seen.count("key" + std::to_string(i)) == 1);
    CHECK(seen.at("key" + std::to_string(i)) == value);
  }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE_TEMPLATE("long keys sharing a 16-byte prefix: CRUD", Store, LONG_KEY_STORE_TYPES) {
  auto store = StoreFactory<Store>::make();

  const std::string prefix = "0123456789abcdef";
  const std::string key_one = prefix + "-alpha";
  const std::string key_two = prefix + "-bravo";
  const std::string key_three = prefix + "-charlie";

  CHECK(store->insert(key_one, 1));
  CHECK(store->insert(key_two, 2));
  CHECK(store->insert(key_three, 3));

  CHECK(test_util::get_sync(store, key_one) == 1U);
  CHECK(test_util::get_sync(store, key_two) == 2U);
  CHECK(test_util::get_sync(store, key_three) == 3U);

  CHECK(test_util::get_sync(store, prefix) == vmemkv::STORE_NOT_FOUND);

  CHECK_FALSE(store->insert(key_two, 99));
  CHECK(test_util::get_sync(store, key_two) == 2U);

  CHECK(store->update(key_two, 22));
  CHECK(test_util::get_sync(store, key_one) == 1U);
  CHECK(test_util::get_sync(store, key_two) == 22U);
  CHECK(test_util::get_sync(store, key_three) == 3U);

  CHECK(store->remove(key_one));
  CHECK(test_util::get_sync(store, key_one) == vmemkv::STORE_NOT_FOUND);
  CHECK(test_util::get_sync(store, key_two) == 22U);
  CHECK(test_util::get_sync(store, key_three) == 3U);

  CHECK(store->insert(key_one, 111));
  CHECK(test_util::get_sync(store, key_one) == 111U);
}

// Offset64 must disambiguate matching prefixes by hash(full_key).
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("Offset64 + hash-index disambiguates long keys sharing a prefix") {
  using OffsetAppendMapIndex = vmemkv::T1Index<vmemkv::Config<>>;
  // Heap-allocate: Hash index embeds a large bucket array unsuited to the stack.
  auto idx = std::make_unique<OffsetAppendMapIndex>();

  auto to_span = [](const std::string &key_string) {
    return std::span<const std::byte>(reinterpret_cast<const std::byte *>(key_string.data()), key_string.size());
  };

  const std::string prefix = "0123456789abcdef";
  const std::string key_one = prefix + "-alpha";
  const std::string key_two = prefix + "-bravo";

  CHECK(idx->put(to_span(key_one), 1) == OffsetAppendMapIndex::PutResult::Applied);
  CHECK(idx->put(to_span(key_two), 2) == OffsetAppendMapIndex::PutResult::Applied);
  CHECK(idx->get(to_span(key_one)) == 1U);
  CHECK(idx->get(to_span(key_two)) == 2U);
  CHECK(idx->put(to_span(key_one), 9) == OffsetAppendMapIndex::PutResult::Applied);
  CHECK(idx->get(to_span(key_one)) == 9U);

  idx->reorganize([](auto) {});
  CHECK(idx->get(to_span(key_one)) == 9U);
  CHECK(idx->get(to_span(key_two)) == 2U);
}

// Large byte values are only tested for stores backed by Tier 2.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE_TEMPLATE("large value (>= 64B): CRUD still works", Store, LARGE_VALUE_STORE_TYPES) {
  auto store = StoreFactory<Store>::make();

  const std::string v64(kValue64Bytes, 'a');
  const std::string v200(kValue200Bytes, 'b');

  CHECK(store->insert("key1", v64));
  auto got = test_util::get_bytes_sync(store, "key1");
  if (!got.has_value()) {
    FAIL("missing key1 after insert");
  }
  CHECK(as_string(*got) == v64);  // NOLINT(bugprone-unchecked-optional-access)

  CHECK_FALSE(store->insert("key1", v200));
  got = test_util::get_bytes_sync(store, "key1");
  if (!got.has_value()) {
    FAIL("missing key1 after duplicate insert");
  }
  CHECK(as_string(*got) == v64);  // NOLINT(bugprone-unchecked-optional-access)

  CHECK(store->update("key1", v200));
  got = test_util::get_bytes_sync(store, "key1");
  if (!got.has_value()) {
    FAIL("missing key1 after grow update");
  }
  CHECK(as_string(*got) == v200);  // NOLINT(bugprone-unchecked-optional-access)

  constexpr std::size_t kShrinkValueBytes = 16;
  const std::string v16(kShrinkValueBytes, 'c');

  CHECK(store->update("key1", v16));
  got = test_util::get_bytes_sync(store, "key1");
  if (!got.has_value()) {
    FAIL("missing key1 after shrink update");
  }
  CHECK(as_string(*got) == v16);  // NOLINT(bugprone-unchecked-optional-access)

  CHECK(store->insert("key2", v64));
  auto got_key2 = test_util::get_bytes_sync(store, "key2");
  if (!got_key2.has_value()) {
    FAIL("missing key2 after insert");
  }
  CHECK(as_string(*got_key2) == v64);  // NOLINT(bugprone-unchecked-optional-access)

  CHECK(store->remove("key2"));
  CHECK_FALSE(test_util::get_bytes_sync(store, "key2").has_value());
  auto got_key1 = test_util::get_bytes_sync(store, "key1");
  if (!got_key1.has_value()) {
    FAIL("missing key1 after key2 removal");
  }
  CHECK(as_string(*got_key1) == v16);  // NOLINT(bugprone-unchecked-optional-access)
}

TEST_CASE_TEMPLATE("large N: all keys retrievable", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  constexpr int key_count = 1000;
  for (int i = 0; i < key_count; ++i) {
    store->insert("k" + std::to_string(i), static_cast<uint64_t>(i));
  }
  for (int i = 0; i < key_count; ++i) {
    CHECK(test_util::get_sync(store, "k" + std::to_string(i)) == static_cast<uint64_t>(i));
  }
}

TEST_CASE_TEMPLATE("[mt] concurrent reads are consistent", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  constexpr int key_count = 500;
  constexpr int kReaderThreadCount = 4;
  for (int i = 0; i < key_count; ++i) {
    store->insert("k" + std::to_string(i), static_cast<uint64_t>(i));
  }

  std::atomic<bool> all_ok{true};
  std::vector<std::thread> threads;
  threads.reserve(kReaderThreadCount);
  for (int thread_index = 0; thread_index < kReaderThreadCount; ++thread_index) {
    threads.emplace_back([&] {
      for (int i = 0; i < key_count; ++i) {
        uint64_t value = test_util::get_sync(store, "k" + std::to_string(i));
        if (value != static_cast<uint64_t>(i)) {
          all_ok.store(false);
        }
      }
    });
  }
  for (auto &thread_handle : threads) {
    thread_handle.join();
  }
  CHECK(all_ok.load());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("VMemKV: checkpoint preserves correctness across garbage from update/remove") {
  auto store = StoreFactory<vmemkv::variants::VMemKV_Baseline>::make();

  std::string val1(kValue100Bytes, 'x');
  std::string val2(kValue200Bytes, 'y');

  CHECK(store->insert("k1", val1));
  CHECK(store->insert("k2", val1));

  uint64_t bytes_used_before = store->t2().bytes_used();

  // Grow-update leaves the old value as garbage; remove leaves k1's record as garbage too.
  CHECK(store->update("k2", val2));
  uint64_t bytes_used_after_update = store->t2().bytes_used();
  CHECK(bytes_used_after_update > bytes_used_before);

  CHECK(store->remove("k1"));

  store->checkpoint();

  CHECK_FALSE(test_util::get_bytes_sync(store, "k1").has_value());
  auto got = test_util::get_bytes_sync(store, "k2");
  if (!got.has_value()) {
    FAIL("missing k2 after reorganize");
  }
  CHECK(as_string(*got) == val2);  // NOLINT(bugprone-unchecked-optional-access)
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE_TEMPLATE("scan with integral keys verifies lexicographical ordering", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  // Keys would sort wrong as strings (1 < 10 < 100 < 2); verifies numeric order instead.
  store->insert(kScanHundred, kScanHundred);
  store->insert(1U, 1U);
  store->insert(kScanTen, kScanTen);
  store->insert(2U, 2U);

  // reorganize() moves append_region to sorted_region, required for ordered scan results.
  store->reorganize();

  std::vector<uint64_t> keys;
  const size_t scan_count =
      store->scan(kScanStart,
                  kScanUpperBound,
                  [&](std::span<const std::byte> key_bytes, [[maybe_unused]] std::span<const std::byte> value) {
                    // Decode big-endian key bytes back to uint32_t.
                    uint64_t key = 0;
                    for (size_t i = 0; i < 4; ++i) {
                      key = (key << kBitsPerByte) | static_cast<uint8_t>(key_bytes[i]);
                    }
                    keys.push_back(key);
                  });
  REQUIRE(scan_count == 4U);

  REQUIRE(keys.size() == 4U);
  CHECK(keys[0] == 1U);
  CHECK(keys[1] == 2U);
  CHECK(keys[2] == kScanTen);
  CHECK(keys[3] == kScanHundred);
}

// Regression test: scan_impl()'s inline-value fast path used to recover a key's original length
// from T1's 16-byte zero-padded prefix by trimming trailing zero bytes -- ambiguous whenever the
// key's own last byte is 0x00 (indistinguishable from padding), which silently truncated the key
// handed to scan()'s callback. A big-endian-encoded integer key that's a multiple of 256 hits this
// directly (e.g. 256 encodes as {0x00,0x00,0x01,0x00}). Fixed by excluding such keys from
// inlining (try_make_inline_payload()) so they take the normal T2-record path instead, where the
// full key is stored verbatim. This test only makes sense for a store with UseT1InlineValue on
// (Var2_Inline) -- the bug is specific to that fast path.
TEST_CASE("Value Inlining: scan() returns the untruncated key for a key ending in a zero byte") {
  auto store = StoreFactory<vmemkv::variants::VMemKV_Var2_Inline>::make();
  store->insert(256U, 256U);  // big-endian encode(256) = {0x00,0x00,0x01,0x00} -- ends in 0x00.
  store->reorganize();

  std::vector<std::byte> observed_key;
  const size_t scan_count =
      store->scan(0U, 1000U, [&](std::span<const std::byte> key_bytes, std::span<const std::byte> /*value*/) {
        observed_key.assign(key_bytes.begin(), key_bytes.end());
      });
  REQUIRE(scan_count == 1U);
  REQUIRE(observed_key.size() == 4U);
  CHECK(static_cast<uint8_t>(observed_key[0]) == 0x00U);
  CHECK(static_cast<uint8_t>(observed_key[1]) == 0x00U);
  CHECK(static_cast<uint8_t>(observed_key[2]) == 0x01U);
  CHECK(static_cast<uint8_t>(observed_key[3]) == 0x00U);

  // get() must still find it via the exact key the caller already knows (unaffected by this fix
  // either way, since it never needed to recover the key from T1's prefix).
  const auto got = test_util::get_bytes_sync(store, 256U);
  REQUIRE(got.has_value());
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("Value Inlining: verify that short/8B-aligned values bypass T2 write paths") {
  const std::string path = reserve_temp_path().string();
  constexpr uint64_t kInlineStoreCapacityBytes = 8ULL * 1024ULL * 1024ULL;
  constexpr std::byte kShortFillByte{0xAB};
  constexpr std::byte kLongFillByte{0xCD};
  constexpr uint64_t kOddInlineValue = 0x003456789ABCDEF1ULL;
  constexpr uint64_t kEvenInlineValue = 0x123456789ABCDEF0ULL;

  SUBCASE("T1InlineValue behavior (1-8 bytes)") {
    using InlineStore = vmemkv::variants::VMemKV_Var2_Inline;
    auto store = std::make_unique<InlineStore>(path, kInlineStoreCapacityBytes);

    uint64_t initial_bytes = store->t2().bytes_used();
    CHECK(initial_bytes == 0);

    // 1-7 bytes: should inline (no T2 usage).
    std::vector<std::byte> val_short(kShortInlineValueBytes, kShortFillByte);
    store->insert("key1", val_short);

    CHECK(store->t2().bytes_used() == 0);

    auto res = test_util::get_bytes_sync(store, "key1");
    if (!res.has_value()) {
      FAIL("missing key1 inline payload");
    }
    const auto &res_value = *res;  // NOLINT(bugprone-unchecked-optional-access)
    CHECK(res_value.size() == kShortInlineValueBytes);
    CHECK(res_value[0] == kShortFillByte);

    // 8-byte odd integer: should inline.
    uint64_t val_odd = kOddInlineValue;
    std::vector<std::byte> val_odd_bytes(kInlineValueBytes);
    std::memcpy(val_odd_bytes.data(), &val_odd, kInlineValueBytes);

    store->insert("key_odd", val_odd_bytes);
    CHECK(store->t2().bytes_used() == 0);

    auto res_odd = test_util::get_bytes_sync(store, "key_odd");
    if (!res_odd.has_value()) {
      FAIL("missing key_odd inline payload");
    }
    const auto &res_odd_value = *res_odd;  // NOLINT(bugprone-unchecked-optional-access)
    REQUIRE(res_odd_value.size() == kInlineValueBytes);
    uint64_t read_odd = 0;
    std::memcpy(&read_odd, res_odd_value.data(), kInlineValueBytes);
    CHECK(read_odd == val_odd);

    // 8-byte even integer: should also inline.
    uint64_t val_even = kEvenInlineValue;
    std::vector<std::byte> val_even_bytes(kInlineValueBytes);
    std::memcpy(val_even_bytes.data(), &val_even, kInlineValueBytes);

    store->insert("key_even", val_even_bytes);
    CHECK(store->t2().bytes_used() == 0);

    auto res_even = test_util::get_bytes_sync(store, "key_even");
    if (!res_even.has_value()) {
      FAIL("missing key_even inline payload");
    }
    const auto &res_even_value = *res_even;  // NOLINT(bugprone-unchecked-optional-access)
    REQUIRE(res_even_value.size() == kInlineValueBytes);
    uint64_t read_even = 0;
    std::memcpy(&read_even, res_even_value.data(), kInlineValueBytes);
    CHECK(read_even == val_even);

    // 9+ bytes: should bypass inlining and go to T2.
    std::vector<std::byte> val_long(kLongValueBytes, kLongFillByte);
    store->insert("key2", val_long);

    CHECK(store->t2().bytes_used() > 0);

    std::filesystem::remove(path);
    vmemkv::remove_wal_segments(vmemkv::derive_wal_path(path));
  }
}

// Regression tests for the formerly-open "torn read via user callback" bug:
// get_impl()/scan_impl() used to invoke the caller-supplied callback directly from inside
// read_t2_record_seqlock()'s copy_func, handing it a std::span into T2Memory::base -- live,
// concurrently update_value_at()-writable memory -- *before* the seqlock's version re-check had
// run. The seqlock did correctly detect the race and retry afterward, but could not retract the
// fact that the callback had already observed torn (part-old, part-new) bytes on the first,
// discarded attempt. Fixed by having copy_func only copy into an owned buffer and invoking the
// callback after read_t2_record_seqlock() returns (see get_impl()/scan_impl()'s own comments).
//
// Deliberately no reorganize()/checkpoint() anywhere in either test below: unlike
// the writer-stop-barrier/residual-window tests elsewhere in this file, this race needed nothing but
// plain concurrent update()+scan() (or update()+get()) on a single key -- proving the fix holds
// even in that minimal case is the point.
TEST_CASE("VMemKV: scan callback never observes a torn read across a concurrent update (regression)") {
  using TestStore = vmemkv::variants::VMemKV_Baseline;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  constexpr std::size_t kValueBytes = 200;  // Non-inline; same-size updates stay in the in-place path.
  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), kStoreCapacityBytes);

  REQUIRE(store->insert("hot", std::string(kValueBytes, 'a')));

  std::atomic<bool> stop{false};
  std::atomic<bool> torn_read_found{false};

  std::thread updater([&] {
    uint64_t i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      std::string next_value(kValueBytes, static_cast<char>('a' + (i++ % 26)));
      store->update("hot", next_value);
    }
  });

  std::thread scanner([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      std::ignore =
          store->scan("hot", "hot~", [&](std::span<const std::byte> /*key*/, std::span<const std::byte> value) {
            if (value.empty()) {
              return;
            }
            const auto expected = value[0];
            for (std::byte b : value) {
              if (b != expected) {
                torn_read_found.store(true, std::memory_order_relaxed);
                return;
              }
            }
          });
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true, std::memory_order_relaxed);
  updater.join();
  scanner.join();

  CHECK_FALSE(torn_read_found.load());
}

// Same bug, same fix, but via get() instead of scan() -- get_impl()'s copy_func had the identical
// callback-inside-the-seqlock-window shape as scan_impl()'s.
TEST_CASE("VMemKV: get callback never observes a torn read across a concurrent update (regression)") {
  using TestStore = vmemkv::variants::VMemKV_Baseline;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  constexpr std::size_t kValueBytes = 200;
  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), kStoreCapacityBytes);

  REQUIRE(store->insert("hot", std::string(kValueBytes, 'a')));

  std::atomic<bool> stop{false};
  std::atomic<bool> torn_read_found{false};

  std::thread updater([&] {
    uint64_t i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      std::string next_value(kValueBytes, static_cast<char>('a' + (i++ % 26)));
      store->update("hot", next_value);
    }
  });

  std::thread getter([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      std::ignore = store->get("hot", [&](std::span<const std::byte> value) {
        if (value.empty()) {
          return;
        }
        const auto expected = value[0];
        for (std::byte b : value) {
          if (b != expected) {
            torn_read_found.store(true, std::memory_order_relaxed);
            return;
          }
        }
      });
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true, std::memory_order_relaxed);
  updater.join();
  getter.join();

  CHECK_FALSE(torn_read_found.load());
}

// Free-spinning variant of the same regression, with no reorganize()/checkpoint() at all: prior
// to the fix above, this exact shape (see the "...repeated cheap checkpoint() promotions
// (stress)" test's own comment elsewhere in this file) had to be deliberately avoided because it
// reliably tripped this bug on its own. Now that the underlying bug is fixed, confirm the
// simplest possible free-spinning form is safe too.
TEST_CASE(
    "VMemKV: free-spinning update+scan survive with no reorganize/checkpoint "
    "at all (regression)") {
  using TestStore = vmemkv::variants::VMemKV_Baseline;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  constexpr std::size_t kValueBytes = 200;
  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), kStoreCapacityBytes);

  REQUIRE(store->insert("hot", std::string(kValueBytes, 'a')));

  std::atomic<bool> stop{false};
  std::atomic<bool> torn_read_found{false};

  std::thread updater([&] {
    uint64_t i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      std::string next_value(kValueBytes, static_cast<char>('a' + (i++ % 26)));
      store->update("hot", next_value);
    }
  });

  std::thread scanner([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      std::ignore =
          store->scan("hot", "hot~", [&](std::span<const std::byte> /*key*/, std::span<const std::byte> value) {
            if (value.empty()) {
              return;
            }
            const auto expected = value[0];
            for (std::byte b : value) {
              if (b != expected) {
                torn_read_found.store(true, std::memory_order_relaxed);
                return;
              }
            }
          });
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true, std::memory_order_relaxed);
  updater.join();
  scanner.join();

  CHECK_FALSE(torn_read_found.load());
}

namespace {

// Shared by the two writer-stop-barrier regression tests below: builds a "straggler" T2 record
// through an already-acquired write handle and publishes it into T1, exactly as
// write_entry_lockfree() would for a real append.
template <typename ImplT, typename StorePtr>
void publish_straggler_entry(StorePtr &store,
                             const vmemkv::T2FlatFile::T2MemoryHandle &mem,
                             const std::string &straggler_value) {
  kvs_detail::with_key_serialized(std::string("straggler"), [&](std::span<const std::byte> key_bytes) {
    kvs_detail::with_val_serialized(straggler_value, [&](std::span<const std::byte> val_bytes) {
      const uint64_t offset = vmemkv::T2FlatFile::append_default(mem, key_bytes, val_bytes);
      uint64_t aligned_len = vmemkv::align_up(sizeof(ValueRecordHeader) + key_bytes.size() + val_bytes.size());
      uint64_t block_count = aligned_len / ImplT::kBlockAlignment;
      uint64_t encoded_payload = offset | (block_count << ImplT::kSizeEmbeddingShift);
      REQUIRE(store->impl().t1().put(vmemkv::prepare_t1_key(key_bytes), vmemkv::T1Value{encoded_payload, false, 0}));
    });
  });
}

// Shared trailing check: the straggler entry published above must be immediately readable, with
// no second checkpoint/reorganize cycle needed.
template <typename StorePtr>
void verify_straggler_readback(StorePtr &store, const std::string &straggler_value) {
  const auto straggler_readback = test_util::get_bytes_sync(store, "straggler");
  REQUIRE(straggler_readback.has_value());
  CHECK(straggler_readback->size() == straggler_value.size());
}

}  // namespace

// Regression test for checkpoint_internal()'s "residual window" race: a writer must never be
// able to land a fresh T2 append past the frontier a concurrently-running checkpoint is about to
// capture as `target`/`base_boundary`.
//
// Closed in t2_flat_file.hpp by pairing acquire_write_handle() (write_entry_lockfree()'s only way
// to get a T2 write handle, held across both the T2 append and the T1 publish attempt) with
// stop_writers_and_wait() (called here right before the target is captured): it marks writes
// stopped, so acquire_write_handle() stops handing out new handles, then blocks until every
// writer that already held a handle -- i.e. started before the flag went up -- has released it,
// which only happens after that writer's T1 publish attempt has returned.
//
// This test proves exactly that handshake, deterministically, using a real writer thread (not a
// synchronous hook simulating the race, which would deadlock: by the time pre_finish_hook fires,
// writer_stop_ is already true, and nothing but this same call's own later resume_writers() would
// ever clear it -- a hook-spawned writer would spin forever waiting for a flag its own spawning
// call is blocking on). Instead, pre_stop_hook fires *before* the stop flag goes up, spawning a
// thread that registers a real T2MemoryHandle and pauses briefly before publishing -- exercising
// the "writer already in flight when the stop starts" case. stop_writers_and_wait() must block
// until this thread finishes, and this *same* cycle's captured target must then already cover its
// entry -- reading it back must work immediately, no second cycle needed.
TEST_CASE(
    "VMemKV: checkpoint's writer-stop barrier waits for an in-flight writer instead of "
    "capturing a frontier underneath it (regression)") {
  using TestStore = vmemkv::variants::VMemKVStore;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), kStoreCapacityBytes);

  // 200B: forces a real T2 record even under T1InlineValue (an inline payload never touches T2,
  // so wouldn't reach the write-handle registration this test targets).
  const std::string baseline_value(200, 'v');
  REQUIRE(store->insert("baseline", baseline_value));

  const std::string straggler_value(200, 's');
  std::thread straggler_writer;

  // pre_stop_hook fires strictly before the stop flag goes up, so the spawned thread's
  // acquire_write_handle() call is guaranteed to register with the reference tracker before
  // stop_writers_and_wait() ever scans it.
  using ImplT = std::decay_t<decltype(store->impl())>;
  store->impl().reorganize_internal(ImplT::ReorgMode::Checkpoint,
                                    /*pre_stop_hook=*/
                                    [&] {
                                      straggler_writer = std::thread([&] {
                                        vmemkv::T2FlatFile::T2MemoryHandle mem =
                                            store->impl().t2().acquire_write_handle();
                                        // Simulates being "mid-write": long enough that, absent the stop-and-wait, the
                                        // checkpoint would very likely have already captured its target by the time
                                        // this thread publishes.
                                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                                        publish_straggler_entry<ImplT>(store, mem, straggler_value);
                                        // mem released here -- only now can stop_writers_and_wait() (blocked on this
                                        // exact handle since before this thread even started sleeping) proceed.
                                      });
                                    });
  straggler_writer.join();

  // This same checkpoint cycle's captured target must already cover "straggler" -- reading it
  // back must work immediately, no second cycle needed.
  verify_straggler_readback(store, straggler_value);
}

// Regression test for the formerly-open "residual window" race described in
// checkpoint_internal()'s comment: T2FlatFile::acquire_write_handle() used to check
// writer_stop_ *before* registering with the reference tracker, which
// ThreadReferenceTracker::wait_until_retired()'s single index-ordered scan (it never re-examines
// a slot once past it) could race -- a writer whose check saw the flag false, but who hadn't
// registered yet, could still be missed by an already-in-progress scan, so the stop-and-wait
// could complete and the writer would go on to append past the frontier this same cycle just
// captured. Fixed by registering first and only then checking the flag, retrying if it turns out
// to already be true.
//
// Reproduces the adversarial timing deterministically via acquire_write_handle()'s hook seam
// (fires once, right after registering and before the writer_stop_ check): the writer thread
// registers, signals pre_stop_hook that it has done so, then sleeps -- guaranteeing
// stop_writers_and_wait() (called on the main thread right after pre_stop_hook returns) sets
// writer_stop_=true and starts scanning while this thread is still paused, already registered but
// not yet having checked the flag. Under the fix, the writer's own check must then see
// writer_stop_==true and back out/retry rather than proceed with a handle that might land past
// the frontier this same cycle already captured -- exactly the guarantee the old check-then-register
// order could not make.
TEST_CASE(
    "VMemKV: acquire_write_handle()'s register-then-check-retry survives a writer racing the "
    "writer-stop scan (regression)") {
  using TestStore = vmemkv::variants::VMemKVStore;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), kStoreCapacityBytes);

  const std::string baseline_value(200, 'v');
  REQUIRE(store->insert("baseline", baseline_value));

  const std::string straggler_value(200, 's');
  std::thread straggler_writer;
  std::atomic<bool> writer_registered{false};

  using ImplT = std::decay_t<decltype(store->impl())>;
  store->impl().reorganize_internal(ImplT::ReorgMode::Checkpoint,
                                    /*pre_stop_hook=*/
                                    [&] {
                                      straggler_writer = std::thread([&] {
                                        // Only the *first* acquire_write_handle() attempt needs to pause here -- that's
                                        // the one stop_writers_and_wait() (below) is guaranteed to observe as "already
                                        // registered, haven't checked the flag yet" (the exact window this test
                                        // targets). A production caller's retries carry no such delay
                                        // (NoOpAcquireWriteHandleHook is instant), so they re-check writer_stop_ within
                                        // nanoseconds of it clearing; re-pausing on every retry here would be purely a
                                        // test-harness artifact -- and a bad one: it can stretch the *net* time this
                                        // slot spends showing a stale value far past what
                                        // ThreadReferenceTracker::wait_until_retired()'s SpinBackoff (yields, then 1ms
                                        // polls) is tuned to catch quickly, since each retry reopens only a
                                        // sub-microsecond release window against a 30ms-wide observation stride.
                                        bool first_attempt = true;
                                        vmemkv::T2FlatFile::T2MemoryHandle mem =
                                            store->impl().t2().acquire_write_handle([&] {
                                              writer_registered.store(true, std::memory_order_release);
                                              if (first_attempt) {
                                                first_attempt = false;
                                                // Long enough that stop_writers_and_wait() below is guaranteed to have
                                                // set writer_stop_=true and started its scan (finding this thread's
                                                // slot registered, hence blocking on it) before this thread wakes up
                                                // and checks the flag itself.
                                                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                                              }
                                            });
                                        publish_straggler_entry<ImplT>(store, mem, straggler_value);
                                      });
                                      // Don't let pre_stop_hook return until the writer has registered -- otherwise
                                      // stop_writers_and_wait() might start scanning before the writer's slot is set at
                                      // all, which wouldn't exercise the "scan already saw/blocked on a registered
                                      // slot, writer only then wakes up and self-checks" path this test targets.
                                      while (!writer_registered.load(std::memory_order_acquire)) {
                                        std::this_thread::yield();
                                      }
                                    });
  straggler_writer.join();

  verify_straggler_readback(store, straggler_value);
}

// Regression test: checkpoint_internal() publishes T1 via a single, I/O-free call at the very
// end (see checkpoint_internal()'s own comment). This test injects a fault at the last possible
// moment before T1 could ever be touched (NoOpPreFinishHook's seam) and confirms the store is
// still fully functional afterward -- no hang, correct data, and a subsequent checkpoint()
// succeeds normally.
TEST_CASE("VMemKV: exception before T1 publish during a T2 rebuild leaves the store fully usable") {
  using TestStore = vmemkv::variants::VMemKVStore;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), kStoreCapacityBytes);

  const std::string value(200, 'v');
  REQUIRE(store->insert("key", value));

  struct InjectedFault : std::runtime_error {
    InjectedFault() : std::runtime_error("injected fault before T1 publish") {}
  };

  bool threw = false;
  try {
    store->impl().reorganize_internal(std::decay_t<decltype(store->impl())>::ReorgMode::Checkpoint,
                                      /*pre_stop_hook=*/vmemkv::NoOpPreStopHook{},
                                      /*pre_finish_hook=*/[] { throw InjectedFault{}; });
  } catch (const InjectedFault &) {
    threw = true;  // Mirrors reorg_worker_loop()'s catch (...) {} -- swallow and move on.
  }
  REQUIRE(threw);

  // No hang, no rollback needed: T1 was never touched, so the store is immediately usable.
  const auto readback = test_util::get_bytes_sync(store, "key");
  REQUIRE(readback.has_value());
  CHECK(std::string(reinterpret_cast<const char *>(readback->data()), readback->size()) == value);

  REQUIRE(store->update("key", std::string(200, 'w')));

  // A subsequent, unfaulted checkpoint cycle must still succeed normally.
  store->checkpoint();
  const auto final_readback = test_util::get_bytes_sync(store, "key");
  REQUIRE(final_readback.has_value());
  CHECK(final_readback->size() == 200);
}
