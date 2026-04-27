// bench_kv.cpp -- T1Index vs RocksDB microbenchmarks
//
// All benchmark functions are templates over a MakeStore factory lambda so the
// same harness measures both stores with minimal code duplication.
//
// Single-threaded : nanobench time-based (one op per iteration).
// Multi-threaded  : custom fixed-duration throughput (1, 4, hw threads).
// Data-scale      : Get/Negative-Get/Scan latency vs. dataset size
//                   (Zipf a=1.0 & uniform).
//
// Build:
//   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release [-DENABLE_ROCKSDB=ON]
//   cmake --build build --target bench_kv
// Run:
//   ./build/benchmark/bench_kv

#define ANKERL_NANOBENCH_IMPLEMENT
#include <nanobench.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../src/t1/t1_index.hpp"
#ifdef ENABLE_ROCKSDB
#include "../src/kvs/rocksdb_store.hpp"
#include <unistd.h>
#endif

using T1AllOff = BasicT1Index<T1Config<false, false, false, false>>;
using T1SingleAppendMap = BasicT1Index<T1Config<true, false, false, false>>;
using T1SingleBloomFilter = BasicT1Index<T1Config<false, true, false, false>>;
using T1SingleSimdScan = BasicT1Index<T1Config<false, false, true, false>>;
using T1SingleMemoryHints = BasicT1Index<T1Config<false, false, false, true>>;
using T1Cumulative1 = BasicT1Index<T1Config<true, false, false, false>>;
using T1Cumulative2 = BasicT1Index<T1Config<true, true, false, false>>;
using T1Cumulative3 = BasicT1Index<T1Config<true, true, true, false>>;
using T1Cumulative4 = BasicT1Index<T1Config<true, true, true, true>>;

namespace nb = ankerl::nanobench;
using Clock = std::chrono::steady_clock;

// ---- Key helpers -------------------------------------------------------------
// "key_%07d" = 11 chars, fits in 16 bytes -> stored fully in StoreKey.
// Zero-padded so lexicographic order == numeric order (needed for SCAN).

static std::string ikey(int i)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "key_%07d", i);
    return buf;
}

static StoreKey make_key(int i) { return make_store_key(ikey(i)); }

template <typename Store>
static void populate(Store &store, int n)
{
    for (int i = 0; i < n; ++i)
        store.insert(make_key(i), static_cast<uint64_t>(i));
}

// ---- Zipf distribution -------------------------------------------------------
// Zipf(alpha, N): samples integers in [0,N) with P(k) proportional to 1/(k+1)^alpha.
// Uses the rejection-inversion method (Hormann & Derflinger 1996).

class ZipfDistribution
{
public:
    ZipfDistribution(int n, double alpha)
        : n_(n), alpha_(alpha),
          h_integral_x1_(h_integral(1.5) - 1.0),
          h_integral_inf_(h_integral((double)n + 0.5)),
          s_(1.0 - h_integral_inv(h_integral(1.5) - 1.0)) {}

    int operator()(std::mt19937_64 &rng)
    {
        std::uniform_real_distribution<double> uni(0.0, 1.0);
        while (true)
        {
            double u = h_integral_inf_ + uni(rng) * (h_integral_x1_ - h_integral_inf_);
            double x = h_integral_inv(u);
            int k = (int)(x + 0.5);
            if (k < 1)
                k = 1;
            if (k > n_)
                k = n_;
            if (k - x <= s_ || u >= h_integral((double)k + 0.5) - h(k))
                return k - 1;
        }
    }

private:
    int n_;
    double alpha_, h_integral_x1_, h_integral_inf_, s_;
    double h(double x) const { return std::exp(-alpha_ * std::log(x)); }
    double h_integral(double x) const
    {
        if (alpha_ == 1.0)
            return std::log(x);
        return std::exp((1.0 - alpha_) * std::log(x)) / (1.0 - alpha_);
    }
    double h_integral_inv(double u) const
    {
        if (alpha_ == 1.0)
            return std::exp(u);
        return std::exp(std::log(u * (1.0 - alpha_)) / (1.0 - alpha_));
    }
};

