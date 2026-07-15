// NOLINTBEGIN
// bench_kv.cpp -- VMemKV microbenchmarks using Google Benchmark
//
// All benchmark functions are dynamic registrations over store variants.
// Runs are executed across a dynamic range of threads to measure scalability.
// Real-time (wall-clock) measurement is used for accurate throughput.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>
#include <vmemkv/vmemkv.hpp>

#ifdef ENABLE_ROCKSDB
#include <rivals/rocksdb_store.hpp>
#endif

namespace {
class YCSBTimelineCollector {
 public:
  static constexpr int kDurationSeconds = 30;

  struct ThreadCounter {
    alignas(64) std::array<std::atomic<uint64_t>, kDurationSeconds> scan_counts{};
    alignas(64) std::array<std::atomic<uint64_t>, kDurationSeconds> insert_counts{};
  };

  std::vector<ThreadCounter> counters;
  std::array<std::atomic<uint64_t>, kDurationSeconds> t1_reorg_counts{};
  std::array<std::atomic<uint64_t>, kDurationSeconds> t2_reorg_counts{};
  std::atomic<uint64_t> last_recorded_t1{0};
  std::atomic<uint64_t> last_recorded_t2{0};
  std::atomic<uint64_t> next_key_index;
  std::chrono::steady_clock::time_point start_time;

  YCSBTimelineCollector(size_t num_threads, uint64_t initial_keys)
      : counters(num_threads), next_key_index(initial_keys) {
    for (int i = 0; i < kDurationSeconds; ++i) {
      t1_reorg_counts[i].store(0, std::memory_order_relaxed);
      t2_reorg_counts[i].store(0, std::memory_order_relaxed);
    }
  }

  void start() { start_time = std::chrono::steady_clock::now(); }

  void record_op(size_t thread_idx, bool is_scan) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
    if (elapsed >= 0 && elapsed < kDurationSeconds) {
      if (is_scan) {
        counters[thread_idx].scan_counts[elapsed].fetch_add(1, std::memory_order_relaxed);
      } else {
        counters[thread_idx].insert_counts[elapsed].fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  void dump_json(std::string store_name, std::string variant_name, std::string val_name) {
    std::vector<uint64_t> total_scan(kDurationSeconds, 0);
    std::vector<uint64_t> total_insert(kDurationSeconds, 0);
    std::vector<uint64_t> total_reorg_t1(kDurationSeconds, 0);
    std::vector<uint64_t> total_reorg_t2(kDurationSeconds, 0);

    for (const auto &tc : counters) {
      for (int i = 0; i < kDurationSeconds; ++i) {
        total_scan[i] += tc.scan_counts[i].load(std::memory_order_relaxed);
        total_insert[i] += tc.insert_counts[i].load(std::memory_order_relaxed);
      }
    }
    for (int i = 0; i < kDurationSeconds; ++i) {
      total_reorg_t1[i] = t1_reorg_counts[i].load(std::memory_order_relaxed);
      total_reorg_t2[i] = t2_reorg_counts[i].load(std::memory_order_relaxed);
    }

    // Sanitize parameters to prevent slash / from breaking folder path
    for (auto *s : {&store_name, &variant_name, &val_name}) {
      std::replace(s->begin(), s->end(), '/', '-');
      std::replace(s->begin(), s->end(), ' ', '_');
      std::replace(s->begin(), s->end(), '(', '_');
      std::replace(s->begin(), s->end(), ')', '_');
      std::replace(s->begin(), s->end(), '%', '_');
    }

    std::string filename = "/tmp/ycsb_e_timeline_" + store_name + "_" + variant_name + "_" + val_name + ".json";
    std::ofstream out(filename);
    if (out.is_open()) {
      out << "{\n";
      out << "  \"store\": \"" << store_name << "\",\n";
      out << "  \"variant\": \"" << variant_name << "\",\n";
      out << "  \"value_size\": \"" << val_name << "\",\n";
      out << "  \"timeline\": [\n";
      for (int i = 0; i < kDurationSeconds; ++i) {
        out << "    {\"sec\": " << (i + 1) << ", \"scan_ops\": " << total_scan[i]
            << ", \"insert_ops\": " << total_insert[i] << ", \"t1_reorg_ops\": " << total_reorg_t1[i]
            << ", \"t2_reorg_ops\": " << total_reorg_t2[i] << "}";
        if (i < kDurationSeconds - 1) out << ",";
        out << "\n";
      }
      out << "  ]\n";
      out << "}\n";
    }
  }
};

constexpr std::size_t kIndexKeyBufferBytes = 32;
constexpr std::size_t kIndexKeyBytes = 16;
constexpr std::size_t kInlineValueBytes = 8;
constexpr std::size_t kInMemoryInlineCorpusEntries = 20'000'000;
constexpr double kZipfPivot = 1.5;
constexpr double kHalfStep = 0.5;
constexpr uint64_t kBenchmarkSeed = 42;
static inline bool is_ltm_mode() {
  const char *env = std::getenv("VMEMKV_BENCH_LTM");
  return env != nullptr && std::string(env) == "1";
}

static inline bool force_host_memory() {
  const char *env = std::getenv("VMEMKV_BENCH_FORCE_HOST_MEMORY");
  return env != nullptr && std::string(env) == "1";
}

static inline std::optional<std::size_t> read_size_from_file(const char *path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    return std::nullopt;
  }

  std::string token;
  input >> token;
  if (!input) {
    return std::nullopt;
  }

  if (token == "max") {
    return std::nullopt;
  }

  try {
    return static_cast<std::size_t>(std::stoull(token));
  } catch (...) {
    return std::nullopt;
  }
}

static inline std::optional<std::size_t> read_size_from_env(const char *name) {
  const char *value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return std::nullopt;
  }

