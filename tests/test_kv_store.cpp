// test_kv_store.cpp — Correctness tests for store implementations.
//
// Each scenario is an independent TEST_CASE_TEMPLATE instantiated for every
// store type.  A fresh store is constructed per TEST_CASE (via StoreFactory)
// so tests are fully isolated.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../src/t1/t1_index.hpp"

#ifdef ENABLE_ROCKSDB
#include "../src/kvs/rocksdb_store.hpp"
#include <unistd.h>
#endif

// ─── Store factory ────────────────────────────────────────────────────────────

template <typename Store>
struct StoreFactory;

template <>
struct StoreFactory<T1Index>
{
    static std::unique_ptr<T1Index> make()
    {
        return std::make_unique<T1Index>();
    }
};

using T1IndexOptimized =
    BasicT1Index<T1Config<true, true, true, true, T1PayloadMode::Inline64>>;

template <>
struct StoreFactory<T1IndexOptimized>
{
    static std::unique_ptr<T1IndexOptimized> make()
    {
        return std::make_unique<T1IndexOptimized>();
    }
};

#ifdef ENABLE_ROCKSDB
template <>
struct StoreFactory<RocksDBStore>
{
    static std::unique_ptr<RocksDBStore> make()
    {
        std::string path =
            "/tmp/vmemkv_test_" + std::to_string(static_cast<int>(::getpid()));
        return std::make_unique<RocksDBStore>(path);
    }
};
#endif

// ─── Store type list ──────────────────────────────────────────────────────────

#ifdef ENABLE_ROCKSDB
#define STORE_TYPES T1Index, T1IndexOptimized, RocksDBStore
#else
#define STORE_TYPES T1Index, T1IndexOptimized
#endif

// Helper: short alias for make_store_key
static const auto k = [](const char *str)
{ return make_store_key(str); };

// ─── Test cases (one per scenario) ───────────────────────────────────────────

TEST_CASE_TEMPLATE("get on empty store returns STORE_NOT_FOUND", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    CHECK(s->get(k("x")) == STORE_NOT_FOUND);
}

TEST_CASE_TEMPLATE("insert and get", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    CHECK(s->insert(k("a"), 10));
    CHECK(s->get(k("a")) == 10u);
}

TEST_CASE_TEMPLATE("insert duplicate returns false, value unchanged", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    CHECK(s->insert(k("a"), 10));
    CHECK_FALSE(s->insert(k("a"), 99));
    CHECK(s->get(k("a")) == 10u);
}

TEST_CASE_TEMPLATE("insert rejects STORE_NOT_FOUND payload in T1", Store, T1Index, T1IndexOptimized)
{
    auto s = StoreFactory<Store>::make();
    CHECK_FALSE(s->insert(k("a"), STORE_NOT_FOUND));
    CHECK(s->get(k("a")) == STORE_NOT_FOUND);
}

TEST_CASE_TEMPLATE("update existing key", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert(k("a"), 1);
    CHECK(s->update(k("a"), 2));
    CHECK(s->get(k("a")) == 2u);
}

TEST_CASE_TEMPLATE("update missing key returns false", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    CHECK_FALSE(s->update(k("z"), 1));
}

TEST_CASE_TEMPLATE("update rejects STORE_NOT_FOUND payload in T1", Store, T1Index, T1IndexOptimized)
{
    auto s = StoreFactory<Store>::make();
    CHECK(s->insert(k("a"), 1));
    CHECK_FALSE(s->update(k("a"), STORE_NOT_FOUND));
    CHECK(s->get(k("a")) == 1u);
}

TEST_CASE_TEMPLATE("remove existing key", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert(k("a"), 7);
    CHECK(s->remove(k("a")));
    CHECK(s->get(k("a")) == STORE_NOT_FOUND);
}

TEST_CASE_TEMPLATE("remove missing key returns false", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    CHECK_FALSE(s->remove(k("z")));
}

TEST_CASE_TEMPLATE("re-insert after remove", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert(k("a"), 1);
    s->remove(k("a"));
    CHECK(s->insert(k("a"), 2));
    CHECK(s->get(k("a")) == 2u);
}

TEST_CASE_TEMPLATE("scan empty range returns 0", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert(k("b"), 1);
    size_t cnt = s->scan(k("a"), k("a"), [](StoreKey, uint64_t) {});
    CHECK(cnt == 0u);
}

TEST_CASE_TEMPLATE("scan returns all entries in range", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert(k("b"), 2);
    s->insert(k("c"), 3);
    s->insert(k("d"), 4);
    std::unordered_set<uint64_t> vals;
    size_t cnt = s->scan(k("b"), k("d"),
                         [&](StoreKey, uint64_t v)
                         { vals.insert(v); });
    CHECK(cnt == 3u);
    CHECK(vals.count(2) == 1);
    CHECK(vals.count(3) == 1);
    CHECK(vals.count(4) == 1);
}

TEST_CASE_TEMPLATE("scan excludes removed entries", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert(k("a"), 1);
    s->insert(k("b"), 2);
    s->remove(k("a"));
    size_t cnt = s->scan(k("a"), k("b"), [](StoreKey, uint64_t) {});
    CHECK(cnt == 1u);
}

TEST_CASE_TEMPLATE("reorganize: CRUD still works", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    s->insert(k("a"), 1);
    s->insert(k("b"), 2);
    s->reorganize();
    CHECK(s->get(k("a")) == 1u);
    CHECK(s->get(k("b")) == 2u);
    CHECK(s->insert(k("c"), 3));
    size_t cnt = s->scan(k("a"), k("c"), [](StoreKey, uint64_t) {});
    CHECK(cnt == 3u);
}

TEST_CASE_TEMPLATE("large N: all keys retrievable", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i)
        s->insert(make_store_key("k" + std::to_string(i)),
                  static_cast<uint64_t>(i));
    for (int i = 0; i < N; ++i)
        CHECK(s->get(make_store_key("k" + std::to_string(i))) ==
              static_cast<uint64_t>(i));
}

TEST_CASE_TEMPLATE("[mt] concurrent reads are consistent", Store, STORE_TYPES)
{
    auto s = StoreFactory<Store>::make();
    constexpr int N = 500;
    for (int i = 0; i < N; ++i)
        s->insert(make_store_key("k" + std::to_string(i)),
                  static_cast<uint64_t>(i));

    std::atomic<bool> ok{true};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&]
                             {
            for (int i = 0; i < N; ++i) {
                uint64_t v = s->get(make_store_key("k" + std::to_string(i)));
                if (v != static_cast<uint64_t>(i)) ok.store(false);
            } });
    for (auto &th : threads)
        th.join();
    CHECK(ok.load());
}
