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
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <rivals/rocksdb_store.hpp>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <vmemkv/vmemkv.hpp>

namespace nb = ankerl::nanobench;
using Clock = std::chrono::steady_clock;

namespace {
constexpr std::size_t kIndexKeyBufferBytes = 16;
constexpr std::size_t kInlineValueBytes = 8;
constexpr double kZipfPivot = 1.5;
constexpr double kHalfStep = 0.5;
constexpr uint64_t kStoreCapacityBytes = 4ULL << 30;
constexpr uint64_t kBenchmarkSeed = 42;
constexpr int kScanReorgMinEpochIterations = 5;
constexpr int kMtTitleWidth = 44;
constexpr int kMtDividerWidth = 46;
constexpr int kMtValueWidth = 8;
constexpr int kHotValueBytes = 256;
constexpr int kWarmValueBytes = 1024;
constexpr double kMinimumElapsedSeconds = 0.0001;
constexpr int kMaxMtInsertKeys = 2'000'000;
constexpr double kDefaultMtDurationSeconds = 20.0;
constexpr int kDeleteEpochIterations = 100'000;
}  // namespace

// ---- Key helpers -------------------------------------------------------------
// "key_%07d" = 11 chars, fits in the 16-byte index prefix.
// Zero-padded so lexicographic order == numeric order (needed for SCAN).

static auto ikey(int index) -> std::string {
  std::array<char, kIndexKeyBufferBytes> key_buffer{};
  std::snprintf(key_buffer.data(), key_buffer.size(), "key_%07d", index);
  return key_buffer.data();
}

static auto make_key(int index) -> std::string { return ikey(index); }

struct PopulateOptions {
  size_t key_count;
  size_t value_size = kInlineValueBytes;
};

template <typename Store>
static void populate(Store &store, PopulateOptions options) {
  std::string dummy(options.value_size, 'a');
  for (size_t i = 0; i < options.key_count; ++i) {
    store.insert(make_key(static_cast<int>(i)), dummy);
  }
}

// ---- Zipf distribution -------------------------------------------------------
// Zipf(alpha, N): samples integers in [0,N) with P(k) proportional to 1/(k+1)^alpha.
// Uses the rejection-inversion method (Hormann & Derflinger 1996).

class ZipfDistribution {
 public:
  struct Params {
    int item_count;
    double alpha;
  };

  explicit ZipfDistribution(Params params)
      : item_count_(params.item_count),
        alpha_(params.alpha),
        h_integral_x1_(h_integral(kZipfPivot) - 1.0),
        h_integral_inf_(h_integral(static_cast<double>(params.item_count) + kHalfStep)),
        s_(1.0 - h_integral_inv(h_integral(kZipfPivot) - 1.0)) {}

  auto operator()(std::mt19937_64 &rng) -> int {
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    while (true) {
      double uniform_sample = h_integral_inf_ + uni(rng) * (h_integral_x1_ - h_integral_inf_);
      double sample_position = h_integral_inv(uniform_sample);
      int rank = static_cast<int>(std::lround(sample_position));
      if (rank < 1) {
        rank = 1;
      }
      if (rank > item_count_) {
        rank = item_count_;
      }
      if (rank - sample_position <= s_ ||
          uniform_sample >= h_integral(static_cast<double>(rank) + kHalfStep) - h(rank)) {
        return rank - 1;
      }
    }
  }

 private:
  int item_count_;
  double alpha_, h_integral_x1_, h_integral_inf_, s_;
  [[nodiscard]] auto h(double input_value) const -> double { return std::exp(-alpha_ * std::log(input_value)); }
  [[nodiscard]] auto h_integral(double input_value) const -> double {
    if (alpha_ == 1.0) {
      return std::log(input_value);
    }
    return std::exp((1.0 - alpha_) * std::log(input_value)) / (1.0 - alpha_);
  }
  [[nodiscard]] auto h_integral_inv(double integral_value) const -> double {
    if (alpha_ == 1.0) {
      return std::exp(integral_value);
    }
    return std::exp(std::log(integral_value * (1.0 - alpha_)) / (1.0 - alpha_));
  }
};