#ifdef ENABLE_ROCKSDB
static std::string rdb_path()
{
    static std::atomic<int> cnt{0};
    return "/tmp/vmemkv_bench_" + std::to_string(::getpid()) +
           "_" + std::to_string(cnt.fetch_add(1, std::memory_order_relaxed));
}
#endif

template <typename Fn>
static void for_each_t1_variant(Fn &&fn)
{
    fn("T1/AllOff", []
       { return std::make_unique<T1AllOff>(); });

    fn("T1/Single/UseAppendMapV", []
       { return std::make_unique<T1SingleAppendMap>(); });
    fn("T1/Single/UseBloomFilterV", []
       { return std::make_unique<T1SingleBloomFilter>(); });
    fn("T1/Single/UseSimdScanV", []
       { return std::make_unique<T1SingleSimdScan>(); });
    fn("T1/Single/UseMemoryHintsV", []
       { return std::make_unique<T1SingleMemoryHints>(); });

    fn("T1/Cumulative/UseAppendMapV", []
       { return std::make_unique<T1Cumulative1>(); });
    fn("T1/Cumulative/UseAppendMapV+UseBloomFilterV", []
       { return std::make_unique<T1Cumulative2>(); });
    fn("T1/Cumulative/UseAppendMapV+UseBloomFilterV+UseSimdScanV", []
       { return std::make_unique<T1Cumulative3>(); });
    fn("T1/Cumulative/UseAppendMapV+UseBloomFilterV+UseSimdScanV+UseMemoryHintsV", []
       { return std::make_unique<T1Cumulative4>(); });
}

template <typename Fn>
static void for_each_store_variant(Fn &&fn)
{
    for_each_t1_variant(fn);
#ifdef ENABLE_ROCKSDB
    fn("RocksDB", []
       { return std::make_unique<RocksDBStore>(rdb_path()); });
#endif
}

// =============================================================================
// Single-threaded benchmarks (nanobench)
// =============================================================================

template <typename MakeStore>
static void bench_insert(nb::Bench &b, const char *name, MakeStore make)
{
    auto store = make();
    std::atomic<int> k{0};
    b.run(name, [&]
          {
        int i = k.fetch_add(1, std::memory_order_relaxed);
        bool ok = store->insert(make_key(i), static_cast<uint64_t>(i));
        nb::doNotOptimizeAway(ok); });
}

template <typename MakeStore>
static void bench_get_hit(nb::Bench &b, const char *name, MakeStore make)
{
    constexpr int N = 5000;
    auto store = make();
    populate(*store, N);
    int i = 0;
    b.run(name, [&]
          {
        uint64_t v = store->get(make_key(i++ % N));
        nb::doNotOptimizeAway(v); });
}

template <typename MakeStore>
static void bench_get_miss(nb::Bench &b, const char *name, MakeStore make)
{
    constexpr int N = 5000;
    auto store = make();
    populate(*store, N);
    int i = 0;
    b.run(name, [&]
          {
        uint64_t v = store->get(make_key(N + i++));
        nb::doNotOptimizeAway(v); });
}

template <typename MakeStore>
static void bench_update(nb::Bench &b, const char *name, MakeStore make)
{
    constexpr int N = 5000;
    auto store = make();
    populate(*store, N);
    int i = 0;
    b.run(name, [&]
          {
        bool ok = store->update(make_key(i % N), static_cast<uint64_t>(i));
        nb::doNotOptimizeAway(ok); ++i; });
}

template <typename MakeStore>
static void bench_delete(nb::Bench &b, const char *name, MakeStore make)
{
    // Delete cannot be benchmarked with an open-ended time loop because once
    // all keys are gone the lambda degenerates into a near no-op branch.  Use
    // one fixed epoch so every measured iteration deletes a live key exactly once.
    constexpr int N = 100000;
    auto store = make();
    populate(*store, N);
    int i = 0;
    b.run(name, [&]
          {
        bool ok = store->remove(make_key(i++));
        nb::doNotOptimizeAway(ok); });
}

