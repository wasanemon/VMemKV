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
#include <mutex>
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
constexpr uint64_t kStoreCapacityBytes = 64ULL << 30;
constexpr uint64_t kBenchmarkSeed = 42;
constexpr int kScanReorgMinEpochIterations = 5;
constexpr int kMtTitleWidth = 44;
constexpr int kMtDividerWidth = 46;
constexpr int kMtValueWidth = 8;
constexpr int kMtElapsedWidth = 9;
constexpr int kMtStopWidth = 8;
constexpr int kHotValueBytes = 256;
constexpr int kWarmValueBytes = 1024;
constexpr double kMinimumElapsedSeconds = 0.0001;
constexpr int kMaxMtInsertKeys = 2'000'000;
constexpr uint64_t kDefaultIterationCount = 10000;
constexpr size_t kDefaultEpochs = 11;
constexpr uint64_t kQuickIterationCount = 2000;
constexpr int kParseBase10 = 10;
}  // namespace

static auto mt_total_ops(uint64_t st_iterations, int thread_count) noexcept -> int64_t {
  return static_cast<int64_t>(st_iterations) * static_cast<int64_t>(thread_count);
}

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
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void bench_insert(nb::Bench &bench, const char *name, MakeStore make, size_t val_size, uint64_t iterations) {
  bench.run(name, [&] {
    auto store = make();
    std::string dummy(val_size, 'a');
    for (uint64_t i = 0; i < iterations; ++i) {
      bool inserted = store->insert(make_key(static_cast<int>(i)), dummy);
      nb::doNotOptimizeAway(inserted);
    }
  });
}

template <typename MakeStore>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void bench_get_hit(nb::Bench &bench, const char *name, MakeStore make, size_t val_size, uint64_t iterations) {
  constexpr int key_count = 5000;
  bench.run(name, [&] {
    auto store = make();
    populate(*store, {key_count, val_size});
    for (uint64_t i = 0; i < iterations; ++i) {
      int key_index = static_cast<int>(i % static_cast<uint64_t>(key_count));
      if (val_size == kInlineValueBytes) {
        uint64_t value = store->get(make_key(key_index));
        nb::doNotOptimizeAway(value);
      } else {
        auto value = store->get_bytes(make_key(key_index));
        nb::doNotOptimizeAway(value);
      }
    }
  });
}

template <typename MakeStore>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void bench_get_miss(nb::Bench &bench, const char *name, MakeStore make, size_t val_size, uint64_t iterations) {
  constexpr int key_count = 5000;
  bench.run(name, [&] {
    auto store = make();
    populate(*store, {key_count, val_size});
    for (uint64_t i = 0; i < iterations; ++i) {
      int key_index = key_count + static_cast<int>(i);
      if (val_size == kInlineValueBytes) {
        uint64_t value = store->get(make_key(key_index));
        nb::doNotOptimizeAway(value);
      } else {
        auto value = store->get_bytes(make_key(key_index));
        nb::doNotOptimizeAway(value);
      }
    }
  });
}

template <typename MakeStore>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void bench_update(nb::Bench &bench, const char *name, MakeStore make, size_t val_size, uint64_t iterations) {
  constexpr int key_count = 5000;
  bench.run(name, [&] {
    auto store = make();
    populate(*store, {key_count, val_size});
    std::string dummy(val_size, 'a');
    for (uint64_t i = 0; i < iterations; ++i) {
      bool updated = store->update(make_key(static_cast<int>(i % static_cast<uint64_t>(key_count))), dummy);
      nb::doNotOptimizeAway(updated);
    }
  });
}

template <typename MakeStore>
static void bench_delete(
    nb::Bench &bench, const char *name, MakeStore make, uint64_t iterations, size_t val_size = kInlineValueBytes) {
  // Delete cannot be benchmarked with an open-ended time loop because once
  // all keys are gone the lambda degenerates into a near no-op branch.  Use
  // one fixed epoch so every measured iteration deletes a live key exactly once.
  bench.run(name, [&] {
    auto store = make();
    populate(*store, {iterations, val_size});
    for (uint64_t i = 0; i < iterations; ++i) {
      bool removed = store->remove(make_key(static_cast<int>(i)));
      nb::doNotOptimizeAway(removed);
    }
  });
}