template <typename Constructor>
static auto make_vmemkv(const std::string &path, Constructor &&constructor) {
  std::error_code error_code;
  std::filesystem::remove(path, error_code);
  return constructor();
}

template <typename Tuple, typename Visitor, std::size_t... Indices>
void for_each_in_tuple_impl(Visitor &&visitor, std::index_sequence<Indices...> index_sequence_unused) {
  (void)index_sequence_unused;
  (visitor(static_cast<std::tuple_element_t<Indices, Tuple> *>(nullptr)), ...);
}

template <typename Tuple, typename Visitor>
void for_each_in_tuple(Visitor &&visitor) {
  for_each_in_tuple_impl<Tuple>(std::forward<Visitor>(visitor), std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename Visitor>
static void for_each_store_variant(Visitor &&visitor) {
  for_each_in_tuple<vmemkv::variants::AllPossibleTypes>([&](auto *dummy) {
    using Store = std::remove_pointer_t<decltype(dummy)>;
    if constexpr (Store::kIsEnabled) {
      std::string filename = "bench_" + Store::name() + ".bin";
      std::replace(filename.begin(), filename.end(), '/', '_');

      visitor(Store::name().c_str(), [&]() {
        return make_vmemkv(filename, [&]() {
          if constexpr (std::is_same_v<Store, vmemkv::variants::VMemKV_RocksDB>) {
            return std::make_unique<Store>(filename);
          } else {
            return std::make_unique<Store>(filename, kStoreCapacityBytes);
          }
        });
      });
    }
  });
}

// =============================================================================
// Single-threaded benchmarks (nanobench)
// =============================================================================

template <typename MakeStore>
static void bench_insert(nb::Bench &bench, const char *name, MakeStore make, size_t val_size = kInlineValueBytes) {
  auto store = make();
  std::atomic<int> key_counter{0};
  std::string dummy(val_size, 'a');
  bench.run(name, [&] {
    int key_index = key_counter.fetch_add(1, std::memory_order_relaxed);
    bool inserted = store->insert(make_key(key_index), dummy);
    nb::doNotOptimizeAway(inserted);
  });
}

template <typename MakeStore>
static void bench_get_hit(nb::Bench &bench, const char *name, MakeStore make, size_t val_size = kInlineValueBytes) {
  constexpr int key_count = 5000;
  auto store = make();
  populate(*store, {key_count, val_size});
  int key_index = 0;
  bench.run(name, [&] {
    if (val_size == kInlineValueBytes) {
      uint64_t value = store->get(make_key(key_index++ % key_count));
      nb::doNotOptimizeAway(value);
    } else {
      auto value = store->get_bytes(make_key(key_index++ % key_count));
      nb::doNotOptimizeAway(value);
    }
  });
}

template <typename MakeStore>
static void bench_get_miss(nb::Bench &bench, const char *name, MakeStore make, size_t val_size = kInlineValueBytes) {
  constexpr int key_count = 5000;
  auto store = make();
  populate(*store, {key_count, val_size});
  int key_index = 0;
  bench.run(name, [&] {
    if (val_size == kInlineValueBytes) {
      uint64_t value = store->get(make_key(key_count + key_index++));
      nb::doNotOptimizeAway(value);
    } else {
      auto value = store->get_bytes(make_key(key_count + key_index++));
      nb::doNotOptimizeAway(value);
    }
  });
}

template <typename MakeStore>
static void bench_update(nb::Bench &bench, const char *name, MakeStore make, size_t val_size = kInlineValueBytes) {
  constexpr int key_count = 5000;
  auto store = make();
  populate(*store, {key_count, val_size});
  int key_index = 0;
  std::string dummy(val_size, 'a');
  bench.run(name, [&] {
    bool updated = store->update(make_key(key_index % key_count), dummy);
    nb::doNotOptimizeAway(updated);
    ++key_index;
  });
}

template <typename MakeStore>
static void bench_delete(nb::Bench &bench, const char *name, MakeStore make, size_t val_size = kInlineValueBytes) {
  // Delete cannot be benchmarked with an open-ended time loop because once
  // all keys are gone the lambda degenerates into a near no-op branch.  Use
  // one fixed epoch so every measured iteration deletes a live key exactly once.
  constexpr int key_count = 100000;
  auto store = make();
  populate(*store, {key_count, val_size});
  int key_index = 0;
  bench.run(name, [&] {
    bool removed = store->remove(make_key(key_index++));
    nb::doNotOptimizeAway(removed);
  });
}

template <typename MakeStore>
static void bench_scan(nb::Bench &bench, const char *name, int scan_count, MakeStore make) {
  auto store = make();
  populate(*store, {static_cast<size_t>(scan_count)});
  bench.batch(scan_count).run(name, [&] {
    size_t result_count = store->scan(make_key(0),
                                      make_key(scan_count - 1),
                                      [](std::span<const std::byte>, uint64_t value) { nb::doNotOptimizeAway(value); });
    nb::doNotOptimizeAway(result_count);
  });
}

template <typename MakeStore>
static void bench_scan_reorg(nb::Bench &bench, const char *name, int scan_count, MakeStore make) {
  auto store = make();
  populate(*store, {static_cast<size_t>(scan_count)});
  store->reorganize();  // no-op for RocksDB; merges ap->ro for T1Only
  bench.batch(scan_count).minEpochIterations(kScanReorgMinEpochIterations).run(name, [&] {
    size_t result_count = store->scan(make_key(0),
                                      make_key(scan_count - 1),
                                      [](std::span<const std::byte>, uint64_t value) { nb::doNotOptimizeAway(value); });
    nb::doNotOptimizeAway(result_count);
  });
}

// =============================================================================
// Data-scale benchmarks (Zipf a=1.0 vs. uniform access)
// =============================================================================

template <typename MakeStore>
static void bench_get_vs_size(nb::Bench &bench, const char *tag, int item_count, MakeStore make) {
  auto store = make();
  populate(*store, {static_cast<size_t>(item_count)});
  std::mt19937_64 rng(kBenchmarkSeed);
  ZipfDistribution zipf({item_count, 1.0});
  std::uniform_int_distribution<int> uniform_index(0, item_count - 1);
  bench.run(std::string(tag) + "/Zipf", [&] {
    uint64_t value = store->get(make_key(zipf(rng)));
    nb::doNotOptimizeAway(value);
  });
  bench.run(std::string(tag) + "/Uniform", [&] {
    uint64_t value = store->get(make_key(uniform_index(rng)));
    nb::doNotOptimizeAway(value);
  });
}

template <typename MakeStore>
static void bench_negative_get_vs_size_reorg(nb::Bench &bench, const char *tag, int item_count, MakeStore make) {
  auto store = make();
  populate(*store, {static_cast<size_t>(item_count)});
  store->reorganize();
  std::mt19937_64 rng(kBenchmarkSeed);
  ZipfDistribution zipf({item_count, 1.0});
  std::uniform_int_distribution<int> uniform_index(0, item_count - 1);
  bench.run(std::string(tag) + "/Zipf", [&] {
    uint64_t value = store->get(make_key(item_count + zipf(rng)));
    nb::doNotOptimizeAway(value);
  });
  bench.run(std::string(tag) + "/Uniform", [&] {
    uint64_t value = store->get(make_key(item_count + uniform_index(rng)));
    nb::doNotOptimizeAway(value);
  });
}

template <typename MakeStore>
static void bench_scan_vs_size(
    nb::Bench &bench, const char *tag, int total_item_count, int scan_count, MakeStore make) {
  auto store = make();
  populate(*store, {static_cast<size_t>(total_item_count)});
  std::mt19937_64 rng(kBenchmarkSeed);
  ZipfDistribution zipf({total_item_count - scan_count, 1.0});
  std::uniform_int_distribution<int> uniform_index(0, total_item_count - scan_count - 1);
  bench.batch(scan_count).run(std::string(tag) + "/Zipf", [&] {
    int scan_start = zipf(rng);
    size_t result_count = store->scan(make_key(scan_start),
                                      make_key(scan_start + scan_count - 1),
                                      [](std::span<const std::byte>, uint64_t value) { nb::doNotOptimizeAway(value); });
    nb::doNotOptimizeAway(result_count);
  });
  bench.batch(scan_count).run(std::string(tag) + "/Uniform", [&] {
    int scan_start = uniform_index(rng);
    size_t result_count = store->scan(make_key(scan_start),
                                      make_key(scan_start + scan_count - 1),
                                      [](std::span<const std::byte>, uint64_t value) { nb::doNotOptimizeAway(value); });
    nb::doNotOptimizeAway(result_count);
  });
}

// =============================================================================
// Multi-threaded throughput benchmarks
// =============================================================================

static void print_mt_header(const char *title) {
  std::cout << "\n| " << std::left << std::setw(kMtTitleWidth) << title << " | threads | M ops/s |\n";
  std::cout << "|" << std::string(kMtDividerWidth, '-') << "|---------|----------|\n";
  std::cout << std::right;
}

struct MtRunConfig {
  int thread_count;
  double duration_sec;
};

template <typename WorkFn>
static void run_mt(const char *label, MtRunConfig cfg, WorkFn work) {
  std::atomic<bool> stop{false};
  std::atomic<int64_t> total{0};
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(cfg.thread_count));
  auto start_time = Clock::now();
  for (int thread_index = 0; thread_index < cfg.thread_count; ++thread_index) {
    threads.emplace_back([&] {
      int64_t local_count = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        if (!work(local_count)) {
          stop.store(true, std::memory_order_relaxed);
          break;
        }
        ++local_count;
      }
      total.fetch_add(local_count, std::memory_order_relaxed);
    });
  }

  double remaining = cfg.duration_sec;
  constexpr double interval = 0.01;
  while (remaining > 0 && !stop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::duration<double>(interval));
    remaining -= interval;
  }
  stop.store(true, std::memory_order_relaxed);

  for (auto &worker_thread : threads) {
    worker_thread.join();
  }
  double elapsed = std::chrono::duration<double>(Clock::now() - start_time).count();
  if (elapsed <= 0) {
    elapsed = kMinimumElapsedSeconds;
  }
  const double throughput_mops = static_cast<double>(total.load()) / elapsed / 1e6;
  std::cout << "| " << std::left << std::setw(kMtTitleWidth) << label << " |   " << std::right << std::setw(3)
            << cfg.thread_count << "   | " << std::fixed << std::setw(kMtValueWidth) << std::setprecision(2)
            << throughput_mops << " |\n";
}