template <typename MakeStore>
static void bench_scan(nb::Bench &b, const char *name, int n, MakeStore make)
{
    auto store = make();
    populate(*store, n);
    b.batch(n).run(name, [&]
                   {
        size_t cnt = store->scan(make_key(0), make_key(n - 1),
                                 [](StoreKey, uint64_t v) { nb::doNotOptimizeAway(v); });
        nb::doNotOptimizeAway(cnt); });
}

template <typename MakeStore>
static void bench_scan_reorg(nb::Bench &b, const char *name, int n, MakeStore make)
{
    auto store = make();
    populate(*store, n);
    store->reorganize(); // no-op for RocksDB; merges ap->ro for T1Only
    b.batch(n).minEpochIterations(5).run(name, [&]
                                         {
        size_t cnt = store->scan(make_key(0), make_key(n - 1),
                                 [](StoreKey, uint64_t v) { nb::doNotOptimizeAway(v); });
        nb::doNotOptimizeAway(cnt); });
}

// =============================================================================
// Data-scale benchmarks (Zipf a=1.0 vs. uniform access)
// =============================================================================

template <typename MakeStore>
static void bench_get_vs_size(nb::Bench &b, const char *tag, int n, MakeStore make)
{
    auto store = make();
    populate(*store, n);
    std::mt19937_64 rng(42);
    ZipfDistribution zipf(n, 1.0);
    std::uniform_int_distribution<int> uni(0, n - 1);
    b.run(std::string(tag) + "/Zipf", [&]
          {
        uint64_t v = store->get(make_key(zipf(rng)));
        nb::doNotOptimizeAway(v); });
    b.run(std::string(tag) + "/Uniform", [&]
          {
        uint64_t v = store->get(make_key(uni(rng)));
        nb::doNotOptimizeAway(v); });
}

template <typename MakeStore>
static void bench_negative_get_vs_size_reorg(nb::Bench &b,
                                             const char *tag,
                                             int n,
                                             MakeStore make)
{
    auto store = make();
    populate(*store, n);
    store->reorganize();
    std::mt19937_64 rng(42);
    ZipfDistribution zipf(n, 1.0);
    std::uniform_int_distribution<int> uni(0, n - 1);
    b.run(std::string(tag) + "/Zipf", [&]
          {
        uint64_t v = store->get(make_key(n + zipf(rng)));
        nb::doNotOptimizeAway(v); });
    b.run(std::string(tag) + "/Uniform", [&]
          {
        uint64_t v = store->get(make_key(n + uni(rng)));
        nb::doNotOptimizeAway(v); });
}

template <typename MakeStore>
static void bench_scan_vs_size(nb::Bench &b, const char *tag,
                               int total_n, int scan_n, MakeStore make)
{
    auto store = make();
    populate(*store, total_n);
    std::mt19937_64 rng(42);
    ZipfDistribution zipf(total_n - scan_n, 1.0);
    std::uniform_int_distribution<int> uni(0, total_n - scan_n - 1);
    b.batch(scan_n).run(std::string(tag) + "/Zipf", [&]
                        {
        int s = zipf(rng);
        size_t cnt = store->scan(make_key(s), make_key(s + scan_n - 1),
                                 [](StoreKey, uint64_t v) { nb::doNotOptimizeAway(v); });
        nb::doNotOptimizeAway(cnt); });
    b.batch(scan_n).run(std::string(tag) + "/Uniform", [&]
                        {
        int s = uni(rng);
        size_t cnt = store->scan(make_key(s), make_key(s + scan_n - 1),
                                 [](StoreKey, uint64_t v) { nb::doNotOptimizeAway(v); });
        nb::doNotOptimizeAway(cnt); });
}

// =============================================================================
// Multi-threaded throughput benchmarks
// =============================================================================

static void print_mt_header(const char *title)
{
    std::printf("\n| %-44s | threads | M ops/s |\n", title);
    std::printf("|%s|---------|----------|\n", std::string(46, '-').c_str());
}

