// test_kv_store.cpp — Correctness tests for store implementations.
//
// Each scenario is an independent TEST_CASE_TEMPLATE instantiated for every
// store type.  A fresh store is constructed per TEST_CASE (via StoreFactory)
// so tests are fully isolated.

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <iostream>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include <unistd.h>

#include <vmemkv/vmemkv.hpp>
#include <rivals/rocksdb_store.hpp>

// Verify that all major variants satisfy the C++20 KVStore concept
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKV_Baseline>);
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKV_T2_InlineAll>);
static_assert(vmemkv::KVStore<vmemkv::variants::VMemKV_RocksDB>);

static std::string as_string(const std::vector<std::byte> &b)
{
    if (b.empty())
        return {};
    return std::string(reinterpret_cast<const char *>(b.data()), b.size());
}

template <typename Store>
struct StoreFactory;

// Helper to reserve temp path for T2 files
static std::filesystem::path reserve_temp_path()
{
    static std::atomic<uint64_t> counter{0};
    const uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
    std::filesystem::path p =
        std::filesystem::temp_directory_path() /
        ("vmemkv_kv_" + std::to_string(static_cast<long>(::getpid())) + "_" +
         std::to_string(n));
    std::error_code ignored;
    std::filesystem::remove(p, ignored);
    return p;
}