template <typename MakeStore>
static void bench_mt_get(const char *name,
                         const std::vector<int> &thread_counts,
                         double dur_sec,
                         MakeStore make,
                         size_t val_size = kInlineValueBytes) {
  constexpr int key_count = 10000;
  auto store = make();
  populate(*store, {key_count, val_size});
  print_mt_header(name);
  for (int thread_count : thread_counts) {
    run_mt(name, MtRunConfig{thread_count, dur_sec}, [&](int64_t operation_count) -> bool {
      if (val_size == kInlineValueBytes) {
        uint64_t value = store->get(make_key(static_cast<int>(operation_count % key_count)));
        nb::doNotOptimizeAway(value);
      } else {
        auto value = store->get_bytes(make_key(static_cast<int>(operation_count % key_count)));
        nb::doNotOptimizeAway(value);
      }
      return true;
    });
  }
}

template <typename MakeStore>
static void bench_mt_insert(const char *name,
                            const std::vector<int> &thread_counts,
                            double dur_sec,
                            MakeStore make,
                            size_t val_size = kInlineValueBytes) {
  print_mt_header(name);
  for (int thread_count : thread_counts) {
    auto store = make();
    std::atomic<int> global_key_counter{0};
    std::string dummy(val_size, 'a');
    run_mt(name, MtRunConfig{thread_count, dur_sec}, [&](int64_t) -> bool {
      int key_index = global_key_counter.fetch_add(1, std::memory_order_relaxed);
      if (key_index >= kMaxMtInsertKeys) {
        return false;
      }
      bool inserted = store->insert(make_key(key_index), dummy);
      nb::doNotOptimizeAway(inserted);
      return true;
    });
  }
}

