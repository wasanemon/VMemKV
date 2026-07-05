// NOLINTBEGIN
// bench_kv.cpp -- VMemKV microbenchmarks using Google Benchmark
//
// All benchmark functions are dynamic registrations over store variants.
// Runs are executed across a dynamic range of threads to measure scalability.
// Real-time (wall-clock) measurement is used for accurate throughput.

#include <benchmark/benchmark.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#include <vmemkv/vmemkv.hpp>

#ifdef ENABLE_ROCKSDB
#include <rivals/rocksdb_store.hpp>
#endif

namespace {
constexpr std::size_t kIndexKeyBufferBytes = 16;
constexpr std::size_t kInlineValueBytes = 8;
constexpr double kZipfPivot = 1.5;
constexpr double kHalfStep = 0.5;
constexpr uint64_t kStoreCapacityBytes = 64ULL << 30;
constexpr uint64_t kBenchmarkSeed = 42;
constexpr int kWarmValueBytes = 1024;
constexpr int kMaxMtInsertKeys = 2'000'000;
}  // namespace

// ---- Key helpers -------------------------------------------------------------
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
  const size_t num_threads = std::max(1U, std::thread::hardware_concurrency());
  std::vector<std::thread> workers;
  size_t chunk_size = (options.key_count + num_threads - 1) / num_threads;

  for (size_t t = 0; t < num_threads; ++t) {
    workers.emplace_back([&store, dummy, t, chunk_size, options]() {
      size_t start = t * chunk_size;
      size_t end = std::min(options.key_count, start + chunk_size);
      for (size_t i = start; i < end; ++i) {
        store.insert(make_key(static_cast<int>(i)), dummy);
      }
    });
  }

  for (auto &w : workers) {
    w.join();
  }
}