  try {
    return static_cast<std::size_t>(std::stoull(value));
  } catch (...) {
    return std::nullopt;
  }
}

static inline std::size_t detect_machine_memory_bytes() {
  constexpr std::size_t kHugeLimitThreshold = 1ULL << 60;

  if (const auto context_budget = read_size_from_env("VMEMKV_CONTEXT_memory_budget_bytes");
      context_budget.has_value() && *context_budget > 0 && *context_budget < kHugeLimitThreshold) {
    return *context_budget;
  }

  if (force_host_memory()) {
    std::ifstream meminfo("/proc/meminfo");
    std::string key;
    std::size_t value_kb = 0;
    std::string unit;
    while (meminfo >> key >> value_kb >> unit) {
      if (key == "MemTotal:") {
        return value_kb * 1024ULL;
      }
      meminfo.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }
    return 256ULL * 1024ULL * 1024ULL;
  }

  // Prefer cgroup limits when present so containerized runs use their effective
  // memory ceiling instead of the host's MemTotal.
  if (const auto cgroup_v2 = read_size_from_file("/sys/fs/cgroup/memory.max");
      cgroup_v2.has_value() && *cgroup_v2 < kHugeLimitThreshold) {
    return *cgroup_v2;
  }
  if (const auto cgroup_v1 = read_size_from_file("/sys/fs/cgroup/memory/memory.limit_in_bytes");
      cgroup_v1.has_value() && *cgroup_v1 < kHugeLimitThreshold) {
    return *cgroup_v1;
  }

  std::ifstream meminfo("/proc/meminfo");
  std::string key;
  std::size_t value_kb = 0;
  std::string unit;
  while (meminfo >> key >> value_kb >> unit) {
    if (key == "MemTotal:") {
      return value_kb * 1024ULL;
    }
    meminfo.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }

  // Conservative fallback if /proc/meminfo is unavailable.
  return 256ULL * 1024ULL * 1024ULL;
}

static inline std::string get_db_dir() {
  const char *env = std::getenv("VMEMKV_DB_DIR");
  return env != nullptr ? std::string(env) : ".";
}

static inline double get_target_ratio() {
  if (const char *env = std::getenv("VMEMKV_BENCH_TARGET_RATIO")) {
    char *end = nullptr;
    const double parsed = std::strtod(env, &end);
    if (end != env && *end == '\0' && parsed > 0.0) {
      return parsed;
    }
  }
  return is_ltm_mode() ? 2.0 : 0.5;
}

static inline std::size_t get_target_base_bytes() { return detect_machine_memory_bytes(); }

static inline std::size_t get_target_corpus_bytes() {
  static const std::size_t cached = []() {
    const double target = static_cast<double>(get_target_base_bytes()) * get_target_ratio();
    return static_cast<std::size_t>(std::llround(target));
  }();
  return cached;
}

static void add_custom_context_from_env(const char *context_key, const char *env_name) {
  const char *value = std::getenv(env_name);
  if (value != nullptr && value[0] != '\0') {
    benchmark::AddCustomContext(context_key, value);
  }
}

static void register_benchmark_context() {
  add_custom_context_from_env("git_revision", "VMEMKV_CONTEXT_git_revision");
  add_custom_context_from_env("git_dirty", "VMEMKV_CONTEXT_git_dirty");
  add_custom_context_from_env("run_kind", "VMEMKV_CONTEXT_run_kind");
  add_custom_context_from_env("build_type", "VMEMKV_CONTEXT_build_type");
  add_custom_context_from_env("build_dir", "VMEMKV_CONTEXT_build_dir");
  add_custom_context_from_env("enable_rocksdb", "VMEMKV_CONTEXT_enable_rocksdb");
  benchmark::AddCustomContext("populate_bytes_scale_by_memory", std::to_string(get_target_ratio()));
  add_custom_context_from_env("scenario_order", "VMEMKV_CONTEXT_scenario_order");
  add_custom_context_from_env("flags", "VMEMKV_CONTEXT_flags");
  add_custom_context_from_env("memory_budget_source", "VMEMKV_CONTEXT_memory_budget_source");
  add_custom_context_from_env("memory_budget_bytes", "VMEMKV_CONTEXT_memory_budget_bytes");
  add_custom_context_from_env("memory_swap_max_bytes", "VMEMKV_CONTEXT_memory_swap_max_bytes");
  add_custom_context_from_env("swap_storage_media", "VMEMKV_CONTEXT_swap_storage_media");
  add_custom_context_from_env("kernel_release", "VMEMKV_CONTEXT_kernel_release");
  add_custom_context_from_env("cpu_model", "VMEMKV_CONTEXT_cpu_model");
  add_custom_context_from_env("cpu_count", "VMEMKV_CONTEXT_cpu_count");
  add_custom_context_from_env("mem_total_bytes", "VMEMKV_CONTEXT_mem_total_bytes");
  add_custom_context_from_env("cgroup_memory_limit_bytes", "VMEMKV_CONTEXT_cgroup_memory_limit_bytes");
  add_custom_context_from_env("swap_total_bytes", "VMEMKV_CONTEXT_swap_total_bytes");
  add_custom_context_from_env("instance_type", "VMEMKV_CONTEXT_instance_type");
  add_custom_context_from_env("aws_region", "VMEMKV_CONTEXT_aws_region");
  add_custom_context_from_env("memo", "VMEMKV_CONTEXT_memo");
  benchmark::AddCustomContext("t1_append_capacity_log2", std::to_string(vmemkv::Config<>::T1AppendCapacityLog2));
  benchmark::AddCustomContext("t1_append_capacity_entries", std::to_string(vmemkv::Config<>::T1AppendCapacityEntries));
  benchmark::AddCustomContext("t1_reorganize_soft_threshold_percent",
                              std::to_string(vmemkv::Config<>::T1ReorganizeSoftThresholdPercent));
  benchmark::AddCustomContext("t1_reorganize_hard_threshold_percent",
                              std::to_string(vmemkv::Config<>::T1ReorganizeHardThresholdPercent));
  benchmark::AddCustomContext("t2_storage_fragmentation_threshold_percent",
                              std::to_string(vmemkv::Config<>::T2StorageFragmentationThresholdPercent));
  benchmark::AddCustomContext("t2_ordering_fragmentation_threshold_percent",
                              std::to_string(vmemkv::Config<>::T2OrderingFragmentationThresholdPercent));
}

class BenchmarkContextRegistrar {
 public:
  static void register_all() { register_benchmark_context(); }
};

static inline bool prefer_large_value_first() {
  const char *env = std::getenv("VMEMKV_BENCH_LARGE_VALUE_FIRST");
  return env != nullptr && std::string(env) == "1";
}
static inline int insert_total_iterations(std::size_t value_size) {
  if (const char *override_total = std::getenv("VMEMKV_BENCH_INSERT_TOTAL_ITERATIONS")) {
    char *end = nullptr;
    long parsed = std::strtol(override_total, &end, 10);
    if (end != override_total && *end == '\0' && parsed > 0 && parsed <= std::numeric_limits<int>::max()) {
      return static_cast<int>(parsed);
    }
  }
  if (value_size == kInlineValueBytes) {
    return 5'000'000;
  }
  if (value_size == 64ULL * 1024ULL) {
    return 20'000;
  }
  return 1'000'000;
}

static auto store_label(std::string_view store_name) -> std::string {
  return store_name == "RocksDB" ? "RocksDB" : "VMemKV";
}

static auto variant_label(std::string_view store_name) -> std::string {
  if (store_name == "RocksDB") {
    return "RocksDB";
  }
  constexpr std::string_view kPrefix = "VMemKV/";
  std::string variant =
      store_name.rfind(kPrefix, 0) == 0 ? std::string(store_name.substr(kPrefix.size())) : std::string(store_name);
  std::replace(variant.begin(), variant.end(), '/', '-');
  return variant;
}

static auto value_label(size_t value_size) -> std::string {
  if (value_size == kInlineValueBytes) {
    return "8B";
  }
  if (value_size == 1024ULL) {
    return "1KB(20% 8B)";
  }
  if (value_size == 64ULL * 1024ULL) {
    return "64KB(20% 8B)";
  }
  return std::to_string(value_size) + "B(20% 8B)";
}

static auto benchmark_name(std::string_view store_name,
                           std::string_view op,
                           std::optional<std::string_view> mode = std::nullopt,
                           std::optional<std::string_view> dist = std::nullopt,
                           std::optional<std::string_view> value = std::nullopt) -> std::string {
  std::string name;
  name.reserve(128);
  name += "Store=";
  name += store_label(store_name);
  name += "/Variant=";
  name += variant_label(store_name);
  name += "/Op=";
  name += op;
  if (mode.has_value()) {
    name += "/Mode=";
    name += *mode;
  }
  if (dist.has_value()) {
    name += "/Dist=";
    name += *dist;
  }
  if (value.has_value()) {
    name += "/Value=";
    name += *value;
  }
  return name;
}

struct BenchmarkMetadata {
  std::size_t corpus_keys;
  std::size_t corpus_bytes;
};

static auto make_metadata(std::size_t corpus_keys, std::size_t value_bytes) -> BenchmarkMetadata {
  return BenchmarkMetadata{
      corpus_keys,
      corpus_keys * (kIndexKeyBytes + value_bytes),
  };
}

static auto set_benchmark_metadata(benchmark::State &state, const BenchmarkMetadata &meta) -> void {
  state.counters["CorpusKeys"] =
      benchmark::Counter(static_cast<double>(meta.corpus_keys), benchmark::Counter::kAvgThreads);
  state.counters["CorpusBytes"] =
      benchmark::Counter(static_cast<double>(meta.corpus_bytes), benchmark::Counter::kAvgThreads);
}
}  // namespace

