// test_kv_store.cpp — Correctness tests for store implementations.
//
// Each scenario is an independent TEST_CASE_TEMPLATE instantiated for every
// store type.  A fresh store is constructed per TEST_CASE (via StoreFactory)
// so tests are fully isolated.

#include <doctest/doctest.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <rivals/rocksdb_store.hpp>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#include <vmemkv/vmemkv.hpp>

// Verify that all major variants satisfy the C++20 KVStore concept
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKV_Baseline>);
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKVStore>);
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKV_RocksDB>);

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

template <typename StorePtr, typename Key>
auto get_bytes_sync(const StorePtr &store, const Key &key) -> std::optional<std::vector<std::byte>> {
  std::optional<std::vector<std::byte>> res;
  bool found =
      store->get(key, [&](std::span<const std::byte> val) { res = std::vector<std::byte>(val.begin(), val.end()); });
  if (found) {
    return res;
  }
  return std::nullopt;
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
  static std::atomic<uint64_t> counter{0};
  const uint64_t sequence_number = counter.fetch_add(1, std::memory_order_relaxed);
  std::filesystem::path temp_path =
      std::filesystem::temp_directory_path() /
      ("vmemkv_kv_" + std::to_string(static_cast<long>(::getpid())) + "_" + std::to_string(sequence_number));
  std::error_code ignored;
  std::filesystem::remove(temp_path, ignored);
  return temp_path;
}