template <typename Impl>
struct VMemKVDeleter
{
    std::filesystem::path path;
    void operator()(vmemkv::StoreAdapter<Impl> *p) const noexcept
    {
        delete p;
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

template <typename Impl>
struct StoreFactory<vmemkv::StoreAdapter<Impl>>
{
    static constexpr uint64_t kCapacityBytes = 1u << 20; // 1 MiB

    static std::unique_ptr<vmemkv::StoreAdapter<Impl>, VMemKVDeleter<Impl>> make()
    {
        std::filesystem::path path = reserve_temp_path();
        if constexpr (std::is_same_v<Impl, ::RocksDBStore>)
        {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            auto *store = new vmemkv::StoreAdapter<Impl>(path.string());
            return std::unique_ptr<vmemkv::StoreAdapter<Impl>, VMemKVDeleter<Impl>>(
                store, VMemKVDeleter<Impl>{path});
        }
        else
        {
            auto *store = new vmemkv::StoreAdapter<Impl>(path, kCapacityBytes);
            return std::unique_ptr<vmemkv::StoreAdapter<Impl>, VMemKVDeleter<Impl>>(
                store, VMemKVDeleter<Impl>{path});
        }
    }
};

// ─── Store type lists ───────────────────────────────────────────────────────

// VMemKV 自体のバリエーション（Baseline, Cumulative Steps, Ablations, Inlining）
#define VMemKVStores \
    vmemkv::variants::VMemKV_Baseline, \
    vmemkv::variants::VMemKV_Cumulative_Step1, \
    vmemkv::variants::VMemKV_Cumulative_Step2, \
    vmemkv::variants::VMemKV_Cumulative_Step3, \
    vmemkv::variants::VMemKV_Cumulative_Step4, \
    vmemkv::variants::VMemKV_Ablation_No_AppendMap, \
    vmemkv::variants::VMemKV_Ablation_No_BloomFilter, \
    vmemkv::variants::VMemKV_Ablation_No_SimdScan, \
    vmemkv::variants::VMemKV_Ablation_No_MemoryHints, \
    vmemkv::variants::VMemKV_T2_InlineAll

// 競合バックエンドのバリエーション（RocksDBStoreなど）
#ifdef ENABLE_ROCKSDB
#define RivalStores , vmemkv::variants::VMemKV_RocksDB
#else
#define RivalStores
#endif

#define STORE_TYPES VMemKVStores RivalStores
#define LONG_KEY_STORE_TYPES STORE_TYPES
#define LARGE_VALUE_STORE_TYPES STORE_TYPES

// ─── Test cases (one per scenario) ───────────────────────────────────────────

TEST_CASE_TEMPLATE("get on empty store returns STORE_NOT_FOUND", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    CHECK(s->get("x") == vmemkv::STORE_NOT_FOUND);
}

TEST_CASE_TEMPLATE("insert and get", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    CHECK(s->insert("a", 10));
    CHECK(s->get("a") == 10u);
}

TEST_CASE_TEMPLATE("insert duplicate returns false, value unchanged", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    CHECK(s->insert("a", 10));
    CHECK_FALSE(s->insert("a", 99));
    CHECK(s->get("a") == 10u);
}


TEST_CASE_TEMPLATE("integral keys use the templated convenience API", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    constexpr int key = 42;

    CHECK(s->insert(key, 10));
    CHECK(s->get(key) == 10u);
    CHECK(s->update(key, 11));
    CHECK(s->get(key) == 11u);
    CHECK(s->remove(key));
    CHECK(s->get(key) == vmemkv::STORE_NOT_FOUND);
}

namespace test_adl
{
struct CustomKey
{
    int id;
    int category;
};

inline std::array<std::byte, 8> serialize_kvs(const CustomKey &k) noexcept
{
    std::array<std::byte, 8> out{};
    uint32_t uid = static_cast<uint32_t>(k.id);
    uint32_t ucat = static_cast<uint32_t>(k.category);
    for (int i = 0; i < 4; ++i)
    {
        out[i] = static_cast<std::byte>((uid >> ((3 - i) * 8)) & 0xffu);
        out[i + 4] = static_cast<std::byte>((ucat >> ((3 - i) * 8)) & 0xffu);
    }
    return out;
}
} // namespace test_adl

TEST_CASE_TEMPLATE("custom serializers (ADL) for user-defined types", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    test_adl::CustomKey k1{42, 7};
    test_adl::CustomKey k2{42, 8};

    CHECK(s->insert(k1, 100));
    CHECK(s->insert(k2, 200));

    CHECK(s->get(k1) == 100u);
    CHECK(s->get(k2) == 200u);

    std::vector<uint64_t> results;
    s->scan(k1, k2, [&](std::span<const std::byte>, uint64_t v) {
        results.push_back(v);
    });
    REQUIRE(results.size() == 2);
    CHECK(results[0] == 100);
    CHECK(results[1] == 200);
}

TEST_CASE_TEMPLATE("insert rejects STORE_NOT_FOUND payload in T1", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    CHECK_FALSE(s->insert("a", vmemkv::STORE_NOT_FOUND));
    CHECK(s->get("a") == vmemkv::STORE_NOT_FOUND);
}

TEST_CASE_TEMPLATE("update existing key", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert("a", 1);
    CHECK(s->update("a", 2));
    CHECK(s->get("a") == 2u);
}

TEST_CASE_TEMPLATE("update missing key returns false", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    CHECK_FALSE(s->update("z", 1));
}

TEST_CASE_TEMPLATE("update rejects STORE_NOT_FOUND payload in T1", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert("a", 1);
    CHECK_FALSE(s->update("a", vmemkv::STORE_NOT_FOUND));
    CHECK(s->get("a") == 1u);
}

TEST_CASE_TEMPLATE("remove existing key", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert("a", 7);
    CHECK(s->remove("a"));
    CHECK(s->get("a") == vmemkv::STORE_NOT_FOUND);
}

TEST_CASE_TEMPLATE("remove missing key returns false", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    CHECK_FALSE(s->remove("z"));
}

TEST_CASE_TEMPLATE("re-insert after remove", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert("a", 1);
    s->remove("a");
    CHECK(s->insert("a", 2));
    CHECK(s->get("a") == 2u);
}

TEST_CASE_TEMPLATE("scan empty range returns 0", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert("b", 1);
    size_t cnt = s->scan("a", "a", [](std::span<const std::byte>, uint64_t) {});
    CHECK(cnt == 0u);
}

TEST_CASE_TEMPLATE("scan returns all entries in range", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert("b", 2);
    s->insert("c", 3);
    s->insert("d", 4);
    std::unordered_set<uint64_t> vals;
    size_t cnt = s->scan("b", "d",
                         [&](std::span<const std::byte>, uint64_t v)
                         { vals.insert(v); });
    CHECK(cnt == 3u);
    CHECK(vals.count(2) == 1);
    CHECK(vals.count(3) == 1);
    CHECK(vals.count(4) == 1);
}

TEST_CASE_TEMPLATE("scan excludes removed entries", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert("a", 1);
    s->insert("b", 2);
    s->remove("a");
    size_t cnt = s->scan("a", "b", [](std::span<const std::byte>, uint64_t) {});
    CHECK(cnt == 1u);
}

TEST_CASE_TEMPLATE("reorganize: CRUD still works", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert("a", 1);
    s->insert("b", 2);
    s->reorganize();
    CHECK(s->get("a") == 1u);
    CHECK(s->get("b") == 2u);
    CHECK(s->insert("c", 3));
    size_t cnt = s->scan("a", "c", [](std::span<const std::byte>, uint64_t) {});
    CHECK(cnt == 3u);
}

TEST_CASE_TEMPLATE("long keys sharing a 16-byte prefix: CRUD", Store, LONG_KEY_STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();

    const std::string prefix = "0123456789abcdef";
    const std::string k1 = prefix + "-alpha";
    const std::string k2 = prefix + "-bravo";
    const std::string k3 = prefix + "-charlie";

    CHECK(s->insert(k1, 1));
    CHECK(s->insert(k2, 2));
    CHECK(s->insert(k3, 3));

    CHECK(s->get(k1) == 1u);
    CHECK(s->get(k2) == 2u);
    CHECK(s->get(k3) == 3u);

    CHECK(s->get(prefix) == vmemkv::STORE_NOT_FOUND);

    CHECK_FALSE(s->insert(k2, 99));
    CHECK(s->get(k2) == 2u);

    CHECK(s->update(k2, 22));
    CHECK(s->get(k1) == 1u);
    CHECK(s->get(k2) == 22u);
    CHECK(s->get(k3) == 3u);

    CHECK(s->remove(k1));
    CHECK(s->get(k1) == vmemkv::STORE_NOT_FOUND);
    CHECK(s->get(k2) == 22u);
    CHECK(s->get(k3) == 3u);

    CHECK(s->insert(k1, 111));
    CHECK(s->get(k1) == 111u);
}

// Offset64 must disambiguate matching prefixes by hash(full_key).
TEST_CASE("Offset64 + append-map disambiguates long keys sharing a prefix")
{
    using OffsetAppendMapIndex =
        vmemkv::T1Index<vmemkv::Config<vmemkv::AppendMap>>;
    // Heap-allocate: UseAppendMap embeds a large bucket array unsuited to the stack.
    auto idx = std::make_unique<OffsetAppendMapIndex>();

    auto to_span = [](const std::string &s) {
        return std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size());
    };

    const std::string prefix = "0123456789abcdef";
    const std::string k1 = prefix + "-alpha";
    const std::string k2 = prefix + "-bravo";

    CHECK(idx->put(to_span(k1), 1));
    CHECK(idx->put(to_span(k2), 2));
    CHECK(idx->get(to_span(k1)) == 1u);
    CHECK(idx->get(to_span(k2)) == 2u);
    CHECK(idx->put(to_span(k1), 9));
    CHECK(idx->get(to_span(k1)) == 9u);

    idx->reorganize([](uint64_t p, uint64_t) { return p; });
    CHECK(idx->get(to_span(k1)) == 9u);
    CHECK(idx->get(to_span(k2)) == 2u);
}

// Large byte values are only tested for stores backed by Tier 2.
TEST_CASE_TEMPLATE("large value (>= 64B): CRUD still works", Store,
                   LARGE_VALUE_STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();

    const std::string v64(64, 'a');
    const std::string v200(200, 'b');

    CHECK(s->insert("key1", v64));
    auto got = s->get_bytes("key1");
    REQUIRE(got.has_value());
    CHECK(as_string(*got) == v64);

    CHECK_FALSE(s->insert("key1", v200));
    got = s->get_bytes("key1");
    REQUIRE(got.has_value());
    CHECK(as_string(*got) == v64);

    CHECK(s->update("key1", v200));
    got = s->get_bytes("key1");
    REQUIRE(got.has_value());
    CHECK(as_string(*got) == v200);

    const std::string v16(16, 'c');

    CHECK(s->update("key1", v16));
    got = s->get_bytes("key1");
    REQUIRE(got.has_value());
    CHECK(as_string(*got) == v16);

    CHECK(s->insert("key2", v64));
    CHECK(as_string(s->get_bytes("key2").value()) == v64);

    CHECK(s->remove("key2"));
    CHECK_FALSE(s->get_bytes("key2").has_value());
    CHECK(as_string(s->get_bytes("key1").value()) == v16);
}

TEST_CASE_TEMPLATE("large N: all keys retrievable", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i)
        s->insert("k" + std::to_string(i), static_cast<uint64_t>(i));
    for (int i = 0; i < N; ++i)
        CHECK(s->get("k" + std::to_string(i)) == static_cast<uint64_t>(i));
}