// ---- Key helpers -------------------------------------------------------------
static auto ikey(std::size_t index) -> std::string {
  std::array<char, kIndexKeyBufferBytes> key_buffer{};
  std::snprintf(key_buffer.data(), key_buffer.size(), "k%015zx", index);
  return key_buffer.data();
}

static auto make_key(std::size_t index) -> std::string { return ikey(index); }

static auto get_value_size_for_key(std::size_t index, std::size_t target_size) -> std::size_t {
  if (target_size == kInlineValueBytes) {
    return kInlineValueBytes;
  }
  if (index % 5 == 0) {
    return kInlineValueBytes;
  }
  return target_size;
}

static auto make_value_for_key(std::size_t index, std::size_t target_size) -> std::string {
  return std::string(get_value_size_for_key(index, target_size), 'a');
}

struct PopulateOptions {
  size_t key_count;
  size_t value_size = kInlineValueBytes;
};

template <typename Store>
static void populate(Store &store, PopulateOptions options) {
  using Impl = std::remove_reference_t<decltype(store.impl())>;
  if constexpr (std::is_same_v<Impl, ::RocksDBStore>) {
    std::string error_message;
    if (!store.impl().bulk_load_impl(
            options.key_count, [](std::size_t index) { return make_key(index); }, options.value_size, &error_message)) {
      throw std::runtime_error(error_message.empty() ? "RocksDB bulk populate failed" : error_message);
    }
  } else {
    const size_t max_concurrency = std::min<size_t>(8, std::thread::hardware_concurrency());
    if (options.key_count < 10000 || max_concurrency <= 1) {
      for (std::size_t i = 0; i < options.key_count; ++i) {
        store.insert(make_key(i), make_value_for_key(i, options.value_size));
      }
    } else {
      const size_t num_threads = max_concurrency;
      std::vector<std::thread> workers;
      size_t chunk_size = (options.key_count + num_threads - 1) / num_threads;

      for (size_t t = 0; t < num_threads; ++t) {
        workers.emplace_back([&store, t, chunk_size, options]() {
          size_t start = t * chunk_size;
          size_t end = std::min(options.key_count, start + chunk_size);
          for (size_t i = start; i < end; ++i) {
            store.insert(make_key(i), make_value_for_key(i, options.value_size));
          }
        });
      }

      for (auto &w : workers) {
        w.join();
      }
    }
  }
}