template <typename MakeStore>
static void bench_mt_mixed(const char *name,
                           const std::vector<int> &thread_counts,
                           double dur_sec,
                           MakeStore make,
                           size_t val_size = kInlineValueBytes) {
  constexpr int key_count = 10000;
  auto store = make();
  populate(*store, {key_count, val_size});
  print_mt_header(name);
  for (int thread_count : thread_counts) {
    std::atomic<int64_t> sequence_number{0};
    std::string dummy(val_size, 'a');
    run_mt(name, MtRunConfig{thread_count, dur_sec}, [&](int64_t) -> bool {
      int64_t operation_number = sequence_number.fetch_add(1, std::memory_order_relaxed);
      if (operation_number % kScanReorgMinEpochIterations == 0) {
        bool updated = store->update(make_key(static_cast<int>(operation_number % key_count)), dummy);
        nb::doNotOptimizeAway(updated);
      } else {
        if (val_size == kInlineValueBytes) {
          uint64_t value = store->get(make_key(static_cast<int>(operation_number % key_count)));
          nb::doNotOptimizeAway(value);
        } else {
          auto value = store->get_bytes(make_key(static_cast<int>(operation_number % key_count)));
          nb::doNotOptimizeAway(value);
        }
      }
      return true;
    });
  }
}