// ---- Zipf distribution -------------------------------------------------------
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
      if (rank < 1) rank = 1;
      if (rank > item_count_) rank = item_count_;
      if (rank - sample_position <= s_ ||
          uniform_sample >= h_integral(static_cast<double>(rank) + kHalfStep) - h(rank)) {
        return rank - 1;
      }
    }
  }

 private:
  int item_count_;
  double alpha_, h_integral_x1_, h_integral_inf_, s_;
  [[nodiscard]] auto h(double val) const -> double { return std::exp(-alpha_ * std::log(val)); }
  [[nodiscard]] auto h_integral(double val) const -> double {
    return (alpha_ == 1.0) ? std::log(val) : std::exp((1.0 - alpha_) * std::log(val)) / (1.0 - alpha_);
  }
  [[nodiscard]] auto h_integral_inv(double val) const -> double {
    return (alpha_ == 1.0) ? std::exp(val) : std::exp(std::log(val * (1.0 - alpha_)) / (1.0 - alpha_));
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

using BenchmarkTypes = std::tuple<vmemkv::variants::VMemKV_Baseline,
                                  vmemkv::variants::VMemKV_Cumulative_Step1,
                                  vmemkv::variants::VMemKV_Cumulative_Step2,
                                  vmemkv::variants::VMemKV_Cumulative_Step3,
                                  vmemkv::variants::VMemKV_Cumulative_Step4,
                                  vmemkv::variants::VMemKV_Cumulative_Step5,
                                  vmemkv::variants::VMemKV_RocksDB>;

template <typename Visitor>
static void for_each_store_variant(Visitor &&visitor) {
  for_each_in_tuple<BenchmarkTypes>([&](auto *dummy) {
    using Store = std::remove_pointer_t<decltype(dummy)>;
    if constexpr (Store::kIsEnabled) {
      std::string filename = "bench_" + Store::name() + ".bin";
      std::replace(filename.begin(), filename.end(), '/', '_');

      visitor(Store::name().c_str(), [filename]() {
        return make_vmemkv(filename, [filename]() {
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
// Helper for managing store lifecycle across threads in Google Benchmark
// =============================================================================
template <typename StorePtr>
struct StoreHolder {
  std::mutex mutex;
  StorePtr store;
  int active_threads = 0;
};

template <typename MakeStore, typename InitFn, typename RunFn>
static void register_bench(const std::string &name,
                           MakeStore make,
                           InitFn &&init_fn,
                           RunFn &&run_fn,
                           const std::vector<int> &thread_counts,
                           bool reuse_store = false,
                           int fixed_iterations = -1) {
  auto holder = std::make_shared<StoreHolder<decltype(make())>>();

  auto bench_func =
      [holder, make, init_fn = std::forward<InitFn>(init_fn), run_fn = std::forward<RunFn>(run_fn), reuse_store](
          benchmark::State &state) {
        {
          std::lock_guard<std::mutex> lock(holder->mutex);
          if (!holder->store) {
            holder->store = make();
            init_fn(*(holder->store));
          }
          holder->active_threads++;
        }

        run_fn(state, *(holder->store));

        {
          std::lock_guard<std::mutex> lock(holder->mutex);
          holder->active_threads--;
          if (!reuse_store && holder->active_threads == 0) {
            holder->store.reset();
          }
        }
      };

  for (int threads : thread_counts) {
    auto *reg = benchmark::RegisterBenchmark(name.c_str(), bench_func)->Threads(threads)->UseRealTime();
    if (fixed_iterations > 0) {
      reg->Iterations(fixed_iterations / threads);
    }
  }
}

// =============================================================================
// Benchmark definitions
// =============================================================================
void register_all_benchmarks() {
  const int hw_threads = static_cast<int>(std::thread::hardware_concurrency());

  // Uniform 4-point thread counts for plotting scalability: {1, 4, 12, hw_threads}
  std::vector<int> thread_counts = {1, 4};
  if (12 < hw_threads) {
    thread_counts.push_back(12);
  }
  if (hw_threads > thread_counts.back()) {
    thread_counts.push_back(hw_threads);
  }
  std::sort(thread_counts.begin(), thread_counts.end());
  thread_counts.erase(std::unique(thread_counts.begin(), thread_counts.end()), thread_counts.end());

  for_each_store_variant([&](const char *name, auto make) {
    std::string sname(name);

    // Loop through value sizes for main CRUD benchmarks (8B and 1KB)
    for (size_t val_size : {kInlineValueBytes, size_t{kWarmValueBytes}}) {
      std::string suffix = (val_size == kInlineValueBytes) ? "" : "/1KB";

      // 1. Insert (Stateful/destructive: reuse_store = false)
      register_bench(
          sname + "/Insert" + suffix,
          make,
          [](auto &) {},
          [val_size](benchmark::State &state, auto &store) {
            std::string dummy(val_size, 'a');
            int threads = state.threads();
            int thread_idx = state.thread_index();
            int i = 0;
            for (auto _ : state) {
              int key_index = thread_idx + i * threads;
              if (key_index >= kMaxMtInsertKeys) {
                state.SkipWithError("Max MT Insert Keys exceeded");
                break;
              }
              bool inserted = store.insert(make_key(key_index), dummy);
              benchmark::DoNotOptimize(inserted);
              i++;
            }
            state.SetItemsProcessed(state.iterations());
          },
          thread_counts,
          false);

      // 2. Get/Hit (Read-only: reuse_store = true)
      register_bench(
          sname + "/Get/Hit" + suffix,
          make,
          [val_size](auto &store) { populate(store, {10000, val_size}); },
          [](benchmark::State &state, auto &store) {
            constexpr int key_count = 10000;
            std::mt19937_64 rng(kBenchmarkSeed + state.thread_index());
            for (auto _ : state) {
              int key_index = static_cast<int>(rng() % key_count);
              auto value = store.get_bytes(make_key(key_index));
              benchmark::DoNotOptimize(value);
            }
            state.SetItemsProcessed(state.iterations());
          },
          thread_counts,
          true);

      // 3. Get/Miss (Read-only: reuse_store = true)
      register_bench(
          sname + "/Get/Miss" + suffix,
          make,
          [val_size](auto &store) { populate(store, {10000, val_size}); },
          [](benchmark::State &state, auto &store) {
            constexpr int key_count = 10000;
            std::mt19937_64 rng(kBenchmarkSeed + state.thread_index());
            for (auto _ : state) {
              int key_index = key_count + static_cast<int>(rng() % key_count);
              auto value = store.get_bytes(make_key(key_index));
              benchmark::DoNotOptimize(value);
            }
            state.SetItemsProcessed(state.iterations());
          },
          thread_counts,
          true);

      // 4. Update (Stateful: reuse_store = false)
      register_bench(
          sname + "/Update" + suffix,
          make,
          [val_size](auto &store) { populate(store, {10000, val_size}); },
          [val_size](benchmark::State &state, auto &store) {
            constexpr int key_count = 10000;
            std::string dummy(val_size, 'a');
            std::mt19937_64 rng(kBenchmarkSeed + state.thread_index());
            for (auto _ : state) {
              int key_index = static_cast<int>(rng() % key_count);
              bool updated = store.update(make_key(key_index), dummy);
              benchmark::DoNotOptimize(updated);
            }
            state.SetItemsProcessed(state.iterations());
          },
          thread_counts,
          false);
    }

    // 5. Delete (Stateful / Thread-Isolated Ranges: reuse_store = false)
    constexpr int kDeletePopulateCount = 100000;
    register_bench(
        sname + "/Delete",
        make,
        [](auto &store) { populate(store, {kDeletePopulateCount, kInlineValueBytes}); },
        [](benchmark::State &state, auto &store) {
          int threads = state.threads();
          int thread_idx = state.thread_index();
          int range_size = kDeletePopulateCount / threads;
          int my_start = thread_idx * range_size;

          int i = 0;
          for (auto _ : state) {
            int key_index = my_start + i;
            bool removed = store.remove(make_key(key_index));
            benchmark::DoNotOptimize(removed);
            i++;
          }
          state.SetItemsProcessed(state.iterations());
        },
        thread_counts,
        false,
        kDeletePopulateCount);

    // 6. Scan & ScanReorg (dataset_size 4-point sweep: {10K, 50K, 100K, 500K}, Read-only: reuse_store = true)
    constexpr int scan_count = 100;
    for (size_t dataset_size : {size_t{10000}, size_t{50000}, size_t{100000}, size_t{500000}}) {
      register_bench(
          sname + "/Scan/N_" + std::to_string(dataset_size),
          make,
          [dataset_size](auto &store) { populate(store, {dataset_size, kInlineValueBytes}); },
          [](benchmark::State &state, auto &store) {
            for (auto _ : state) {
              size_t result_count =
                  store.scan(make_key(0), make_key(scan_count - 1), [](std::span<const std::byte>, uint64_t value) {
                    benchmark::DoNotOptimize(value);
                  });
              benchmark::DoNotOptimize(result_count);
            }
            state.SetItemsProcessed(state.iterations() * scan_count);
          },
          thread_counts,
          true);

      if (sname != "RocksDB") {
        register_bench(
            sname + "/ScanReorg/N_" + std::to_string(dataset_size),
            make,
            [dataset_size](auto &store) {
              populate(store, {dataset_size, kInlineValueBytes});
              store.reorganize();
            },
            [](benchmark::State &state, auto &store) {
              for (auto _ : state) {
                size_t result_count =
                    store.scan(make_key(0), make_key(scan_count - 1), [](std::span<const std::byte>, uint64_t value) {
                      benchmark::DoNotOptimize(value);
                    });
                benchmark::DoNotOptimize(result_count);
              }
              state.SetItemsProcessed(state.iterations() * scan_count);
            },
            thread_counts,
            true);
      }
    }

    // 7. SCALE Benchmarks (GET, SCAN vs. dataset size, Read-only: reuse_store = true)
    for (int dataset_size : {1000, 5000, 10000, 20000}) {
      for (const char *dist : {"Zipf", "Uniform"}) {
        register_bench(
            sname + "/SCALE/Get/" + dist + "/N_" + std::to_string(dataset_size),
            make,
            [dataset_size](auto &store) { populate(store, {static_cast<size_t>(dataset_size), kInlineValueBytes}); },
            [dataset_size, dist](benchmark::State &state, auto &store) {
              std::mt19937_64 rng(kBenchmarkSeed + state.thread_index());
              ZipfDistribution zipf({dataset_size, 1.0});
              std::uniform_int_distribution<int> uniform_index(0, dataset_size - 1);
              for (auto _ : state) {
                int key = (std::string(dist) == "Zipf") ? zipf(rng) : uniform_index(rng);
                uint64_t value = store.get(make_key(key));
                benchmark::DoNotOptimize(value);
              }
              state.SetItemsProcessed(state.iterations());
            },
            thread_counts,
            true);
      }
    }

    // SCALE Scan (Read-only: reuse_store = true, scan_window = 100)
    constexpr int scale_scan_window = 100;
    for (int dataset_size : {5000, 10000, 20000}) {
      for (const char *dist : {"Zipf", "Uniform"}) {
        register_bench(
            sname + "/SCALE/Scan/" + dist + "/N_" + std::to_string(dataset_size),
            make,
            [dataset_size](auto &store) { populate(store, {static_cast<size_t>(dataset_size), kInlineValueBytes}); },
            [dataset_size, dist](benchmark::State &state, auto &store) {
              std::mt19937_64 rng(kBenchmarkSeed + state.thread_index());
              ZipfDistribution zipf({dataset_size - scale_scan_window, 1.0});
              std::uniform_int_distribution<int> uniform_index(0, dataset_size - scale_scan_window - 1);
              for (auto _ : state) {
                int scan_start = (std::string(dist) == "Zipf") ? zipf(rng) : uniform_index(rng);
                size_t result_count =
                    store.scan(make_key(scan_start),
                               make_key(scan_start + scale_scan_window - 1),
                               [](std::span<const std::byte>, uint64_t value) { benchmark::DoNotOptimize(value); });
                benchmark::DoNotOptimize(result_count);
              }
              state.SetItemsProcessed(state.iterations() * scale_scan_window);
            },
            thread_counts,
            true);
      }
    }
  });
}

int main(int argc, char **argv) {
  benchmark::Initialize(&argc, argv);
  register_all_benchmarks();
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
// NOLINTEND