template <typename MakeStore>
static void bench_scan(nb::Bench &bench, const char *name, int scan_count, uint64_t iterations, MakeStore make) {
  bench.batch(static_cast<double>(scan_count) * static_cast<double>(iterations)).run(name, [&] {
    auto store = make();
    populate(*store, {static_cast<size_t>(scan_count)});
    for (uint64_t i = 0; i < iterations; ++i) {
      size_t result_count =
          store->scan(make_key(0), make_key(scan_count - 1), [](std::span<const std::byte>, uint64_t value) {
            nb::doNotOptimizeAway(value);
          });
      nb::doNotOptimizeAway(result_count);
    }
  });
}

template <typename MakeStore>
static void bench_scan_reorg(nb::Bench &bench, const char *name, int scan_count, uint64_t iterations, MakeStore make) {
  bench.batch(static_cast<double>(scan_count) * static_cast<double>(iterations))
      .minEpochIterations(kScanReorgMinEpochIterations)
      .run(name, [&] {
        auto store = make();
        populate(*store, {static_cast<size_t>(scan_count)});
        store->reorganize();  // no-op for RocksDB; merges ap->ro for T1Only
        for (uint64_t i = 0; i < iterations; ++i) {
          size_t result_count =
              store->scan(make_key(0), make_key(scan_count - 1), [](std::span<const std::byte>, uint64_t value) {
                nb::doNotOptimizeAway(value);
              });
          nb::doNotOptimizeAway(result_count);
        }
      });
}

// =============================================================================
// Data-scale benchmarks (Zipf a=1.0 vs. uniform access)
// =============================================================================

template <typename MakeStore>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void bench_get_vs_size(nb::Bench &bench, const char *tag, int item_count, uint64_t iterations, MakeStore make) {
  bench.run(std::string(tag) + "/Zipf", [&] {
    auto store = make();
    populate(*store, {static_cast<size_t>(item_count)});
    std::mt19937_64 rng(kBenchmarkSeed);
    ZipfDistribution zipf({item_count, 1.0});
    for (uint64_t i = 0; i < iterations; ++i) {
      uint64_t value = store->get(make_key(zipf(rng)));
      nb::doNotOptimizeAway(value);
    }
  });
  bench.run(std::string(tag) + "/Uniform", [&] {
    auto store = make();
    populate(*store, {static_cast<size_t>(item_count)});
    std::mt19937_64 rng(kBenchmarkSeed);
    std::uniform_int_distribution<int> uniform_index(0, item_count - 1);
    for (uint64_t i = 0; i < iterations; ++i) {
      uint64_t value = store->get(make_key(uniform_index(rng)));
      nb::doNotOptimizeAway(value);
    }
  });
}

template <typename MakeStore>
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static void bench_negative_get_vs_size_reorg(nb::Bench &bench,
                                             const char *tag,
                                             uint64_t iterations,  // NOLINT(bugprone-easily-swappable-parameters)
                                             int item_count,       // NOLINT(bugprone-easily-swappable-parameters)
                                             MakeStore make) {
  bench.run(std::string(tag) + "/Zipf", [&] {
    auto store = make();
    populate(*store, {static_cast<size_t>(item_count)});
    store->reorganize();
    std::mt19937_64 rng(kBenchmarkSeed);
    ZipfDistribution zipf({item_count, 1.0});
    for (uint64_t i = 0; i < iterations; ++i) {
      uint64_t value = store->get(make_key(item_count + zipf(rng)));
      nb::doNotOptimizeAway(value);
    }
  });
  bench.run(std::string(tag) + "/Uniform", [&] {
    auto store = make();
    populate(*store, {static_cast<size_t>(item_count)});
    store->reorganize();
    std::mt19937_64 rng(kBenchmarkSeed);
    std::uniform_int_distribution<int> uniform_index(0, item_count - 1);
    for (uint64_t i = 0; i < iterations; ++i) {
      uint64_t value = store->get(make_key(item_count + uniform_index(rng)));
      nb::doNotOptimizeAway(value);
    }
  });
}