TEST_CASE_TEMPLATE("[mt] concurrent reads are consistent", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    constexpr int N = 500;
    for (int i = 0; i < N; ++i)
        s->insert("k" + std::to_string(i), static_cast<uint64_t>(i));

    std::atomic<bool> ok{true};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&]
                             {
            for (int i = 0; i < N; ++i) {
                uint64_t v = s->get("k" + std::to_string(i));
                if (v != static_cast<uint64_t>(i)) ok.store(false);
            } });
    for (auto &th : threads)
        th.join();
    CHECK(ok.load());
}

TEST_CASE("VMemKV reorganize resolves storage fragmentation")
{
    auto s = StoreFactory<vmemkv::variants::VMemKV_Baseline>::make();
    
    std::string val1(100, 'x');
    std::string val2(200, 'y');

    CHECK(s->insert("k1", val1));
    CHECK(s->insert("k2", val1));

    // T2 has two records
    uint64_t bytes_used_before = s->t2().bytes_used();
    
    // Update key2 with larger value (cannot be in-place, appends and leaves garbage)
    CHECK(s->update("k2", val2));
    uint64_t bytes_used_after_update = s->t2().bytes_used();
    CHECK(bytes_used_after_update > bytes_used_before);

    // Delete key1 (leaves garbage in T2)
    CHECK(s->remove("k1"));

    // Now, run reorganize to defragment T2.
    // It should copy only key2's new value to a new T2 file, making bytes_used smaller.
    s->reorganize();

    uint64_t bytes_used_after_reorg = s->t2().bytes_used();
    
    // Confirm garbage is collected.
    CHECK(bytes_used_after_reorg < bytes_used_after_update);
    
    // Check that key1 is gone and key2 is retrievable
    CHECK_FALSE(s->get_bytes("k1").has_value());
    auto got = s->get_bytes("k2");
    REQUIRE(got.has_value());
    CHECK(as_string(*got) == val2);
}