// ---- Zipf distribution -------------------------------------------------------
class ZipfDistribution {
 public:
  struct Params {
    std::size_t item_count;
    double alpha;
  };

  explicit ZipfDistribution(Params params)
      : item_count_(params.item_count),
        alpha_(params.alpha),
        h_integral_x1_(h_integral(kZipfPivot) - 1.0),
        h_integral_inf_(h_integral(static_cast<double>(params.item_count) + kHalfStep)),
        s_(1.0 - h_integral_inv(h_integral(kZipfPivot) - 1.0)) {}

  auto operator()(std::mt19937_64 &rng) -> std::size_t {
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    while (true) {
      double uniform_sample = h_integral_inf_ + uni(rng) * (h_integral_x1_ - h_integral_inf_);
      double sample_position = h_integral_inv(uniform_sample);
      std::size_t rank = static_cast<std::size_t>(std::llround(sample_position));
      if (rank < 1) rank = 1;
      if (rank > item_count_) rank = item_count_;
      if (rank - sample_position <= s_ ||
          uniform_sample >= h_integral(static_cast<double>(rank) + kHalfStep) - h(rank)) {
        return rank - 1;
      }
    }
  }

 private:
  std::size_t item_count_;
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

using BenchmarkTypes = vmemkv::variants::AllPossibleTypes;

template <typename Visitor>
static void for_each_store_variant(Visitor &&visitor) {
  auto visit_one = [&](auto *dummy) {
    (void)dummy;
    using Store = std::remove_pointer_t<decltype(dummy)>;
    if constexpr (Store::kIsEnabled) {
      std::string filename = get_db_dir() + "/bench_" + Store::name() + ".bin";
      std::replace(filename.begin() + get_db_dir().size() + 1, filename.end(), '/', '_');

      visitor(Store::name().c_str(), [filename]() {
        return make_vmemkv(filename, [filename]() {
          if constexpr (std::is_same_v<Store, vmemkv::variants::VMemKV_RocksDB>) {
            return std::make_unique<Store>(filename);
          } else {
            return std::make_unique<Store>(filename, Store::ConfigType::DefaultT2CapacityBytes);
          }
        });
      });
    }
  };

  for_each_in_tuple<BenchmarkTypes>([&](auto *dummy) {
    using Store = std::remove_pointer_t<decltype(dummy)>;
    if constexpr (Store::kIsEnabled && std::is_same_v<Store, vmemkv::variants::VMemKV_RocksDB>) {
      visit_one(dummy);
    }
  });

  for_each_in_tuple<BenchmarkTypes>([&](auto *dummy) {
    using Store = std::remove_pointer_t<decltype(dummy)>;
    if constexpr (Store::kIsEnabled && !std::is_same_v<Store, vmemkv::variants::VMemKV_RocksDB>) {
      visit_one(dummy);
    }
  });
}

// =============================================================================
// Helper for managing store lifecycle across threads in Google Benchmark
// =============================================================================
struct StoreHolderBase {
  virtual ~StoreHolderBase() = default;
  virtual void reset_store() = 0;
};

template <typename StorePtr>
struct StoreHolder : public StoreHolderBase {
  std::mutex mutex;
  StorePtr store;
  int active_threads = 0;