template <typename Impl>
struct VMemKVDeleter {
  std::filesystem::path path;
  void operator()(vmemkv::StoreAdapter<Impl> *store_ptr) const noexcept {
    delete store_ptr;
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

template <typename Impl>
struct StoreFactory<vmemkv::StoreAdapter<Impl>> {
  static constexpr uint64_t kCapacityBytes = 8U << 20;  // 8 MiB

  static auto make() -> std::unique_ptr<vmemkv::StoreAdapter<Impl>, VMemKVDeleter<Impl>> {
    std::filesystem::path path = reserve_temp_path();
    if constexpr (std::is_same_v<Impl, ::RocksDBStore>) {
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

// VMemKV 自体のバリエーション（Baseline, Cumulative Steps, Ablations, Inlining）
#define VMemKVStores                                                                                               \
  vmemkv::variants::VMemKV_Var0_Baseline, vmemkv::variants::VMemKV_Var1_Bloom, vmemkv::variants::VMemKV_Var2_Simd, \
      vmemkv::variants::VMemKV_Var3_Inline, vmemkv::variants::VMemKV_Var4_Prefault

// 競合バックエンドのバリエーション（RocksDBStoreなど）
#ifdef ENABLE_ROCKSDB
#define RivalStores , vmemkv::variants::VMemKV_RocksDB
#else
#define RivalStores
#endif

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
      store->scan(key_one, key_two, [&](std::span<const std::byte>, uint64_t value) { results.push_back(value); });
  REQUIRE(scan_count == 2U);
  REQUIRE(results.size() == 2);
  CHECK(results[0] == kSmallValue);
  CHECK(results[1] == kLargeValue);
}

TEST_CASE_TEMPLATE("insert rejects STORE_NOT_FOUND payload in T1", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  CHECK_FALSE(store->insert("a", vmemkv::STORE_NOT_FOUND));
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

TEST_CASE_TEMPLATE("update rejects STORE_NOT_FOUND payload in T1", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  store->insert("a", 1);
  CHECK_FALSE(store->update("a", vmemkv::STORE_NOT_FOUND));
  CHECK(test_util::get_sync(store, "a") == 1U);
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
  entry_count = store->scan("a", "a", [](std::span<const std::byte>, uint64_t) {});
  CHECK(entry_count == 0U);
}

TEST_CASE_TEMPLATE("scan returns all entries in range", Store, STORE_TYPES) {
  auto store = StoreFactory<Store>::make();
  store->insert("b", 2);
  store->insert("c", 3);
  store->insert("d", 4);
  std::unordered_set<uint64_t> values;
  size_t entry_count = store->scan("b", "d", [&](std::span<const std::byte>, uint64_t value) { values.insert(value); });
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
  const size_t entry_count = store->scan("a", "b", [](std::span<const std::byte>, uint64_t) {});
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
  const size_t entry_count = store->scan("a", "c", [](std::span<const std::byte>, uint64_t) {});
  CHECK(entry_count == 3U);
}

TEST_CASE("VMemKV: manual reorganize forces physical storage garbage collection") {
  auto store = StoreFactory<vmemkv::variants::VMemKV_Baseline>::make();

  const int key_count = 100;
  for (int i = 0; i < key_count; ++i) {
    store->insert(std::to_string(i), static_cast<uint64_t>(i + 1000));
  }

  uint64_t initial_t2_used = store->t2().bytes_used();
  REQUIRE(initial_t2_used > 0);

  const int remove_count = 15;
  for (int i = 0; i < remove_count; ++i) {
    store->remove(std::to_string(i));
  }

  CHECK(store->t2().bytes_used() == initial_t2_used);

  store->reorganize();

  uint64_t post_reorg_t2_used = store->t2().bytes_used();
  CHECK(post_reorg_t2_used < initial_t2_used);

  for (int i = remove_count; i < key_count; ++i) {
    CHECK(test_util::get_sync(store, std::to_string(i)) == static_cast<uint64_t>(i + 1000));
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

  idx->reorganize([](uint64_t physical_offset, uint64_t) { return physical_offset; });
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
TEST_CASE("VMemKV reorganize resolves storage fragmentation") {
  auto store = StoreFactory<vmemkv::variants::VMemKV_Baseline>::make();

  std::string val1(kValue100Bytes, 'x');
  std::string val2(kValue200Bytes, 'y');

  CHECK(store->insert("k1", val1));
  CHECK(store->insert("k2", val1));

  // T2 has two records
  uint64_t bytes_used_before = store->t2().bytes_used();

  // Update key2 with larger value (cannot be in-place, appends and leaves garbage)
  CHECK(store->update("k2", val2));
  uint64_t bytes_used_after_update = store->t2().bytes_used();
  CHECK(bytes_used_after_update > bytes_used_before);

  // Delete key1 (leaves garbage in T2)
  CHECK(store->remove("k1"));

  // Now, run reorganize to defragment T2.
  // It should copy only key2's new value to a new T2 file, making bytes_used smaller.
  store->reorganize();

  uint64_t bytes_used_after_reorg = store->t2().bytes_used();

  // Confirm garbage is collected.
  CHECK(bytes_used_after_reorg < bytes_used_after_update);

  // Check that key1 is gone and key2 is retrievable
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
  // Insert values with keys that would be ordered incorrectly if serialized as simple strings.
  // E.g., string order: 1 < 10 < 100 < 2
  // Expected numeric order: 1 < 2 < 10 < 100
  store->insert(kScanHundred, kScanHundred);
  store->insert(1U, 1U);
  store->insert(kScanTen, kScanTen);
  store->insert(2U, 2U);

  // T1 Index requires reorganize to guarantee sorted scan results (moves append_region to sorted_region)
  store->reorganize();

  std::vector<uint64_t> keys;
  const size_t scan_count = store->scan(
      kScanStart, kScanUpperBound, [&](std::span<const std::byte> key_bytes, [[maybe_unused]] uint64_t value) {
        // Retrieve key by deserializing big-endian bytes (decode first 4 bytes for uint32_t)
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("Value Inlining: verify that short/8B-aligned values bypass T2 write paths") {
  const std::string path = reserve_temp_path().string();
  constexpr uint64_t kInlineStoreCapacityBytes = 8ULL * 1024ULL * 1024ULL;
  constexpr std::byte kShortFillByte{0xAB};
  constexpr std::byte kLongFillByte{0xCD};
  constexpr uint64_t kOddInlineValue = 0x003456789ABCDEF1ULL;
  constexpr uint64_t kEvenInlineValue = 0x123456789ABCDEF0ULL;

  SUBCASE("T1InlineValue behavior (1-8 bytes)") {
    using InlineStore = vmemkv::variants::VMemKV_Var3_Inline;
    auto store = std::make_unique<InlineStore>(path, kInlineStoreCapacityBytes);

    uint64_t initial_bytes = store->t2().bytes_used();
    CHECK(initial_bytes == 0);

    // 1. 1-7 bytes (should inline)
    std::vector<std::byte> val_short(kShortInlineValueBytes, kShortFillByte);
    store->insert("key1", val_short);

    // Inline success = no T2 file capacity usage
    CHECK(store->t2().bytes_used() == 0);

    // Verify read back
    auto res = test_util::get_bytes_sync(store, "key1");
    if (!res.has_value()) {
      FAIL("missing key1 inline payload");
    }
    const auto &res_value = *res;  // NOLINT(bugprone-unchecked-optional-access)
    CHECK(res_value.size() == kShortInlineValueBytes);
    CHECK(res_value[0] == kShortFillByte);

    // 2. 8 bytes Odd integer (should inline)
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
    uint64_t read_odd = 0;
    std::memcpy(&read_odd, res_odd_value.data(), kInlineValueBytes);
    CHECK(read_odd == val_odd);

    // 3. 8 bytes Even integer (should inline now!)
    uint64_t val_even = kEvenInlineValue;
    std::vector<std::byte> val_even_bytes(kInlineValueBytes);
    std::memcpy(val_even_bytes.data(), &val_even, kInlineValueBytes);

    store->insert("key_even", val_even_bytes);
    CHECK(store->t2().bytes_used() == 0);  // Correctly inlined now!

    auto res_even = test_util::get_bytes_sync(store, "key_even");
    if (!res_even.has_value()) {
      FAIL("missing key_even inline payload");
    }
    const auto &res_even_value = *res_even;  // NOLINT(bugprone-unchecked-optional-access)
    uint64_t read_even = 0;
    std::memcpy(&read_even, res_even_value.data(), kInlineValueBytes);
    CHECK(read_even == val_even);

    // 4. 9+ bytes (should bypass inlining, going to T2)
    std::vector<std::byte> val_long(kLongValueBytes, kLongFillByte);
    store->insert("key2", val_long);

    CHECK(store->t2().bytes_used() > 0);

    std::filesystem::remove(path);
  }
}

TEST_CASE("VMemKV: reorganize lost update race condition (Deterministic)") {
  const std::string path = reserve_temp_path().string();
  constexpr uint64_t kStoreCapacityBytes = 1024ULL * 1024ULL;

  using TestStore = vmemkv::variants::VMemKVStore;
  auto store = std::make_unique<TestStore>(path, kStoreCapacityBytes);

  // 1. Insert key with initial value (this sits in append_active)
  std::vector<std::byte> v1 = {std::byte{1}};
  store->insert("lost_key", v1);

  // 2. Trigger T1 reorganize. In the mapper callback (run during rebuild),
  // we concurrently update the same key "lost_key" to a new value.
  std::vector<std::byte> v2 = {std::byte{9}};
  bool update_executed = false;

  store->impl().t1().reorganize([&](uint64_t payload, uint64_t /*hash*/) -> uint64_t {
    if (!update_executed) {
      update_executed = true;
      // This update will look up and find the key in append_immutable_
      // and update it in-place. But since reorganize has already copied the key,
      // the new value 9 will be overwritten by the old value 1.
      bool ok = store->update("lost_key", v2);
      CHECK(ok == true);
    }
    return payload;
  });

  // 3. Read back the value
  auto res = test_util::get_bytes_sync(store, "lost_key");
  REQUIRE(res.has_value() == true);

  // 4. Assert that the update to 9 is preserved.
  // This CHECK will FAIL because the value rolled back to 1 (Lost Update).
  CHECK((*res)[0] == std::byte{9});

  std::filesystem::remove(path);
}