template <typename MakeStore>
static void bench_scan_vs_size(
    nb::Bench &bench, const char *tag, int total_item_count, int scan_count, uint64_t iterations, MakeStore make) {
  bench.batch(static_cast<double>(scan_count) * static_cast<double>(iterations)).run(std::string(tag) + "/Zipf", [&] {
    auto store = make();
    populate(*store, {static_cast<size_t>(total_item_count)});
    std::mt19937_64 rng(kBenchmarkSeed);
    ZipfDistribution zipf({total_item_count - scan_count, 1.0});
    for (uint64_t i = 0; i < iterations; ++i) {
      int scan_start = zipf(rng);
      size_t result_count = store->scan(
          make_key(scan_start), make_key(scan_start + scan_count - 1), [](std::span<const std::byte>, uint64_t value) {
            nb::doNotOptimizeAway(value);
          });
      nb::doNotOptimizeAway(result_count);
    }
  });
  bench.batch(static_cast<double>(scan_count) * static_cast<double>(iterations))
      .run(std::string(tag) + "/Uniform", [&] {
        auto store = make();
        populate(*store, {static_cast<size_t>(total_item_count)});
        std::mt19937_64 rng(kBenchmarkSeed);
        std::uniform_int_distribution<int> uniform_index(0, total_item_count - scan_count - 1);
        for (uint64_t i = 0; i < iterations; ++i) {
          int scan_start = uniform_index(rng);
          size_t result_count =
              store->scan(make_key(scan_start),
                          make_key(scan_start + scan_count - 1),
                          [](std::span<const std::byte>, uint64_t value) { nb::doNotOptimizeAway(value); });
          nb::doNotOptimizeAway(result_count);
        }
      });
}

// =============================================================================
// Multi-threaded throughput benchmarks
// =============================================================================

static void print_mt_header(const char *title) {
  std::cout << "\n| " << std::left << std::setw(kMtTitleWidth) << title
            << " | threads | M ops/s | elapsed s | stop     |\n";
  std::cout << "|" << std::string(kMtDividerWidth, '-') << "|---------|----------|-----------|----------|\n";
  std::cout << std::right;
}

struct MtRunConfig {
  int thread_count;
  int64_t total_ops;
};

template <typename WorkFn>
// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void run_mt(const char *label, MtRunConfig cfg, WorkFn work) {
  std::atomic<bool> stop{false};
  std::atomic<bool> stop_by_work_limit{false};
  std::atomic<bool> stop_by_capacity{false};
  std::atomic<bool> stop_by_error{false};
  std::atomic<int64_t> total{0};
  std::atomic<int64_t> next_operation{0};
  std::mutex error_mutex;
  std::string error_message;
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(cfg.thread_count));
  auto start_time = Clock::now();
  for (int thread_index = 0; thread_index < cfg.thread_count; ++thread_index) {
    threads.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        int64_t operation_index = next_operation.fetch_add(1, std::memory_order_relaxed);
        if (operation_index >= cfg.total_ops) {
          break;
        }
        try {
          if (!work(operation_index)) {
            stop_by_work_limit.store(true, std::memory_order_relaxed);
            stop.store(true, std::memory_order_relaxed);
            break;
          }
          total.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception &ex) {
          {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (error_message.empty()) {
              error_message = ex.what();
            }
          }
          if (std::string_view(ex.what()) == "T2 storage capacity exceeded") {
            stop_by_capacity.store(true, std::memory_order_relaxed);
          } else {
            stop_by_error.store(true, std::memory_order_relaxed);
          }
          stop.store(true, std::memory_order_relaxed);
          break;
        } catch (...) {
          {
            std::lock_guard<std::mutex> lock(error_mutex);
            if (error_message.empty()) {
              error_message = "unknown exception";
            }
          }
          stop_by_error.store(true, std::memory_order_relaxed);
          stop.store(true, std::memory_order_relaxed);
          break;
        }
      }
    });
  }

  for (auto &worker_thread : threads) {
    worker_thread.join();
  }
  double elapsed = std::chrono::duration<double>(Clock::now() - start_time).count();
  if (elapsed <= 0) {
    elapsed = kMinimumElapsedSeconds;
  }
  const double throughput_mops = static_cast<double>(total.load()) / elapsed / 1e6;
  const char *stop_reason = "count";
  if (stop_by_capacity.load(std::memory_order_relaxed)) {
    stop_reason = "capacity";
  } else if (stop_by_work_limit.load(std::memory_order_relaxed)) {
    stop_reason = "limit";
  } else if (stop_by_error.load(std::memory_order_relaxed)) {
    stop_reason = "error";
  }
  std::cout << "| " << std::left << std::setw(kMtTitleWidth) << label << " |   " << std::right << std::setw(3)
            << cfg.thread_count << "   | " << std::fixed << std::setw(kMtValueWidth) << std::setprecision(2)
            << throughput_mops << " | " << std::setw(kMtElapsedWidth) << std::setprecision(2) << elapsed << " | "
            << std::setw(kMtStopWidth) << stop_reason << " |\n";
  if (stop_by_error.load(std::memory_order_relaxed)) {
    std::lock_guard<std::mutex> lock(error_mutex);
    std::cout << "  note: " << label << " stopped by exception: " << error_message << "\n";
  }
}