  void reset_store() override {
    std::lock_guard<std::mutex> lock(mutex);
    store.reset();
  }
};

static std::mutex g_active_store_mutex;
static std::shared_ptr<StoreHolderBase> g_active_store_holder = nullptr;

static inline bool should_log_phase_steps(const std::string &name) {
  return name.find("/Op=Insert/") != std::string::npos || name.find("/Op=Delete/") != std::string::npos;
}

template <typename StorePtr, typename MakeStore, typename InitFn>
static void ensure_store_ready(std::shared_ptr<StoreHolder<StorePtr>> holder_ptr,
                               MakeStore &make,
                               InitFn &init_fn,
                               const std::string &name,
                               benchmark::State &state,
                               std::optional<double> &populate_seconds) {
  {
    std::lock_guard<std::mutex> lock(g_active_store_mutex);
    if (g_active_store_holder && g_active_store_holder != holder_ptr) {
      g_active_store_holder->reset_store();
    }
    g_active_store_holder = holder_ptr;
  }

  StoreHolder<StorePtr> &holder = *holder_ptr;
  std::lock_guard<std::mutex> lock(holder.mutex);
  if (!holder.store) {
    if (should_log_phase_steps(name)) {
      std::cout << "[phase] start init benchmark=" << name << " threads=" << state.threads() << std::endl;
    }
    holder.store = make();
    auto init_start = std::chrono::steady_clock::now();
    init_fn(*(holder.store));
    auto init_end = std::chrono::steady_clock::now();
    std::chrono::duration<double> init_elapsed = init_end - init_start;
    populate_seconds = init_elapsed.count();
    if (should_log_phase_steps(name)) {
      std::cout << "[phase] end init benchmark=" << name << " populate_sec=" << init_elapsed.count() << std::endl;
    }
  }
  holder.active_threads++;
}

template <typename StorePtr>
static void record_store_statistics(benchmark::State &state, StoreHolder<StorePtr> &holder) {
  if (state.thread_index() != 0) {
    return;
  }
  auto stats = holder.store->get_statistics();
  state.counters["Reorgs_T1"] =
      benchmark::Counter(static_cast<double>(stats.t1_reorg_count), benchmark::Counter::kDefaults);
  state.counters["Reorgs_T2"] =
      benchmark::Counter(static_cast<double>(stats.t2_reorg_count), benchmark::Counter::kDefaults);
  state.counters["Hard_Stalls"] =
      benchmark::Counter(static_cast<double>(stats.hard_stall_count), benchmark::Counter::kDefaults);
}

template <typename StorePtr>
static void release_store(StoreHolder<StorePtr> &holder,
                          bool destroy_store_after_benchmark,
                          bool reuse_store,
                          int max_threads,
                          benchmark::State &state,
                          std::optional<double> &reset_seconds) {
  std::lock_guard<std::mutex> lock(holder.mutex);
  holder.active_threads--;
  if (holder.active_threads != 0 || !destroy_store_after_benchmark) {
    return;
  }

  const bool should_reset = !reuse_store || state.threads() == max_threads;
  if (!should_reset) {
    return;
  }

  auto reset_start = std::chrono::steady_clock::now();
  holder.store.reset();
  auto reset_end = std::chrono::steady_clock::now();
  std::chrono::duration<double> reset_elapsed = reset_end - reset_start;
  reset_seconds = reset_elapsed.count();
}

template <typename StorePtr, typename RunFn>
static void execute_benchmark_run(StoreHolder<StorePtr> &holder,
                                  RunFn &run_fn,
                                  const std::string &name,
                                  benchmark::State &state) {
  if (should_log_phase_steps(name) && state.thread_index() == 0) {
    std::cout << "[phase] start run benchmark=" << name << " threads=" << state.threads() << std::endl;
  }

  auto start = std::chrono::steady_clock::now();
  run_fn(state, *(holder.store));
  auto end = std::chrono::steady_clock::now();
  std::chrono::duration<double> elapsed = end - start;
  state.counters["Elapsed_Sec"] = benchmark::Counter(elapsed.count(), benchmark::Counter::kAvgThreads);

  if (should_log_phase_steps(name) && state.thread_index() == 0) {
    std::cout << "[phase] end run benchmark=" << name << " elapsed_sec=" << elapsed.count() << std::endl;
  }
}

template <typename StorePtr, typename MakeStore, typename InitFn, typename RunFn>
static void register_bench(std::shared_ptr<StoreHolder<StorePtr>> holder,
                           const std::string &name,
                           const BenchmarkMetadata &metadata,
                           MakeStore make,
                           InitFn &&init_fn,
                           RunFn &&run_fn,
                           const std::vector<int> &thread_counts,
                           bool reuse_store = false,
                           int fixed_iterations = -1,
                           bool destroy_store_after_benchmark = true) {
  int max_threads = *std::max_element(thread_counts.begin(), thread_counts.end());
  auto init_fn_copy = std::forward<InitFn>(init_fn);
  auto run_fn_copy = std::forward<RunFn>(run_fn);

  for (int threads : thread_counts) {
    auto populate_seconds = std::make_shared<std::optional<double>>();
    auto reset_seconds = std::make_shared<std::optional<double>>();

    auto bench_func = [holder,
                       name,
                       make,
                       metadata,
                       init_fn_copy,
                       run_fn_copy,
                       reuse_store,
                       max_threads,
                       destroy_store_after_benchmark,
                       populate_seconds,
                       reset_seconds](benchmark::State &state) {
      ensure_store_ready(holder, make, init_fn_copy, name, state, *populate_seconds);
      set_benchmark_metadata(state, metadata);
      execute_benchmark_run(*holder, run_fn_copy, name, state);
      record_store_statistics(state, *holder);
      release_store(*holder, destroy_store_after_benchmark, reuse_store, max_threads, state, *reset_seconds);

      if (state.thread_index() == 0) {
        state.counters["Populate_Sec"] =
            benchmark::Counter(populate_seconds->value_or(0.0), benchmark::Counter::kDefaults);
        state.counters["Reset_Sec"] = benchmark::Counter(reset_seconds->value_or(0.0), benchmark::Counter::kDefaults);
      }
    };

    auto *reg = benchmark::RegisterBenchmark(name.c_str(), bench_func)->Threads(threads)->UseRealTime();
    if (name.find("/Op=YCSB-E/") != std::string::npos) {
      // YCSB-E already enforces a 30-second wall-clock window internally.
      // Keep Google Benchmark to a single execution so it does not add another
      // adaptive MinTime pass on top of the benchmark's own timing.
      reg->Iterations(1);
    } else if (fixed_iterations > 0) {
      reg->Iterations(fixed_iterations / threads);
    }
  }
}

template <typename MakeStore, typename InitFn, typename RunFn>
static void register_bench(const std::string &name,
                           const BenchmarkMetadata &metadata,
                           MakeStore make,
                           InitFn &&init_fn,
                           RunFn &&run_fn,
                           const std::vector<int> &thread_counts,
                           bool reuse_store = false,
                           int fixed_iterations = -1) {
  auto holder = std::make_shared<StoreHolder<decltype(make())>>();
  register_bench(holder,
                 name,
                 metadata,
                 make,
                 std::forward<InitFn>(init_fn),
                 std::forward<RunFn>(run_fn),
                 thread_counts,
                 reuse_store,
                 fixed_iterations,
                 true);
}

// =============================================================================
// Benchmark definitions
// =============================================================================
void register_all_benchmarks() {
  const int hw_threads = static_cast<int>(std::thread::hardware_concurrency());

  // Uniform 4-point thread counts for plotting scalability: {1, 4, 16, hw_threads}
  std::vector<int> thread_counts = {1, 4};
  if (16 < hw_threads) {
    thread_counts.push_back(16);
  }
  if (hw_threads > thread_counts.back()) {
    thread_counts.push_back(hw_threads);
  }
  std::sort(thread_counts.begin(), thread_counts.end());
  thread_counts.erase(std::unique(thread_counts.begin(), thread_counts.end()), thread_counts.end());

  for_each_store_variant([&](const char *name, auto make) {
    std::string sname(name);

    // Keep the benchmark matrix aligned with the scenario model:
    // 8B for in-memory, 1KB/64KB for LTM.
    std::vector<size_t> value_sizes =
        is_ltm_mode() ? std::vector<size_t>{1024ULL, 64ULL * 1024ULL} : std::vector<size_t>{kInlineValueBytes, 1024ULL};
    if (prefer_large_value_first()) {
      std::reverse(value_sizes.begin(), value_sizes.end());
    }
    for (size_t val_size : value_sizes) {
      const std::string value_name = value_label(val_size);
      const size_t target_corpus_bytes = get_target_corpus_bytes();
      const size_t corpus_size = (!is_ltm_mode() && val_size == kInlineValueBytes)
                                     ? kInMemoryInlineCorpusEntries
                                     : std::max<std::size_t>(1, target_corpus_bytes / (kIndexKeyBytes + val_size));
      auto crud_holder = std::make_shared<StoreHolder<decltype(make())>>();
      auto insert_holder = std::make_shared<StoreHolder<decltype(make())>>();

      // 1. Insert
      // In LTM mode we pre-populate the scenario-sized corpus first, then
      // measure incremental inserts on top of that corpus.
      // In in-memory mode we start from an empty store.
      const bool ltm_insert_mode = is_ltm_mode();
      auto insert_meta = make_metadata(corpus_size, val_size);
      register_bench(
          insert_holder,
          benchmark_name(sname, "Insert", std::nullopt, std::nullopt, value_name),
          insert_meta,
          make,
          [corpus_size, val_size, ltm_insert_mode](auto &store) {
            if (ltm_insert_mode) {
              populate(store, {corpus_size, val_size});
            }
          },
          [corpus_size, val_size](benchmark::State &state, auto &store) {
            std::string dummy_large(val_size, 'a');
            std::string dummy_8b(8, 'a');
            const std::size_t insert_start = is_ltm_mode() ? corpus_size : 0;
            int threads = state.threads();
            int thread_idx = state.thread_index();
            int i = 0;
            for (auto _ : state) {
              std::size_t key_index = insert_start + static_cast<std::size_t>(thread_idx) +
                                      static_cast<std::size_t>(i) * static_cast<std::size_t>(threads);
              bool inserted;
              if (val_size != 8 && key_index % 5 == 0) {
                inserted = store.insert(make_key(key_index), dummy_8b);
              } else {
                inserted = store.insert(make_key(key_index), dummy_large);
              }
              benchmark::DoNotOptimize(inserted);
              i++;
            }
            state.SetItemsProcessed(state.iterations());
          },
          thread_counts,
          false,
          insert_total_iterations(val_size),
          true);

      // 2. Get/Hit (Read-only: shared corpus)
      for (const char *dist : {"Zipf", "Uniform"}) {
        auto get_hit_meta = make_metadata(corpus_size, val_size);
        register_bench(
            crud_holder,
            benchmark_name(sname, "Get", "Hit", dist, value_name),
            get_hit_meta,
            make,
            [corpus_size, val_size](auto &store) { populate(store, {corpus_size, val_size}); },
            [corpus_size, dist](benchmark::State &state, auto &store) {
              std::mt19937_64 rng(kBenchmarkSeed + state.thread_index());
              ZipfDistribution zipf({corpus_size, 1.0});
              std::uniform_int_distribution<std::size_t> uniform_index(0, corpus_size - 1);
              for (auto _ : state) {
                std::size_t key_index = (std::string(dist) == "Zipf") ? zipf(rng) : uniform_index(rng);
                store.get(make_key(key_index),
                          [](std::span<const std::byte> value) { benchmark::DoNotOptimize(value); });
              }
              state.SetItemsProcessed(state.iterations());
            },
            thread_counts,
            true,
            -1,
            false);
      }

      // 3. Get/Miss (Read-only: shared corpus)
      auto get_miss_meta = make_metadata(corpus_size, val_size);
      register_bench(
          crud_holder,
          benchmark_name(sname, "Get", "Miss", "Zipf", value_name),
          get_miss_meta,
          make,
          [corpus_size, val_size](auto &store) { populate(store, {corpus_size, val_size}); },
          [corpus_size](benchmark::State &state, auto &store) {
            std::mt19937_64 rng(kBenchmarkSeed + state.thread_index());
            ZipfDistribution zipf({corpus_size, 1.0});
            for (auto _ : state) {
              std::size_t key_index = corpus_size + zipf(rng);
              store.get(make_key(key_index), [](std::span<const std::byte> value) { benchmark::DoNotOptimize(value); });
            }
            state.SetItemsProcessed(state.iterations());
          },
          thread_counts,
          true,
          -1,
          false);

      // 4. Update (Stateful: shared corpus)
      auto update_meta = make_metadata(corpus_size, val_size);
      register_bench(
          crud_holder,
          benchmark_name(sname, "Update", std::nullopt, "Zipf", value_name),
          update_meta,
          make,
          [corpus_size, val_size](auto &store) { populate(store, {corpus_size, val_size}); },
          [corpus_size, val_size](benchmark::State &state, auto &store) {
            std::string dummy(val_size, 'a');
            std::mt19937_64 rng(kBenchmarkSeed + state.thread_index());
            ZipfDistribution zipf({corpus_size, 1.0});
            for (auto _ : state) {
              std::size_t key_index = zipf(rng);
              bool updated = store.update(make_key(key_index), dummy);
              benchmark::DoNotOptimize(updated);
            }
            state.SetItemsProcessed(state.iterations());
          },
          thread_counts,
          true,
          -1,
          false);

      // 5. YCSB-E Benchmark (Short Range Scans, 30s mixed workload, hw_threads threads max)
      {
        auto ycsb_meta = make_metadata(corpus_size, val_size);
        std::vector<int> ycsb_threads = {hw_threads};

        struct YCSBState {
          std::unique_ptr<YCSBTimelineCollector> collector;
          std::mutex init_mutex;
          std::atomic<uint64_t> current_epoch{0};
          std::atomic<uint64_t> init_done_epoch{0};
          std::atomic<uint64_t> start_done_epoch{0};
          // Track last seen epoch per thread locally to this benchmark registration
          std::array<std::atomic<uint64_t>, 128> thread_last_epochs{};
          std::atomic<size_t> ready_threads{0};
        };
        auto ycsb_state = std::make_shared<YCSBState>();

        // YCSB-E populate size: respect YCSB_E_POPULATE env var, else use corpus_size (same scale as other benchmarks)
        const size_t ycsb_populate_size = [&]() -> size_t {
          const char *env = std::getenv("YCSB_E_POPULATE");
          if (env) return static_cast<size_t>(std::stoull(env));
          return corpus_size;  // same as GetHit/Insert benchmarks for fair comparison
        }();

        register_bench(
            crud_holder,
            benchmark_name(sname, "YCSB-E", std::nullopt, "Zipf", value_name),
            ycsb_meta,
            make,
            [ycsb_state, val_size, ycsb_populate_size](auto &store) {
              populate(store, {ycsb_populate_size, val_size});
              // Perform initial reorganize to ensure a fully sorted index before benchmark starts
              store.reorganize();

              // NOTE: collector setup and background reorg thread are launched per-run inside the
              // benchmark body (via epoch synchronization) to handle multiple trial/warmup runs correctly.
            },
            [ycsb_state, ycsb_populate_size, sname, variant_label = variant_label(sname), value_name, val_size](
                benchmark::State &state, auto &store) {
              std::mt19937_64 rng(kBenchmarkSeed + state.thread_index());
              std::uniform_int_distribution<int> op_dist(0, 99);

              const size_t thread_idx = state.thread_index();
              uint64_t my_last_epoch =
                  (thread_idx < 128) ? ycsb_state->thread_last_epochs[thread_idx].load(std::memory_order_relaxed) : 0;

              uint64_t local_epoch = 0;
              if (thread_idx == 0) {
                local_epoch = ycsb_state->current_epoch.load(std::memory_order_relaxed) + 1;

                // Setup fresh collector with the actual thread count
                ycsb_state->collector = std::make_unique<YCSBTimelineCollector>(state.threads(), ycsb_populate_size);
                ycsb_state->ready_threads.store(0, std::memory_order_release);

                // Publish epoch
                ycsb_state->current_epoch.store(local_epoch, std::memory_order_release);
                ycsb_state->init_done_epoch.store(local_epoch, std::memory_order_release);
              } else {
                // Wait for Thread 0 to finish initializing the current epoch
                while (ycsb_state->init_done_epoch.load(std::memory_order_acquire) <= my_last_epoch) {
                  std::this_thread::yield();
                }
                local_epoch = ycsb_state->init_done_epoch.load(std::memory_order_relaxed);
              }
              if (thread_idx < 128) {
                ycsb_state->thread_last_epochs[thread_idx].store(local_epoch, std::memory_order_relaxed);
              }

              auto *col = ycsb_state->collector.get();
              std::string dummy_large(val_size, 'a');
              std::string dummy_8b(8, 'a');
              uint64_t local_ops = 0;

              ycsb_state->ready_threads.fetch_add(1, std::memory_order_acq_rel);
              // Inside the loop: start the timeline on the very first iteration of this run epoch
              if (thread_idx == 0) {
                if (ycsb_state->start_done_epoch.load(std::memory_order_relaxed) < local_epoch) {
                  while (ycsb_state->ready_threads.load(std::memory_order_acquire) <
                         static_cast<size_t>(state.threads())) {
                    std::this_thread::yield();
                  }
                  col->start();
                  auto stats = store.get_statistics();
                  col->last_recorded_t1.store(stats.t1_reorg_count, std::memory_order_relaxed);
                  col->last_recorded_t2.store(stats.t2_reorg_count, std::memory_order_relaxed);
                  ycsb_state->start_done_epoch.store(local_epoch, std::memory_order_release);
                }
              } else {
                while (ycsb_state->start_done_epoch.load(std::memory_order_acquire) < local_epoch) {
                  std::this_thread::yield();
                }
              }

              while (true) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - col->start_time).count();
                if (elapsed >= YCSBTimelineCollector::kDurationSeconds) {
                  break;
                }

                // Thread 0: poll and record Reorg stats (background thread handles actual triggering)
                if (thread_idx == 0) {
                  auto stats = store.get_statistics();

                  // Track T1 Reorg
                  uint64_t current_t1 = stats.t1_reorg_count;
                  uint64_t prev_t1 = col->last_recorded_t1.load(std::memory_order_relaxed);
                  if (current_t1 > prev_t1) {
                    uint64_t diff = current_t1 - prev_t1;
                    col->t1_reorg_counts[elapsed].fetch_add(diff, std::memory_order_relaxed);
                    col->last_recorded_t1.store(current_t1, std::memory_order_relaxed);
                  }

                  // Track T2 Reorg
                  uint64_t current_t2 = stats.t2_reorg_count;
                  uint64_t prev_t2 = col->last_recorded_t2.load(std::memory_order_relaxed);
                  if (current_t2 > prev_t2) {
                    uint64_t diff = current_t2 - prev_t2;
                    col->t2_reorg_counts[elapsed].fetch_add(diff, std::memory_order_relaxed);
                    col->last_recorded_t2.store(current_t2, std::memory_order_relaxed);
                  }
                }

                int op_choice = op_dist(rng);
                if (op_choice < 95) {
                  // 95% Scan (100 items)
                  uint64_t max_keys = col->next_key_index.load(std::memory_order_relaxed);
                  ZipfDistribution dynamic_zipf({max_keys > 100 ? max_keys - 100 : 1, 1.0});
                  uint64_t scan_start = dynamic_zipf(rng);

                  size_t result_count = store.scan(
                      make_key(scan_start), make_key(scan_start + 99), [](std::span<const std::byte>, uint64_t value) {
                        benchmark::DoNotOptimize(value);
                      });
                  benchmark::DoNotOptimize(result_count);
                  col->record_op(thread_idx, true);
                } else {
                  // 5% Insert (with 20% 8B ratio for non-8B workloads)
                  uint64_t next_idx = col->next_key_index.fetch_add(1, std::memory_order_relaxed);
                  bool inserted;
                  if (val_size != 8 && next_idx % 5 == 0) {
                    inserted = store.insert(make_key(next_idx), dummy_8b);
                  } else {
                    inserted = store.insert(make_key(next_idx), dummy_large);
                  }
                  benchmark::DoNotOptimize(inserted);
                  col->record_op(thread_idx, false);
                }
                ++local_ops;
              }

              if (state.thread_index() == 0) {
                col->dump_json(sname, variant_label, value_name);
              }
              state.SetItemsProcessed(local_ops);
            },
            ycsb_threads,
            false,
            -1,
            false);
      }

      // 6. Delete
      // Delete mutates benchmark state, so it starts from a fresh corpus for
      // each thread-count instance. It is T1-only, but we keep it in both
      // scenarios so the T1 path remains comparable against RocksDB.
      auto delete_meta = make_metadata(corpus_size, val_size);
      const int delete_fixed_iterations = static_cast<int>(
          std::min<std::size_t>(corpus_size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
      register_bench(
          benchmark_name(sname, "Delete", std::nullopt, std::nullopt, value_name),
          delete_meta,
          make,
          [corpus_size, val_size](auto &store) { populate(store, {corpus_size, val_size}); },
          [](benchmark::State &state, auto &store) {
            std::size_t threads = static_cast<std::size_t>(state.threads());
            std::size_t thread_idx = static_cast<std::size_t>(state.thread_index());
            std::size_t i = 0;
            for (auto _ : state) {
              std::size_t key_index = static_cast<std::size_t>(thread_idx) +
                                      static_cast<std::size_t>(i) * static_cast<std::size_t>(threads);
              bool removed = store.remove(make_key(key_index));
              benchmark::DoNotOptimize(removed);
              i++;
            }
            state.SetItemsProcessed(state.iterations());
          },
          thread_counts,
          false,
          delete_fixed_iterations);

      auto scan_reorg_holder = std::make_shared<StoreHolder<decltype(make())>>();
      if (sname != "RocksDB") {
        constexpr int scan_count_reorg = 100;
        const size_t reorg_dataset_size = corpus_size;
        for (int dist_idx = 0; dist_idx < 2; ++dist_idx) {
          const char *dist = (dist_idx == 0) ? "Zipf" : "Uniform";
          auto scan_reorg_meta = make_metadata(reorg_dataset_size, kInlineValueBytes);
          register_bench(
              scan_reorg_holder,
              benchmark_name(sname, "ScanReorg", std::nullopt, dist, value_name),
              scan_reorg_meta,
              make,
              [reorg_dataset_size](auto &store) {
                populate(store, {reorg_dataset_size, kInlineValueBytes});
                store.reorganize();
              },
              [reorg_dataset_size, dist](benchmark::State &state, auto &store) {
                std::mt19937_64 rng(kBenchmarkSeed + state.thread_index());
                const std::size_t scan_span = reorg_dataset_size > static_cast<std::size_t>(scan_count_reorg)
                                                  ? reorg_dataset_size - static_cast<std::size_t>(scan_count_reorg)
                                                  : 1;
                ZipfDistribution zipf({scan_span, 1.0});
                std::uniform_int_distribution<std::size_t> uniform_index(0, scan_span - 1);
                for (auto _ : state) {
                  std::size_t scan_start = (std::string(dist) == "Zipf") ? zipf(rng) : uniform_index(rng);
                  size_t result_count =
                      store.scan(make_key(scan_start),
                                 make_key(scan_start + scan_count_reorg - 1),
                                 [](std::span<const std::byte>, uint64_t value) { benchmark::DoNotOptimize(value); });
                  benchmark::DoNotOptimize(result_count);
                }
                state.SetItemsProcessed(state.iterations());
              },
              thread_counts,
              true,
              -1,
              false);
        }
      }
    }
  });
}

int main(int argc, char **argv) {
  std::cout.setf(std::ios::unitbuf);
  BenchmarkContextRegistrar::register_all();
  benchmark::Initialize(&argc, argv);
  register_all_benchmarks();
  benchmark::RunSpecifiedBenchmarks();
  {
    std::lock_guard<std::mutex> lock(g_active_store_mutex);
    if (g_active_store_holder) {
      g_active_store_holder->reset_store();
      g_active_store_holder = nullptr;
    }
  }
  benchmark::Shutdown();
  return 0;
}
// NOLINTEND