template <typename WorkFn>
static void run_mt(const char *label, int n_threads, double dur_sec, WorkFn work)
{
    std::atomic<bool> stop{false};
    std::atomic<int64_t> total{0};
    std::vector<std::thread> threads;
    threads.reserve(n_threads);
    for (int t = 0; t < n_threads; ++t)
        threads.emplace_back([&]
                             {
            int64_t local = 0;
            while (!stop.load(std::memory_order_relaxed)) { work(local); ++local; }
            total.fetch_add(local, std::memory_order_relaxed); });
    auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(dur_sec));
    stop.store(true, std::memory_order_relaxed);
    for (auto &th : threads)
        th.join();
    double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
    std::printf("| %-44s |   %3d   | %8.2f |\n", label, n_threads,
                (double)total.load() / elapsed / 1e6);
}

template <typename MakeStore>
static void bench_mt_get(const char *name, const std::vector<int> &threads,
                         double dur_sec, MakeStore make)
{
    constexpr int N = 10000;
    auto store = make();
    populate(*store, N);
    print_mt_header(name);
    for (int t : threads)
        run_mt(name, t, dur_sec, [&](int64_t ops)
               {
            uint64_t v = store->get(make_key((int)(ops % N)));
            nb::doNotOptimizeAway(v); });
}

template <typename MakeStore>
static void bench_mt_insert(const char *name, const std::vector<int> &threads,
                            double dur_sec, MakeStore make)
{
    print_mt_header(name);
    for (int t : threads)
    {
        auto store = make();
        std::atomic<int> gk{0};
        run_mt(name, t, dur_sec, [&](int64_t)
               {
            int k = gk.fetch_add(1, std::memory_order_relaxed);
            bool ok = store->insert(make_key(k), static_cast<uint64_t>(k));
            nb::doNotOptimizeAway(ok); });
    }
}

