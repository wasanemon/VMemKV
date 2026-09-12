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
static_assert(vmemkv::KVStore<vmemkv::VMemKVStore>);
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKV_RocksDB>);
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKV_RocksDBBlobDB>);
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKV_LMDB>);

namespace test_util {
template <typename StorePtr, typename Key>
auto get_u64_sync(const StorePtr &store, const Key &key) -> uint64_t {
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

template <typename Store>
struct StoreFactory;

// Helper to reserve temp path for T2 files
static auto reserve_temp_path() -> std::filesystem::path {
  return vmemkv_test::reserve_unique_temp_path("vmemkv_kv", /*also_remove_wal_sibling=*/true);
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

  static auto make() -> std::unique_ptr<vmemkv::StoreAdapter<Impl>, VMemKVDeleter<Impl>> {
    std::filesystem::path path = reserve_temp_path();
    if constexpr (std::is_same_v<Impl, ::RocksDBStore> || std::is_same_v<Impl, ::RocksDBBlobDBStore> ||
                  std::is_same_v<Impl, ::LMDBStore> || std::is_same_v<Impl, ::LeanStoreStore>) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      auto *store = new vmemkv::StoreAdapter<Impl>(path.string());
      return std::unique_ptr<vmemkv::StoreAdapter<Impl>, VMemKVDeleter<Impl>>(store, VMemKVDeleter<Impl>{path});
    } else {
      auto *store = new vmemkv::StoreAdapter<Impl>(path, kCapacityBytes);
      return std::unique_ptr<vmemkv::StoreAdapter<Impl>, VMemKVDeleter<Impl>>(store, VMemKVDeleter<Impl>{path});
    }
  }
};

// ─── Store type lists ───────────────────────────────────────────────────────

// VMemKV 自体のバリエーション（Baseline, Cumulative Steps, Ablations, Inlining, Read policy）
#define VMemKVStores                                                                                                 \
  vmemkv::variants::VMemKV_Var0_Baseline, vmemkv::variants::VMemKV_Var1_Bloom, vmemkv::variants::VMemKV_Var2_Inline, \
      vmemkv::variants::VMemKV_Var3_ReadRandom, vmemkv::variants::VMemKV_Var4_ReadSeq

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

#ifdef ENABLE_LEANSTORE
#define LeanStoreRivalStores , vmemkv::variants::VMemKV_LeanStore
#else
#define LeanStoreRivalStores
#endif

#define RivalStores RocksDBRivalStores LMDBRivalStores LeanStoreRivalStores

#define STORE_TYPES VMemKVStores RivalStores

struct FrequentCheckpointConfig : vmemkv::Config<> {
  // Partial override style: inherit all defaults and only tune checkpoint aggressiveness.
  static constexpr size_t WalMaxBytesSinceCheckpoint = 64ULL << 10;  // 64 KiB.
};

static_assert(FrequentCheckpointConfig::T1AppendCapacityEntries ==
              (size_t{1} << FrequentCheckpointConfig::T1AppendCapacityLog2));

using VMemKV_FrequentCheckpoint = vmemkv::StoreAdapter<vmemkv::VMemKVImpl<FrequentCheckpointConfig>>;

template <typename StoreHandle>
static void insert_sequential_u64_values(StoreHandle &store, int key_count) {
  for (int i = 0; i < key_count; ++i) {
    CHECK(store->insert("k" + std::to_string(i), static_cast<uint64_t>(i)));
  }
}

template <typename StoreHandle>
static void check_sequential_u64_values(StoreHandle &store, int key_count) {
  for (int i = 0; i < key_count; ++i) {
    CHECK(test_util::get_u64_sync(store, "k" + std::to_string(i)) == static_cast<uint64_t>(i));
  }
}

template <typename StoreHandle>
static void check_hot_key_upgrade_is_visible(StoreHandle &store) {
  CHECK(store->insert("hot", std::string("a")));
  const std::string large_value(kValue64Bytes, 'x');
  CHECK(store->update("hot", large_value));
  const auto got = vmemkv_test::get_optional_bytes(store, "hot");
  REQUIRE(got.has_value());
  if (!got.has_value()) {
    return;
  }
  CHECK(vmemkv_test::span_to_string(vmemkv_test::as_span(got.value())) == large_value);
}

// ─── Test cases (one per scenario) ───────────────────────────────────────────

TEST_CASE_TEMPLATE("get on empty store returns STORE_NOT_FOUND", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  CHECK(test_util::get_u64_sync(store, "x") == vmemkv::STORE_NOT_FOUND);
}

TEST_CASE_TEMPLATE("insert and get", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  CHECK(store->insert("a", 10));
  CHECK(test_util::get_u64_sync(store, "a") == 10U);
}

TEST_CASE("checkpoint churn during writes is transparent") {
  auto store = StoreFactory<VMemKV_FrequentCheckpoint>::make();

  constexpr int key_count = 25000;
  insert_sequential_u64_values(store, key_count);

  // The append-then-inline-to-non-inline update path should stay transparent even when
  // checkpoints trigger frequently in between.
  check_hot_key_upgrade_is_visible(store);

  check_sequential_u64_values(store, key_count);
}

TEST_CASE("partial config inheritance keeps required append-capacity fields") {
  auto store = StoreFactory<VMemKV_FrequentCheckpoint>::make();
  CHECK(FrequentCheckpointConfig::T1AppendCapacityEntries == vmemkv::Config<>::T1AppendCapacityEntries);
  CHECK(store->insert("partial_cfg", 1));
  CHECK(test_util::get_u64_sync(store, "partial_cfg") == 1U);
}

TEST_CASE_TEMPLATE("insert duplicate returns false, value unchanged", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  CHECK(store->insert("a", 10));
  CHECK_FALSE(store->insert("a", 99));
  CHECK(test_util::get_u64_sync(store, "a") == 10U);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE_TEMPLATE("integral keys use the templated convenience API", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  constexpr int key = 42;

  CHECK(store->insert(key, 10));
  CHECK(test_util::get_u64_sync(store, key) == 10U);
  CHECK(store->update(key, 11));
  CHECK(test_util::get_u64_sync(store, key) == 11U);
  CHECK(store->remove(key));
  CHECK(test_util::get_u64_sync(store, key) == vmemkv::STORE_NOT_FOUND);
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

  CHECK(test_util::get_u64_sync(store, key_one) == kSmallValue);
  CHECK(test_util::get_u64_sync(store, key_two) == kLargeValue);

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

// A value that's bit-for-bit identical to T1's STORE_NOT_FOUND sentinel (~0ULL) is fully storable
// and retrievable like any other: VMemKVImpl::try_make_inline_payload() declines to inline this
// exact value, routing it through the ordinary T2-record path instead, whose payload is an
// offset, never the raw value bytes.
TEST_CASE_TEMPLATE("insert accepts a value equal to STORE_NOT_FOUND and it round-trips", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  CHECK(store->insert("a", vmemkv::STORE_NOT_FOUND));
  bool found = store->get("a", [](std::span<const std::byte> /*val*/) {});
  CHECK(found);
  CHECK(test_util::get_u64_sync(store, "a") == vmemkv::STORE_NOT_FOUND);
}

TEST_CASE_TEMPLATE("update existing key", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  store->insert("a", 1);
  CHECK(store->update("a", 2));
  CHECK(test_util::get_u64_sync(store, "a") == 2U);
}

TEST_CASE_TEMPLATE("update missing key returns false", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  CHECK_FALSE(store->update("z", 1));
}

// Same as the insert case above, for update().
TEST_CASE_TEMPLATE("update accepts a value equal to STORE_NOT_FOUND and it round-trips", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  store->insert("a", 1);
  store->update("a", vmemkv::STORE_NOT_FOUND);
  CHECK(test_util::get_u64_sync(store, "a") == vmemkv::STORE_NOT_FOUND);
}

// Per-key get() exactness after distinct same-size updates plus interleaved deletes:
// every surviving key must read back exactly its latest value (not a stale version or a
// length-corrupted payload), deleted keys must read absent. Same-size values keep this
// valid for LeanStore (no size-changing update path there); no key is reinserted.
TEST_CASE_TEMPLATE("get returns latest value per key after update and delete churn", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  constexpr size_t kKeys = 300;
  for (size_t i = 0; i < kKeys; ++i) {
    store->insert("k" + std::to_string(i), i);
  }
  for (size_t i = 0; i < kKeys; ++i) {
    CHECK(store->update("k" + std::to_string(i), 100000 + i));
  }
  for (size_t i = 0; i < kKeys; i += 3) {
    CHECK(store->remove("k" + std::to_string(i)));
  }
  for (size_t i = 0; i < kKeys; ++i) {
    if (i % 3 == 0) {
      CHECK(test_util::get_u64_sync(store, "k" + std::to_string(i)) == vmemkv::STORE_NOT_FOUND);
    } else {
      CHECK(test_util::get_u64_sync(store, "k" + std::to_string(i)) == 100000 + i);
    }
  }
  CHECK(test_util::get_u64_sync(store, "k_missing") == vmemkv::STORE_NOT_FOUND);
}
// LeanStore-only: clone-from-master construction round-trips data, isolates mutations to
// the clone (the master stays intact for later clones), and a second clone of the same
// master still recovers. Exercises ensure_master_built()/clone_from()/open_recover(),
// including the registry prune across sequential instances.
TEST_CASE("LeanStore clone from master round-trips and isolates") {
  using Store = vmemkv::StoreAdapter<::LeanStoreStore>;
  const std::string master = reserve_temp_path().string() + "_master";
  auto make_key = [](std::size_t i) { return vmemkv_test::padded_key(i, 5); };
  auto make_value = [](std::size_t i) {
    std::string v = "v" + std::to_string(i);
    v.append(24 - v.size(), '.');
    return v;
  };
  constexpr std::size_t kKeys = 200;
  const auto check_all = [&](Store *store, const std::string &phase) {
    for (size_t i = 0; i < kKeys; ++i) {
      const auto got = vmemkv_test::get_optional_bytes(store, make_key(i));
      if (!got.has_value() || vmemkv_test::span_to_string(vmemkv_test::as_span(*got)) != make_value(i)) {
        MESSAGE("mismatch phase=" << phase << " i=" << i << " got="
                                  << (got.has_value() ? vmemkv_test::span_to_string(vmemkv_test::as_span(*got))
                                                      : "<absent>"));
      }
      REQUIRE(got.has_value());
      CHECK(vmemkv_test::span_to_string(vmemkv_test::as_span(*got)) == make_value(i));
    }
  };
  {
    // Same data through the plain insert path (no master/clone/recover involved).
    auto plain = StoreFactory<Store>::make();
    for (size_t i = 0; i < kKeys; ++i) {
      plain->insert(make_key(i), make_value(i));
    }
    check_all(plain.get(), "plain");
  }
  {
    Store clone(typename ::LeanStoreStore::CloneFromMasterTag{}, master, kKeys, make_key, make_value);
    check_all(&clone, "clone1");
    for (size_t i = 0; i < kKeys; i += 2) {
      CHECK(clone.remove(make_key(i)));
    }
  }
  {
    Store clone2(typename ::LeanStoreStore::CloneFromMasterTag{}, master, kKeys, make_key, make_value);
    check_all(&clone2, "clone2");
  }
  std::error_code ignored;
  std::filesystem::remove(master, ignored);
  std::filesystem::remove(master + ".json", ignored);
  std::filesystem::remove(master + "_clone.leanstore", ignored);
  std::filesystem::remove(master + "_clone.leanstore.json", ignored);
}

// StoreAdapter::bulk_load() calls impl_.bulk_load_impl() directly, bypassing insert()/update()'s
// own path -- exercises that a bulk-loaded entry whose 8-byte value equals STORE_NOT_FOUND still
// round-trips via get()/scan() instead of being silently inlined and becoming indistinguishable
// from a tombstone, on the one variant where inlining is actually possible.
TEST_CASE("bulk_load: an entry whose value equals STORE_NOT_FOUND round-trips via get() and scan()") {
  auto store = StoreFactory<vmemkv::variants::VMemKV_Var2_Inline>::make();
  const std::string all_ff_value(8, '\xFF');
  store->bulk_load(
      1, [](std::size_t /*index*/) { return std::string("a"); }, [&](std::size_t /*index*/) { return all_ff_value; });

  bool found = store->get("a", [](std::span<const std::byte> /*val*/) {});
  CHECK(found);
  CHECK(test_util::get_u64_sync(store, "a") == vmemkv::STORE_NOT_FOUND);

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
  CHECK(test_util::get_u64_sync(store, "a") == vmemkv::STORE_NOT_FOUND);
}

TEST_CASE_TEMPLATE("remove missing key returns false", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  CHECK_FALSE(store->remove("z"));
}

TEST_CASE_TEMPLATE("re-insert after remove", Store, STORE_TYPES) {
  // LeanStore's BTreeVI has no insert-after-remove path (upstream TODO, hits ensure(false)),
  // and no benchmark flow reinserts a removed key.
  if constexpr (std::is_same_v<Store, vmemkv::variants::VMemKV_LeanStore>) {
    MESSAGE("skipped for LeanStore: engine cannot reinsert removed keys");
    return;
  }
  auto store = StoreFactory<Store>::make();
  store->insert("a", 1);
  store->remove("a");
  CHECK(store->insert("a", 2));
  CHECK(test_util::get_u64_sync(store, "a") == 2U);
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

TEST_CASE_TEMPLATE("scan after churn returns key order with latest values", Store, STORE_TYPES) {
  // 300 keys cross several internal read batches; updating every key with a larger value forces
  // out-of-place appends, decorrelating T2 physical offsets from key order. The scan must still
  // report keys in order, each with its latest value, and count every live entry.
  auto store = StoreFactory<Store>::make();
  constexpr int kKeys = 300;
  for (int i = 0; i < kKeys; ++i) {
    store->insert(vmemkv_test::padded_key(i, 8), static_cast<uint64_t>(i));
  }
  // Descending update order appends tail records in reverse-key sequence, so T2 physical
  // offset order is the opposite of key order: emitting reads in offset order would observably
  // reverse the callback sequence.
  for (int i = kKeys - 1; i >= 0; --i) {
    store->update(vmemkv_test::padded_key(i, 8), static_cast<uint64_t>(kKeys + i));
  }
  std::vector<std::pair<std::string, uint64_t>> seen;
  const size_t entry_count = store->scan(
      "k00000000", "k00000300", [&](std::span<const std::byte> key_bytes, std::span<const std::byte> value) {
        seen.emplace_back(vmemkv_test::span_to_string(key_bytes), test_util::decode_scanned_u64(value));
      });
  CHECK(entry_count == static_cast<size_t>(kKeys));
  REQUIRE(seen.size() == static_cast<size_t>(kKeys));
  for (int i = 0; i < kKeys; ++i) {
    CHECK(seen[static_cast<size_t>(i)].first == vmemkv_test::padded_key(i, 8));
    CHECK(seen[static_cast<size_t>(i)].second == static_cast<uint64_t>(kKeys + i));
  }
}

TEST_CASE_TEMPLATE("reorganize: CRUD still works", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  store->insert("a", 1);
  store->insert("b", 2);
  store->reorganize();
  CHECK(test_util::get_u64_sync(store, "a") == 1U);
  CHECK(test_util::get_u64_sync(store, "b") == 2U);
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
  const auto get_result = vmemkv_test::get_optional_bytes(store, "key_a");
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

namespace {

// Builds a store pre-filled with key_count uniform 'a' values for the checkpoint stress tests below.
template <typename TestStore>
auto make_seeded_checkpoint_stress_store(uint64_t capacity_bytes, int key_count, std::size_t value_bytes) {
  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), capacity_bytes);
  for (int i = 0; i < key_count; ++i) {
    REQUIRE(store->insert("key" + std::to_string(i), std::string(value_bytes, 'a')));
  }
  return store;
}

// Flags corruption_found unless value is exactly value_bytes copies of one repeated character.
inline void check_uniform_value(std::span<const std::byte> value,
                                std::size_t value_bytes,
                                std::atomic<bool> &corruption_found) {
  if (value.size() != value_bytes) {
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
}

// Every pre-seeded key must still be present at its full value size after the stress run.
template <typename StorePtr>
void check_final_value_sizes(StorePtr &store, int key_count, std::size_t value_bytes) {
  for (int i = 0; i < key_count; ++i) {
    const auto final_value = vmemkv_test::get_optional_bytes(store, "key" + std::to_string(i));
    REQUIRE(final_value.has_value());
    CHECK(final_value->size() == value_bytes);
  }
}

}  // namespace

// update()/scan() racing repeated checkpoint() cycles must never observe a torn value.
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("VMemKV: concurrent update+scan survive repeated checkpoint (stress)") {
  using TestStore = vmemkv::variants::VMemKV_Baseline;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  constexpr int kKeyCount = 50;
  constexpr std::size_t kStressValueBytes = 200;  // Non-inline; see other tests' same-size note.
  constexpr int kReorgCycles = 60;

  auto store = make_seeded_checkpoint_stress_store<TestStore>(kStoreCapacityBytes, kKeyCount, kStressValueBytes);

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
            check_uniform_value(value, kStressValueBytes, corruption_found);
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

  check_final_value_sizes(store, kKeyCount, kStressValueBytes);
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

  const std::string value(kValue200Bytes, 'a');
  for (int i = 0; i < 10; ++i) {
    REQUIRE(store->insert(vmemkv_test::padded_key("key", i, 2), value));
  }
  store->checkpoint();  // First checkpoint: creates the T2 checkpoint file.

  const uint64_t boundary_after_first = store->impl().t2().get_memory()->base_boundary;
  CHECK(boundary_after_first > 0);
  CHECK(store->impl().t2().get_memory()->base_mmap_scan != nullptr);
  CHECK(store->impl().t2().get_memory()->base_mmap_scan_seq != nullptr);
  CHECK(store->impl().t2().get_memory()->read_fd >= 0);

  // Insert more, then checkpoint again -- base_boundary advances further in place.
  for (int i = 10; i < 20; ++i) {
    REQUIRE(store->insert(vmemkv_test::padded_key("key", i, 2), value));
  }
  store->checkpoint();

  const uint64_t boundary_after_second = store->impl().t2().get_memory()->base_boundary;
  CHECK(boundary_after_second > boundary_after_first);

  // Confirm the newly-promoted range is actually reachable and correct via the base_mmap_scan
  // path (Scan), not just the always-correct fallback (Get).
  std::map<std::string, std::string> seen;
  std::ignore = store->scan(vmemkv_test::padded_key("key", 0, 2),
                            "key19~",
                            [&](std::span<const std::byte> key, std::span<const std::byte> val) {
                              seen.emplace(std::string(reinterpret_cast<const char *>(key.data()), key.size()),
                                           std::string(reinterpret_cast<const char *>(val.data()), val.size()));
                            });
  for (int i = 0; i < 20; ++i) {
    REQUIRE(seen.count(vmemkv_test::padded_key("key", i, 2)) == 1);
    CHECK(seen.at(vmemkv_test::padded_key("key", i, 2)) == value);
  }
}

// update()'s in-place path racing repeated checkpoint() cycles must never observe a torn value.
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

  auto store = make_seeded_checkpoint_stress_store<TestStore>(kStoreCapacityBytes, kKeyCount, kStressValueBytes);
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
              check_uniform_value(value, kStressValueBytes, corruption_found);
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

  check_final_value_sizes(store, kKeyCount, kStressValueBytes);
}

// Edge case: checkpoint() on an empty store still establishes base_mmap_scan/read_fd.
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
TEST_CASE_TEMPLATE("long keys sharing a 16-byte prefix: CRUD", Store, STORE_TYPES) {
  // Same reinsert-after-remove limitation as above (this case reinserts key_one).
  if constexpr (std::is_same_v<Store, vmemkv::variants::VMemKV_LeanStore>) {
    MESSAGE("skipped for LeanStore: engine cannot reinsert removed keys");
    return;
  }
  auto store = StoreFactory<Store>::make();

  const std::string prefix = "0123456789abcdef";
  const std::string key_one = prefix + "-alpha";
  const std::string key_two = prefix + "-bravo";
  const std::string key_three = prefix + "-charlie";

  CHECK(store->insert(key_one, 1));
  CHECK(store->insert(key_two, 2));
  CHECK(store->insert(key_three, 3));

  CHECK(test_util::get_u64_sync(store, key_one) == 1U);
  CHECK(test_util::get_u64_sync(store, key_two) == 2U);
  CHECK(test_util::get_u64_sync(store, key_three) == 3U);

  CHECK(test_util::get_u64_sync(store, prefix) == vmemkv::STORE_NOT_FOUND);

  CHECK_FALSE(store->insert(key_two, 99));
  CHECK(test_util::get_u64_sync(store, key_two) == 2U);

  CHECK(store->update(key_two, 22));
  CHECK(test_util::get_u64_sync(store, key_one) == 1U);
  CHECK(test_util::get_u64_sync(store, key_two) == 22U);
  CHECK(test_util::get_u64_sync(store, key_three) == 3U);

  CHECK(store->remove(key_one));
  CHECK(test_util::get_u64_sync(store, key_one) == vmemkv::STORE_NOT_FOUND);
  CHECK(test_util::get_u64_sync(store, key_two) == 22U);
  CHECK(test_util::get_u64_sync(store, key_three) == 3U);

  CHECK(store->insert(key_one, 111));
  CHECK(test_util::get_u64_sync(store, key_one) == 111U);
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
TEST_CASE_TEMPLATE("large value (>= 64B): CRUD still works", Store, STORE_TYPES) {
  // LeanStore's BTreeVI has same-size-only in-place update and no insert-after-remove path,
  // so grow/shrink updates have no correct engine path (benchmark updates are same-size).
  if constexpr (std::is_same_v<Store, vmemkv::variants::VMemKV_LeanStore>) {
    MESSAGE("skipped for LeanStore: engine cannot grow/shrink values in place");
    return;
  }
  auto store = StoreFactory<Store>::make();

  const std::string v64(kValue64Bytes, 'a');
  const std::string v200(kValue200Bytes, 'b');

  CHECK(store->insert("key1", v64));
  auto got = vmemkv_test::get_optional_bytes(store, "key1");
  if (!got.has_value()) {
    FAIL("missing key1 after insert");
  }
  CHECK(vmemkv_test::span_to_string(vmemkv_test::as_span(*got)) == v64);  // NOLINT(bugprone-unchecked-optional-access)

  CHECK_FALSE(store->insert("key1", v200));
  got = vmemkv_test::get_optional_bytes(store, "key1");
  if (!got.has_value()) {
    FAIL("missing key1 after duplicate insert");
  }
  CHECK(vmemkv_test::span_to_string(vmemkv_test::as_span(*got)) == v64);  // NOLINT(bugprone-unchecked-optional-access)

  CHECK(store->update("key1", v200));
  got = vmemkv_test::get_optional_bytes(store, "key1");
  if (!got.has_value()) {
    FAIL("missing key1 after grow update");
  }
  CHECK(vmemkv_test::span_to_string(vmemkv_test::as_span(*got)) == v200);  // NOLINT(bugprone-unchecked-optional-access)

  constexpr std::size_t kShrinkValueBytes = 16;
  const std::string v16(kShrinkValueBytes, 'c');

  CHECK(store->update("key1", v16));
  got = vmemkv_test::get_optional_bytes(store, "key1");
  if (!got.has_value()) {
    FAIL("missing key1 after shrink update");
  }
  CHECK(vmemkv_test::span_to_string(vmemkv_test::as_span(*got)) == v16);  // NOLINT(bugprone-unchecked-optional-access)

  CHECK(store->insert("key2", v64));
  auto got_key2 = vmemkv_test::get_optional_bytes(store, "key2");
  if (!got_key2.has_value()) {
    FAIL("missing key2 after insert");
  }
  CHECK(vmemkv_test::span_to_string(vmemkv_test::as_span(*got_key2)) ==
        v64);  // NOLINT(bugprone-unchecked-optional-access)

  CHECK(store->remove("key2"));
  CHECK_FALSE(vmemkv_test::get_optional_bytes(store, "key2").has_value());
  auto got_key1 = vmemkv_test::get_optional_bytes(store, "key1");
  if (!got_key1.has_value()) {
    FAIL("missing key1 after key2 removal");
  }
  CHECK(vmemkv_test::span_to_string(vmemkv_test::as_span(*got_key1)) ==
        v16);  // NOLINT(bugprone-unchecked-optional-access)
}

TEST_CASE_TEMPLATE("large N: all keys retrievable", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  constexpr int key_count = 1000;
  for (int i = 0; i < key_count; ++i) {
    store->insert("k" + std::to_string(i), static_cast<uint64_t>(i));
  }
  for (int i = 0; i < key_count; ++i) {
    CHECK(test_util::get_u64_sync(store, "k" + std::to_string(i)) == static_cast<uint64_t>(i));
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
        uint64_t value = test_util::get_u64_sync(store, "k" + std::to_string(i));
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

  CHECK_FALSE(vmemkv_test::get_optional_bytes(store, "k1").has_value());
  auto got = vmemkv_test::get_optional_bytes(store, "k2");
  if (!got.has_value()) {
    FAIL("missing k2 after checkpoint");
  }
  CHECK(vmemkv_test::span_to_string(vmemkv_test::as_span(*got)) == val2);  // NOLINT(bugprone-unchecked-optional-access)
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE_TEMPLATE("scan with integral keys verifies lexicographical ordering", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  // Keys would sort wrong as strings (1 < 10 < 100 < 2); verifies numeric order instead.
  store->insert(kScanHundred, kScanHundred);
  store->insert(1U, 1U);
  store->insert(kScanTen, kScanTen);
  store->insert(2U, 2U);

  // No reorganize() here on purpose: scan() merges the sorted and append regions live and sorts
  // the combined result, so ordering holds regardless of which region each key currently sits in.
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

// try_make_inline_payload() excludes a key whose own last byte is 0x00 from inlining -- T1's
// 16-byte zero-padded prefix would make such a key's true length ambiguous with padding on
// scan_impl()'s inline-value fast path, so it takes the normal T2-record path instead, where the
// full key is stored verbatim. A big-endian-encoded integer key that's a multiple of 256 hits this
// directly (e.g. 256 encodes as {0x00,0x00,0x01,0x00}). This test only makes sense for a store
// with UseT1InlineValue on (Var2_Inline) -- the case is specific to that fast path.
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
  const auto got = vmemkv_test::get_optional_bytes(store, 256U);
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

  using InlineStore = vmemkv::variants::VMemKV_Var2_Inline;
  auto store = std::make_unique<InlineStore>(path, kInlineStoreCapacityBytes);

  uint64_t initial_bytes = store->t2().bytes_used();
  CHECK(initial_bytes == 0);

  // 1-7 bytes: should inline (no T2 usage).
  std::vector<std::byte> val_short(kShortInlineValueBytes, kShortFillByte);
  store->insert("key1", val_short);

  CHECK(store->t2().bytes_used() == 0);

  auto res = vmemkv_test::get_optional_bytes(store, "key1");
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

  auto res_odd = vmemkv_test::get_optional_bytes(store, "key_odd");
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

  auto res_even = vmemkv_test::get_optional_bytes(store, "key_even");
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

// Read callbacks observe an owned buffer copy validated by the seqlock, never live T2 memory.
namespace {

// A value written by these tests is copies of one repeated character; a non-uniform value is a torn read.
inline auto is_uniform(std::span<const std::byte> value) -> bool {
  if (value.empty()) {
    return true;
  }
  const auto expected = value[0];
  for (std::byte b : value) {
    if (b != expected) {
      return false;
    }
  }
  return true;
}

constexpr std::size_t kTornReadValueBytes = 200;  // Non-inline; same-size updates stay in the in-place path.

// Runs one updater thread plus one reader thread for 500ms; the reader performs one
// reader_loop(store, torn_read_found) read-and-check per iteration.
template <typename StorePtr, typename ReaderLoop>
void check_no_torn_read(StorePtr &store, ReaderLoop &&reader_loop) {
  REQUIRE(store->insert("hot", std::string(kTornReadValueBytes, 'a')));

  std::atomic<bool> stop{false};
  std::atomic<bool> torn_read_found{false};

  std::thread updater([&] {
    uint64_t i = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      store->update("hot", std::string(kTornReadValueBytes, static_cast<char>('a' + (i++ % 26))));
    }
  });

  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      reader_loop(store, torn_read_found);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  stop.store(true, std::memory_order_relaxed);
  updater.join();
  reader.join();

  CHECK_FALSE(torn_read_found.load());
}

}  // namespace

TEST_CASE("VMemKV: scan callback never observes a torn read across a concurrent update (regression)") {
  using TestStore = vmemkv::variants::VMemKV_Baseline;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), kStoreCapacityBytes);

  check_no_torn_read(store, [](auto &store, std::atomic<bool> &torn_read_found) {
    std::ignore = store->scan("hot", "hot~", [&](std::span<const std::byte> /*key*/, std::span<const std::byte> value) {
      if (!is_uniform(value)) {
        torn_read_found.store(true, std::memory_order_relaxed);
      }
    });
  });
}

// Same guarantee via get() instead of scan().
TEST_CASE("VMemKV: get callback never observes a torn read across a concurrent update (regression)") {
  using TestStore = vmemkv::variants::VMemKV_Baseline;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), kStoreCapacityBytes);

  check_no_torn_read(store, [](auto &store, std::atomic<bool> &torn_read_found) {
    std::ignore = store->get("hot", [&](std::span<const std::byte> value) {
      if (!is_uniform(value)) {
        torn_read_found.store(true, std::memory_order_relaxed);
      }
    });
  });
}

// Free-spinning variant with no reorganize()/checkpoint() at all.
TEST_CASE(
    "VMemKV: free-spinning update+scan survive with no reorganize/checkpoint "
    "at all (regression)") {
  using TestStore = vmemkv::variants::VMemKV_Baseline;
  constexpr uint64_t kStoreCapacityBytes = 8ULL * 1024 * 1024;
  auto store = std::make_unique<TestStore>(reserve_temp_path().string(), kStoreCapacityBytes);

  check_no_torn_read(store, [](auto &store, std::atomic<bool> &torn_read_found) {
    std::ignore = store->scan("hot", "hot~", [&](std::span<const std::byte> /*key*/, std::span<const std::byte> value) {
      if (!is_uniform(value)) {
        torn_read_found.store(true, std::memory_order_relaxed);
      }
    });
  });
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
      REQUIRE(store->impl().t1().put(key_bytes, encoded_payload, false, 0) == ImplT::T1IndexT::PutResult::Applied);
    });
  });
}