// =============================================================================
// main
// =============================================================================

auto main(int argc, char **argv) -> int {
  try {
    double mt_duration_seconds = kDefaultMtDurationSeconds;
    for (int i = 1; i < argc; ++i) {
      std::string_view arg(argv[i]);
      if (arg == "--mt-dur" && i + 1 < argc) {
        mt_duration_seconds = std::atof(argv[++i]);
        continue;
      }
      if (arg == "-h" || arg == "--help") {
        std::cout << "Usage: bench_kv [--mt-dur <seconds>]\n";
        return 0;
      }
      std::cerr << "Unknown option: " << argv[i] << '\n';
      return 1;
    }

    const int hardware_threads = static_cast<int>(std::thread::hardware_concurrency());
    const std::vector<int> thread_counts = {1, 4, hardware_threads};

    // ---- ST / INSERT ---------------------------------------------------------
    {
      nb::Bench bench;
      bench.title("ST / INSERT").unit("op").warmup(1).relative(true);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Insert";
        bench_insert(bench, label.c_str(), make);
      });
    }

    // ---- ST / GET ------------------------------------------------------------
    {
      nb::Bench bench;
      bench.title("ST / GET").unit("op").warmup(1).relative(true);
      for_each_store_variant([&](const char *name, auto make) {
        std::string hit = std::string(name) + "/Get/Hit";
        std::string miss = std::string(name) + "/Get/Miss";
        bench_get_hit(bench, hit.c_str(), make);
        bench_get_miss(bench, miss.c_str(), make);
      });
    }

    // ---- ST / UPDATE ---------------------------------------------------------
    {
      nb::Bench bench;
      bench.title("ST / UPDATE").unit("op").warmup(1).relative(true);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Update";
        bench_update(bench, label.c_str(), make);
      });
    }

    // ---- ST / DELETE ---------------------------------------------------------
    {
      nb::Bench bench;
      bench.title("ST / DELETE").unit("op").warmup(0).epochs(1).epochIterations(kDeleteEpochIterations).relative(true);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Delete";
        bench_delete(bench, label.c_str(), make);
      });
    }

    // ---- ST / SCAN -----------------------------------------------------------
    // Scan vs Scan(reorg) to reveal ap_region overhead.
    // RocksDB/Scan is always sorted (no reorg needed).
    for (int dataset_size : {1000, 5000, 10000}) {
      nb::Bench bench;
      bench.title("ST / SCAN/" + std::to_string(dataset_size)).unit("op").warmup(1).relative(true);
      for_each_store_variant([&](const char *name, auto make) {
        std::string scan = std::string(name) + "/Scan";
        bench_scan(bench, scan.c_str(), dataset_size, make);

        if (std::string(name) != "RocksDB") {
          std::string scan_reorg = std::string(name) + "/Scan(reorg)";
          bench_scan_reorg(bench, scan_reorg.c_str(), dataset_size, make);
        }
      });
    }

    // ---- MT ------------------------------------------------------------------
    std::cout << "\n\n=== Multi-threaded throughput (" << std::fixed << std::setprecision(1) << mt_duration_seconds
              << " s per run, hw_concurrency=" << hardware_threads << ") ===\n";

    for_each_store_variant([&](const char *name, auto make) {
      std::string get = std::string("MT / GET    ") + name;
      std::string insert = std::string("MT / INSERT ") + name;
      std::string mixed = std::string("MT / MIXED  ") + name + " (80R/20W)";
      bench_mt_get(get.c_str(), thread_counts, mt_duration_seconds, make);
      bench_mt_insert(insert.c_str(), thread_counts, mt_duration_seconds, make);
      bench_mt_mixed(mixed.c_str(), thread_counts, mt_duration_seconds, make);
    });

    // ---- Data-scale: GET latency vs. dataset size ----------------------------
    std::cout << "\n\n=== Data-scale: GET latency vs. dataset size ===\n";
    for (int dataset_size : {1000, 5000, 10000, 20000}) {
      nb::Bench bench;
      bench.title("SCALE / GET N=" + std::to_string(dataset_size)).unit("op").warmup(2).relative(true);
      for_each_store_variant([&](const char *name, auto make) { bench_get_vs_size(bench, name, dataset_size, make); });
    }

    // ---- Data-scale: NEGATIVE GET latency vs. dataset size -------------------
    // Reorganize first so misses are dominated by sorted-region lookup.
    // This makes Bloom filter effects visible instead of append-region scan cost.
    std::cout << "\n\n=== Data-scale: NEGATIVE GET latency vs. dataset size (after reorganize) ===\n";
    for (int dataset_size : {1000, 5000, 10000, 20000}) {
      nb::Bench bench;
      bench.title("SCALE / NEGATIVE_GET(reorg) N=" + std::to_string(dataset_size)).unit("op").warmup(2).relative(true);
      for_each_store_variant(
          [&](const char *name, auto make) { bench_negative_get_vs_size_reorg(bench, name, dataset_size, make); });
    }

    // ---- Data-scale: SCAN latency vs. dataset size ---------------------------
    std::cout << "\n\n=== Data-scale: SCAN latency vs. dataset size (window=1000) ===\n";
    for (int dataset_size : {5000, 10000, 20000}) {
      constexpr int scan_window = 1000;
      nb::Bench bench;
      bench.title("SCALE / SCAN N=" + std::to_string(dataset_size) + " win=" + std::to_string(scan_window))
          .unit("op")
          .warmup(2)
          .relative(true);
      for_each_store_variant(
          [&](const char *name, auto make) { bench_scan_vs_size(bench, name, dataset_size, scan_window, make); });
    }

    // ---- MySQL Row Simulation: 256B Value ------------------------------------
    std::cout << "\n=== MySQL Row Simulation: 256B Value ===\n";
    {
      nb::Bench bench;
      bench.title("ST / INSERT (256B)").unit("op").warmup(1).relative(true);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Insert/256B";
        bench_insert(bench, label.c_str(), make, kHotValueBytes);
      });
    }
    {
      nb::Bench bench;
      bench.title("ST / GET (256B)").unit("op").warmup(1).relative(true);
      for_each_store_variant([&](const char *name, auto make) {
        std::string hit = std::string(name) + "/Get/Hit/256B";
        std::string miss = std::string(name) + "/Get/Miss/256B";
        bench_get_hit(bench, hit.c_str(), make, kHotValueBytes);
        bench_get_miss(bench, miss.c_str(), make, kHotValueBytes);
      });
    }
    {
      nb::Bench bench;
      bench.title("ST / UPDATE (256B)").unit("op").warmup(1).relative(true);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Update/256B";
        bench_update(bench, label.c_str(), make, kHotValueBytes);
      });
    }

    // ---- MySQL Row Simulation: 1KB Value -------------------------------------
    std::cout << "\n=== MySQL Row Simulation: 1KB Value ===\n";
    {
      nb::Bench bench;
      bench.title("ST / INSERT (1KB)").unit("op").warmup(1).relative(true);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Insert/1KB";
        bench_insert(bench, label.c_str(), make, kWarmValueBytes);
      });
    }
    {
      nb::Bench bench;
      bench.title("ST / GET (1KB)").unit("op").warmup(1).relative(true);
      for_each_store_variant([&](const char *name, auto make) {
        std::string hit = std::string(name) + "/Get/Hit/1KB";
        std::string miss = std::string(name) + "/Get/Miss/1KB";
        bench_get_hit(bench, hit.c_str(), make, kWarmValueBytes);
        bench_get_miss(bench, miss.c_str(), make, kWarmValueBytes);
      });
    }
    {
      nb::Bench bench;
      bench.title("ST / UPDATE (1KB)").unit("op").warmup(1).relative(true);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Update/1KB";
        bench_update(bench, label.c_str(), make, kWarmValueBytes);
      });
    }

    std::cout << "\n=== Multi-threaded throughput (MySQL Row Simulation, " << std::fixed << std::setprecision(1)
              << mt_duration_seconds << " s per run) ===\n";
    for_each_store_variant([&](const char *name, auto make) {
      std::string get_256 = std::string("MT / GET (256B)    ") + name;
      std::string insert_256 = std::string("MT / INSERT (256B) ") + name;
      std::string mixed_256 = std::string("MT / MIXED (256B)  ") + name + " (80R/20W)";
      bench_mt_get(get_256.c_str(), thread_counts, mt_duration_seconds, make, kHotValueBytes);
      bench_mt_insert(insert_256.c_str(), thread_counts, mt_duration_seconds, make, kHotValueBytes);
      bench_mt_mixed(mixed_256.c_str(), thread_counts, mt_duration_seconds, make, kHotValueBytes);

      std::string get_1k = std::string("MT / GET (1KB)     ") + name;
      std::string insert_1k = std::string("MT / INSERT (1KB)  ") + name;
      std::string mixed_1k = std::string("MT / MIXED (1KB)   ") + name + " (80R/20W)";
      bench_mt_get(get_1k.c_str(), thread_counts, mt_duration_seconds, make, kWarmValueBytes);
      bench_mt_insert(insert_1k.c_str(), thread_counts, mt_duration_seconds, make, kWarmValueBytes);
      bench_mt_mixed(mixed_1k.c_str(), thread_counts, mt_duration_seconds, make, kWarmValueBytes);
    });

    std::cout << '\n';
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "benchmark failed: " << ex.what() << '\n';
    return 2;
  } catch (...) {
    std::cerr << "benchmark failed: unknown exception\n";
    return 2;
  }
}