template <typename MakeStore>
static void bench_mt_mixed(const char *name, const std::vector<int> &threads,
                           double dur_sec, MakeStore make)
{
    constexpr int N = 10000;
    auto store = make();
    populate(*store, N);
    print_mt_header(name);
    for (int t : threads)
    {
        std::atomic<int64_t> seq{0};
        run_mt(name, t, dur_sec, [&](int64_t)
               {
            int64_t s = seq.fetch_add(1, std::memory_order_relaxed);
            if (s % 5 == 0) {
                bool ok = store->update(make_key((int)(s % N)), static_cast<uint64_t>(s));
                nb::doNotOptimizeAway(ok);
            } else {
                uint64_t v = store->get(make_key((int)(s % N)));
                nb::doNotOptimizeAway(v);
            } });
    }
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char **argv)
{
    double mt_dur = 20.0;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg(argv[i]);
        if (arg == "--mt-dur" && i + 1 < argc)
        {
            mt_dur = std::atof(argv[++i]);
            continue;
        }
        if (arg == "-h" || arg == "--help")
        {
            std::printf("Usage: bench_kv [--mt-dur <seconds>]\n");
            return 0;
        }
        std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return 1;
    }

    const int hw = (int)std::thread::hardware_concurrency();
    const std::vector<int> thread_counts = {1, 4, hw};

    // ---- ST / INSERT ---------------------------------------------------------
    {
        nb::Bench b;
        b.title("ST / INSERT").unit("op").warmup(1).relative(true);
        for_each_store_variant([&](const char *name, auto make)
                               {
            std::string label = std::string(name) + "/Insert";
            bench_insert(b, label.c_str(), make); });
    }

    // ---- ST / GET ------------------------------------------------------------
    {
        nb::Bench b;
        b.title("ST / GET").unit("op").warmup(1).relative(true);
        for_each_store_variant([&](const char *name, auto make)
                               {
            std::string hit = std::string(name) + "/Get/Hit";
            std::string miss = std::string(name) + "/Get/Miss";
            bench_get_hit(b, hit.c_str(), make);
            bench_get_miss(b, miss.c_str(), make); });
    }

    // ---- ST / UPDATE ---------------------------------------------------------
    {
        nb::Bench b;
        b.title("ST / UPDATE").unit("op").warmup(1).relative(true);
        for_each_store_variant([&](const char *name, auto make)
                               {
            std::string label = std::string(name) + "/Update";
            bench_update(b, label.c_str(), make); });
    }

    // ---- ST / DELETE ---------------------------------------------------------
    {
        nb::Bench b;
        b.title("ST / DELETE")
            .unit("op")
            .warmup(0)
            .epochs(1)
            .epochIterations(100000)
            .relative(true);
        for_each_store_variant([&](const char *name, auto make)
                               {
            std::string label = std::string(name) + "/Delete";
            bench_delete(b, label.c_str(), make); });
    }

    // ---- ST / SCAN -----------------------------------------------------------
    // Scan vs Scan(reorg) to reveal ap_region overhead.
    // RocksDB/Scan is always sorted (no reorg needed).
    for (int n : {1000, 5000, 10000})
    {
        nb::Bench b;
        b.title("ST / SCAN/" + std::to_string(n)).unit("op").warmup(1).relative(true);
        for_each_t1_variant([&](const char *name, auto make)
                            {
            std::string scan = std::string(name) + "/Scan";
            std::string scan_reorg = std::string(name) + "/Scan(reorg)";
            bench_scan(b, scan.c_str(), n, make);
            bench_scan_reorg(b, scan_reorg.c_str(), n, make); });
#ifdef ENABLE_ROCKSDB
        bench_scan(b, "RocksDB/Scan", n, []
                   { return std::make_unique<RocksDBStore>(rdb_path()); });
#endif
    }

    // ---- MT ------------------------------------------------------------------
    std::printf("\n\n=== Multi-threaded throughput (%.1f s per run, hw_concurrency=%d) ===\n",
                mt_dur, hw);

    for_each_store_variant([&](const char *name, auto make)
                           {
        std::string get = std::string("MT / GET    ") + name;
        std::string insert = std::string("MT / INSERT ") + name;
        std::string mixed = std::string("MT / MIXED  ") + name + " (80R/20W)";
        bench_mt_get(get.c_str(), thread_counts, mt_dur, make);
        bench_mt_insert(insert.c_str(), thread_counts, mt_dur, make);
        bench_mt_mixed(mixed.c_str(), thread_counts, mt_dur, make); });

    // ---- Data-scale: GET latency vs. dataset size ----------------------------
    std::printf("\n\n=== Data-scale: GET latency vs. dataset size ===\n");
    for (int n : {1000, 5000, 10000, 20000})
    {
        nb::Bench b;
        b.title("SCALE / GET N=" + std::to_string(n))
            .unit("op")
            .warmup(2)
            .relative(true);
        for_each_store_variant([&](const char *name, auto make)
                               { bench_get_vs_size(b, name, n, make); });
    }

    // ---- Data-scale: NEGATIVE GET latency vs. dataset size -------------------
    // Reorganize first so misses are dominated by sorted-region lookup.
    // This makes Bloom filter effects visible instead of append-region scan cost.
    std::printf("\n\n=== Data-scale: NEGATIVE GET latency vs. dataset size (after reorganize) ===\n");
    for (int n : {1000, 5000, 10000, 20000})
    {
        nb::Bench b;
        b.title("SCALE / NEGATIVE_GET(reorg) N=" + std::to_string(n))
            .unit("op")
            .warmup(2)
            .relative(true);
        for_each_store_variant([&](const char *name, auto make)
                               { bench_negative_get_vs_size_reorg(b, name, n, make); });
    }

    // ---- Data-scale: SCAN latency vs. dataset size ---------------------------
    std::printf("\n\n=== Data-scale: SCAN latency vs. dataset size (window=1000) ===\n");
    for (int n : {5000, 10000, 20000})
    {
        constexpr int WIN = 1000;
        nb::Bench b;
        b.title("SCALE / SCAN N=" + std::to_string(n) + " win=" + std::to_string(WIN))
            .unit("op")
            .warmup(2)
            .relative(true);
        for_each_store_variant([&](const char *name, auto make)
                               { bench_scan_vs_size(b, name, n, WIN, make); });
    }

    std::printf("\n");
}