// Shared trailing check: the straggler entry published above must be immediately readable, with
// no second checkpoint/reorganize cycle needed.
template <typename StorePtr>
void verify_straggler_readback(StorePtr &store, const std::string &straggler_value) {
  const auto straggler_readback = vmemkv_test::get_optional_bytes(store, "straggler");
  REQUIRE(straggler_readback.has_value());
  CHECK(straggler_readback->size() == straggler_value.size());
}

}  // namespace

// checkpoint's writer-stop barrier waits for in-flight writers before capturing the frontier.
TEST_CASE(
    "VMemKV: checkpoint's writer-stop barrier waits for an in-flight writer instead of "
    "capturing a frontier underneath it (regression)") {
  using TestStore = vmemkv::VMemKVStore;
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

// T2FlatFile::acquire_write_handle() registers with the reference tracker *before* checking
// writer_stop_, and retries if that check then finds the flag already true: registering first
// closes a window that ThreadReferenceTracker::wait_until_retired()'s single index-ordered scan
// (it never re-examines a slot once past it) would otherwise open -- a writer whose flag check ran
// before registering could be missed by an already-in-progress scan, letting stop_writers_and_wait()
// complete while that writer goes on to append past the frontier this same cycle just captured.
//
// Reproduces the adversarial timing deterministically via acquire_write_handle()'s hook seam
// (fires once, right after registering and before the writer_stop_ check): the writer thread
// registers, signals pre_stop_hook that it has done so, then sleeps -- guaranteeing
// stop_writers_and_wait() (called on the main thread right after pre_stop_hook returns) sets
// writer_stop_=true and starts scanning while this thread is still paused, already registered but
// not yet having checked the flag. The writer's own check must then see writer_stop_==true and
// back out/retry rather than proceed with a handle that might land past the frontier this same
// cycle already captured.
TEST_CASE(
    "VMemKV: acquire_write_handle()'s register-then-check-retry survives a writer racing the "
    "writer-stop scan (regression)") {
  using TestStore = vmemkv::VMemKVStore;
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

// An exception before T1 publish during a T2 rebuild leaves the store fully usable.
TEST_CASE("VMemKV: exception before T1 publish during a T2 rebuild leaves the store fully usable") {
  using TestStore = vmemkv::VMemKVStore;
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
  const auto readback = vmemkv_test::get_optional_bytes(store, "key");
  REQUIRE(readback.has_value());
  CHECK(std::string(reinterpret_cast<const char *>(readback->data()), readback->size()) == value);

  REQUIRE(store->update("key", std::string(200, 'w')));

  // A subsequent, unfaulted checkpoint cycle must still succeed normally.
  store->checkpoint();
  const auto final_readback = vmemkv_test::get_optional_bytes(store, "key");
  REQUIRE(final_readback.has_value());
  CHECK(final_readback->size() == 200);
}