template <typename MakeStore>
static void bench_mt_get(const char *name,
                         const std::vector<int> &thread_counts,
                         uint64_t st_iterations,
                         MakeStore make,
                         size_t val_size = kInlineValueBytes) {
  constexpr int key_count = 10000;
  print_mt_header(name);
  for (int thread_count : thread_counts) {
    auto store = make();
    populate(*store, {key_count, val_size});
    run_mt(name,
           MtRunConfig{thread_count, mt_total_ops(st_iterations, thread_count)},
           [&](int64_t operation_count) -> bool {
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
                            uint64_t st_iterations,
                            MakeStore make,
                            size_t val_size = kInlineValueBytes) {
  print_mt_header(name);
  for (int thread_count : thread_counts) {
    auto store = make();
    std::string dummy(val_size, 'a');
    run_mt(name,
           MtRunConfig{thread_count, mt_total_ops(st_iterations, thread_count)},
           [&](int64_t operation_count) -> bool {
             int key_index = static_cast<int>(operation_count);
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
                           uint64_t st_iterations,
                           MakeStore make,
                           size_t val_size = kInlineValueBytes) {
  constexpr int key_count = 10000;
  print_mt_header(name);
  for (int thread_count : thread_counts) {
    auto store = make();
    populate(*store, {key_count, val_size});
    std::string dummy(val_size, 'a');
    run_mt(name,
           MtRunConfig{thread_count, mt_total_ops(st_iterations, thread_count)},
           [&](int64_t operation_number) -> bool {
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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto main(int argc, char **argv) -> int {
  try {
    uint64_t st_iterations = kDefaultIterationCount;
    bool quick_mode = false;
    for (int i = 1; i < argc; ++i) {
      std::string_view arg(argv[i]);
      if (arg == "--iters" && i + 1 < argc) {
        st_iterations = static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, kParseBase10));
        continue;
      }
      if (arg == "--quick") {
        quick_mode = true;
        continue;
      }
      if (arg == "-h" || arg == "--help") {
        std::cout << "Usage: bench_kv [--iters <count>] [--quick]\n";
        return 0;
      }
      std::cerr << "Unknown option: " << argv[i] << '\n';
      return 1;
    }

    const int hardware_threads = static_cast<int>(std::thread::hardware_concurrency());
    const std::vector<int> thread_counts = {1, 4, hardware_threads};
    const size_t epochs = kDefaultEpochs;

    if (quick_mode) {
      st_iterations = kQuickIterationCount;
    }

    if (quick_mode) {
      std::cout << "=== QUICK mode: 1 ST + 1 MT ===\n";
      {
        nb::Bench bench;
        bench.title("ST / QUICK / INSERT")
            .unit("op")
            .warmup(0)
            .epochs(epochs)
            .epochIterations(1)
            .batch(static_cast<double>(st_iterations))
            .relative(true)
            .minEpochIterations(1);
        bool done = false;
        for_each_store_variant([&](const char *name, auto make) {
          if (done) {
            return;
          }
          std::string label = std::string(name) + "/QuickInsert";
          bench_insert(bench, label.c_str(), make, kInlineValueBytes, st_iterations);
          done = true;
        });
      }

      bool done = false;
      for_each_store_variant([&](const char *name, auto make) {
        if (done) {
          return;
        }
        std::string label = std::string("MT / QUICK / GET ") + name;
        bench_mt_get(label.c_str(), std::vector<int>{1}, st_iterations, make);
        done = true;
      });
      std::cout << '\n';
      return 0;
    }

    // ---- ST / INSERT ---------------------------------------------------------
    {
      nb::Bench bench;
      bench.title("ST / INSERT")
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .batch(static_cast<double>(st_iterations))
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Insert";
        bench_insert(bench, label.c_str(), make, kInlineValueBytes, st_iterations);
      });
    }

    // ---- ST / GET ------------------------------------------------------------
    {
      nb::Bench bench;
      bench.title("ST / GET")
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .batch(static_cast<double>(st_iterations))
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant([&](const char *name, auto make) {
        std::string hit = std::string(name) + "/Get/Hit";
        std::string miss = std::string(name) + "/Get/Miss";
        bench_get_hit(bench, hit.c_str(), make, kInlineValueBytes, st_iterations);
        bench_get_miss(bench, miss.c_str(), make, kInlineValueBytes, st_iterations);
      });
    }

    // ---- ST / UPDATE ---------------------------------------------------------
    {
      nb::Bench bench;
      bench.title("ST / UPDATE")
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .batch(static_cast<double>(st_iterations))
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Update";
        bench_update(bench, label.c_str(), make, kInlineValueBytes, st_iterations);
      });
    }

    // ---- ST / DELETE ---------------------------------------------------------
    {
      nb::Bench bench;
      bench.title("ST / DELETE")
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .batch(static_cast<double>(st_iterations))
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Delete";
        bench_delete(bench, label.c_str(), make, st_iterations);
      });
    }

    // ---- ST / SCAN -----------------------------------------------------------
    // Scan vs Scan(reorg) to reveal ap_region overhead.
    // RocksDB/Scan is always sorted (no reorg needed).
    for (int dataset_size : {1000, 5000, 10000}) {
      nb::Bench bench;
      bench.title("ST / SCAN/" + std::to_string(dataset_size))
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant([&](const char *name, auto make) {
        std::string scan = std::string(name) + "/Scan";
        bench_scan(bench, scan.c_str(), dataset_size, st_iterations, make);

        if (std::string(name) != "RocksDB") {
          std::string scan_reorg = std::string(name) + "/Scan(reorg)";
          bench_scan_reorg(bench, scan_reorg.c_str(), dataset_size, st_iterations, make);
        }
      });
    }

    // ---- MT ------------------------------------------------------------------
    std::cout << "\n\n=== Multi-threaded throughput (base iters=" << st_iterations
              << ", total ops = base iters * thread_count, hw_concurrency=" << hardware_threads << ") ===\n";

    for_each_store_variant([&](const char *name, auto make) {
      std::string get = std::string("MT / GET    ") + name;
      std::string insert = std::string("MT / INSERT ") + name;
      std::string mixed = std::string("MT / MIXED  ") + name + " (80R/20W)";
      bench_mt_get(get.c_str(), thread_counts, st_iterations, make);
      bench_mt_insert(insert.c_str(), thread_counts, st_iterations, make);
      bench_mt_mixed(mixed.c_str(), thread_counts, st_iterations, make);
    });

    // ---- Data-scale: GET latency vs. dataset size ----------------------------
    std::cout << "\n\n=== Data-scale: GET latency vs. dataset size ===\n";
    for (int dataset_size : {1000, 5000, 10000, 20000}) {
      nb::Bench bench;
      bench.title("SCALE / GET N=" + std::to_string(dataset_size))
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .batch(static_cast<double>(st_iterations))
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant(
          [&](const char *name, auto make) { bench_get_vs_size(bench, name, dataset_size, st_iterations, make); });
    }

    // ---- Data-scale: NEGATIVE GET latency vs. dataset size -------------------
    // Reorganize first so misses are dominated by sorted-region lookup.
    // This makes Bloom filter effects visible instead of append-region scan cost.
    std::cout << "\n\n=== Data-scale: NEGATIVE GET latency vs. dataset size (after reorganize) ===\n";
    for (int dataset_size : {1000, 5000, 10000, 20000}) {
      nb::Bench bench;
      bench.title("SCALE / NEGATIVE_GET(reorg) N=" + std::to_string(dataset_size))
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .batch(static_cast<double>(st_iterations))
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant([&](const char *name, auto make) {
        bench_negative_get_vs_size_reorg(bench, name, st_iterations, dataset_size, make);
      });
    }

    // ---- Data-scale: SCAN latency vs. dataset size ---------------------------
    std::cout << "\n\n=== Data-scale: SCAN latency vs. dataset size (window=1000) ===\n";
    for (int dataset_size : {5000, 10000, 20000}) {
      constexpr int scan_window = 1000;
      nb::Bench bench;
      bench.title("SCALE / SCAN N=" + std::to_string(dataset_size) + " win=" + std::to_string(scan_window))
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant([&](const char *name, auto make) {
        bench_scan_vs_size(bench, name, dataset_size, scan_window, st_iterations, make);
      });
    }

    // ---- MySQL Row Simulation: 256B Value ------------------------------------
    std::cout << "\n=== MySQL Row Simulation: 256B Value ===\n";
    {
      nb::Bench bench;
      bench.title("ST / INSERT (256B)")
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .batch(static_cast<double>(st_iterations))
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Insert/256B";
        bench_insert(bench, label.c_str(), make, kHotValueBytes, st_iterations);
      });
    }
    {
      nb::Bench bench;
      bench.title("ST / GET (256B)")
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .batch(static_cast<double>(st_iterations))
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant([&](const char *name, auto make) {
        std::string hit = std::string(name) + "/Get/Hit/256B";
        std::string miss = std::string(name) + "/Get/Miss/256B";
        bench_get_hit(bench, hit.c_str(), make, kHotValueBytes, st_iterations);
        bench_get_miss(bench, miss.c_str(), make, kHotValueBytes, st_iterations);
      });
    }
    {
      nb::Bench bench;
      bench.title("ST / UPDATE (256B)")
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .batch(static_cast<double>(st_iterations))
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Update/256B";
        bench_update(bench, label.c_str(), make, kHotValueBytes, st_iterations);
      });
    }

    // ---- MySQL Row Simulation: 1KB Value -------------------------------------
    std::cout << "\n=== MySQL Row Simulation: 1KB Value ===\n";
    {
      nb::Bench bench;
      bench.title("ST / INSERT (1KB)")
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .batch(static_cast<double>(st_iterations))
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Insert/1KB";
        bench_insert(bench, label.c_str(), make, kWarmValueBytes, st_iterations);
      });
    }
    {
      nb::Bench bench;
      bench.title("ST / GET (1KB)")
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .batch(static_cast<double>(st_iterations))
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant([&](const char *name, auto make) {
        std::string hit = std::string(name) + "/Get/Hit/1KB";
        std::string miss = std::string(name) + "/Get/Miss/1KB";
        bench_get_hit(bench, hit.c_str(), make, kWarmValueBytes, st_iterations);
        bench_get_miss(bench, miss.c_str(), make, kWarmValueBytes, st_iterations);
      });
    }
    {
      nb::Bench bench;
      bench.title("ST / UPDATE (1KB)")
          .unit("op")
          .warmup(0)
          .epochs(epochs)
          .epochIterations(1)
          .batch(static_cast<double>(st_iterations))
          .relative(true)
          .minEpochIterations(1);
      for_each_store_variant([&](const char *name, auto make) {
        std::string label = std::string(name) + "/Update/1KB";
        bench_update(bench, label.c_str(), make, kWarmValueBytes, st_iterations);
      });
    }

    std::cout << "\n=== Multi-threaded throughput (MySQL Row Simulation, base iters=" << st_iterations << ") ===\n";
    for_each_store_variant([&](const char *name, auto make) {
      std::string get_256 = std::string("MT / GET (256B)    ") + name;
      std::string insert_256 = std::string("MT / INSERT (256B) ") + name;
      std::string mixed_256 = std::string("MT / MIXED (256B)  ") + name + " (80R/20W)";
      bench_mt_get(get_256.c_str(), thread_counts, st_iterations, make, kHotValueBytes);
      bench_mt_insert(insert_256.c_str(), thread_counts, st_iterations, make, kHotValueBytes);
      bench_mt_mixed(mixed_256.c_str(), thread_counts, st_iterations, make, kHotValueBytes);

      std::string get_1k = std::string("MT / GET (1KB)     ") + name;
      std::string insert_1k = std::string("MT / INSERT (1KB)  ") + name;
      std::string mixed_1k = std::string("MT / MIXED (1KB)   ") + name + " (80R/20W)";
      bench_mt_get(get_1k.c_str(), thread_counts, st_iterations, make, kWarmValueBytes);
      bench_mt_insert(insert_1k.c_str(), thread_counts, st_iterations, make, kWarmValueBytes);
      bench_mt_mixed(mixed_1k.c_str(), thread_counts, st_iterations, make, kWarmValueBytes);
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