TEST_CASE_TEMPLATE("scan with integral keys verifies lexicographical ordering", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    // Insert values with keys that would be ordered incorrectly if serialized as simple strings.
    // E.g., string order: 1 < 10 < 100 < 2
    // Expected numeric order: 1 < 2 < 10 < 100
    s->insert(100u, 100u);
    s->insert(1u, 1u);
    s->insert(10u, 10u);
    s->insert(2u, 2u);

    // T1 Index requires reorganize to guarantee sorted scan results (moves append_region to sorted_region)
    s->reorganize();

    std::vector<uint64_t> keys;
    s->scan(0u, 1000u, [&](std::span<const std::byte> key_bytes, uint64_t val) {
        // Retrieve key by deserializing big-endian bytes (decode first 4 bytes for uint32_t)
        uint64_t key = 0;
        for (size_t i = 0; i < 4; ++i) {
            key = (key << 8) | static_cast<uint8_t>(key_bytes[i]);
        }
        keys.push_back(key);
    });

    REQUIRE(keys.size() == 4u);
    CHECK(keys[0] == 1u);
    CHECK(keys[1] == 2u);
    CHECK(keys[2] == 10u);
    CHECK(keys[3] == 100u);
}

TEST_CASE("Value Inlining: verify that short/8B-aligned values bypass T2 write paths")
{
    const std::string path = "test_inlining.bin";
    const uint64_t cap = 1024 * 1024;
    
    SUBCASE("T1InlineValue behavior (1-8 bytes)")
    {
        using InlineStore = vmemkv::variants::VMemKV_T2_InlineAll;
        auto s = std::make_unique<InlineStore>(path, cap);
        
        uint64_t initial_bytes = s->t2().bytes_used();
        CHECK(initial_bytes == 0);
        
        // 1. 1-7 bytes (should inline)
        std::vector<std::byte> val_short(5, std::byte{0xAB});
        s->insert("key1", val_short);
        
        // Inline success = no T2 file capacity usage
        CHECK(s->t2().bytes_used() == 0);
        
        // Verify read back
        auto res = s->get_bytes("key1");
        REQUIRE(res.has_value());
        CHECK(res->size() == 5);
        CHECK((*res)[0] == std::byte{0xAB});
        
        // 2. 8 bytes Odd integer (should inline)
        uint64_t val_odd = 0x003456789ABCDEF1ULL;
        std::vector<std::byte> val_odd_bytes(8);
        std::memcpy(val_odd_bytes.data(), &val_odd, 8);
        
        s->insert("key_odd", val_odd_bytes);
        CHECK(s->t2().bytes_used() == 0);
        
        auto res_odd = s->get_bytes("key_odd");
        REQUIRE(res_odd.has_value());
        uint64_t read_odd = 0;
        std::memcpy(&read_odd, res_odd->data(), 8);
        CHECK(read_odd == val_odd);

        // 3. 8 bytes Even integer (should inline now!)
        uint64_t val_even = 0x123456789ABCDEF0ULL;
        std::vector<std::byte> val_even_bytes(8);
        std::memcpy(val_even_bytes.data(), &val_even, 8);
        
        s->insert("key_even", val_even_bytes);
        CHECK(s->t2().bytes_used() == 0); // Correctly inlined now!
        
        auto res_even = s->get_bytes("key_even");
        REQUIRE(res_even.has_value());
        uint64_t read_even = 0;
        std::memcpy(&read_even, res_even->data(), 8);
        CHECK(read_even == val_even);
        
        // 4. 9+ bytes (should bypass inlining, going to T2)
        std::vector<std::byte> val_long(12, std::byte{0xCD});
        s->insert("key2", val_long);
        
        CHECK(s->t2().bytes_used() > 0);
        
        std::filesystem::remove(path);
    }
}
