// NOLINTBEGIN
// bench_kv.cpp -- VMemKV microbenchmarks using Google Benchmark
//
// All benchmark functions are dynamic registrations over store variants.
// Runs are executed across a dynamic range of threads to measure scalability.
// Real-time (wall-clock) measurement is used for accurate throughput.

#include <benchmark/benchmark.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <sstream>
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

  // One forced store.checkpoint() call, fired deterministically at a fixed second-mark instead of
  // waiting for organic triggering. elapsed_sec is the call's own measured wall-clock duration, so
  // a throughput dip in the timeline can be attributed directly instead of inferred after the fact.
  struct ForcedEvent {
    int scheduled_sec;
    int fired_sec;     // may exceed scheduled_sec if a prior forced call ran long -- see
                       // kForcedTriggers' comment on why no guard skips a late trigger.
    std::string kind;  // always "checkpoint" -- see kForcedTriggers' comment on why a forced T1
                       // reorganize() is no longer part of this schedule.
    double elapsed_sec;
  };

  std::vector<ThreadCounter> counters;
  std::array<std::atomic<uint64_t>, kDurationSeconds> t2_checkpoint_counts{};
  // Separate from t2_checkpoint_counts: counts checkpoints the benchmark itself forced (see
  // kForcedTriggers below), as opposed to ones VMemKV triggered organically (WAL-size threshold).
  // Kept apart so a future report pass can render these as differently-colored vertical lines.
  // Exact per-call timing lives in forced_events instead -- these per-second buckets exist only to
  // keep the same "checkpoint activity per second" chart t2_checkpoint_counts already draws.
  std::array<std::atomic<uint64_t>, kDurationSeconds> t2_forced_checkpoint_counts{};
  // Only ever touched by thread_idx==0 (see the trigger site below), so no locking needed.
  std::vector<ForcedEvent> forced_events;
  std::atomic<uint64_t> last_recorded_checkpoint_t2{0};
  std::atomic<uint64_t> next_key_index;
  std::chrono::steady_clock::time_point start_time;

  YCSBTimelineCollector(size_t num_threads, uint64_t initial_keys)
      : counters(num_threads), next_key_index(initial_keys) {
    for (int i = 0; i < kDurationSeconds; ++i) {
      t2_checkpoint_counts[i].store(0, std::memory_order_relaxed);
      t2_forced_checkpoint_counts[i].store(0, std::memory_order_relaxed);
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

  // scenario_tag ("in_memory" or "ltm") must be part of the filename: it's the only thing that
  // distinguishes an in_memory run's timeline from an ltm run's, since both can share the exact
  // same store/variant/value-size combination. Without it, run_bench_aws_c6id.sh's 4-parallel
  // split (one instance per scenario+value-size) downloads both scenarios' files into the same
  // local directory under the same name, and whichever instance finishes later silently
  // overwrites the other's timeline data -- observed directly: in_memory/1KB's timeline was lost
  // this way when ltm/1KB (retried for hours after an unrelated OOM bug) finished afterward.
  void dump_json(std::string store_name, std::string variant_name, std::string val_name, std::string scenario_tag) {
    std::vector<uint64_t> total_scan(kDurationSeconds, 0);
    std::vector<uint64_t> total_insert(kDurationSeconds, 0);
    std::vector<uint64_t> total_checkpoint_t2(kDurationSeconds, 0);
    std::vector<uint64_t> total_forced_checkpoint_t2(kDurationSeconds, 0);

    for (const auto &tc : counters) {
      for (int i = 0; i < kDurationSeconds; ++i) {
        total_scan[i] += tc.scan_counts[i].load(std::memory_order_relaxed);
        total_insert[i] += tc.insert_counts[i].load(std::memory_order_relaxed);
      }
    }
    for (int i = 0; i < kDurationSeconds; ++i) {
      total_checkpoint_t2[i] = t2_checkpoint_counts[i].load(std::memory_order_relaxed);
      total_forced_checkpoint_t2[i] = t2_forced_checkpoint_counts[i].load(std::memory_order_relaxed);
    }

    // Sanitize parameters to prevent slash / from breaking folder path
    for (auto *s : {&store_name, &variant_name, &val_name, &scenario_tag}) {
      std::replace(s->begin(), s->end(), '/', '-');
      std::replace(s->begin(), s->end(), ' ', '_');
      std::replace(s->begin(), s->end(), '(', '_');
      std::replace(s->begin(), s->end(), ')', '_');
      std::replace(s->begin(), s->end(), '%', '_');
    }

    std::string filename =
        "/tmp/ycsb_e_timeline_" + scenario_tag + "_" + store_name + "_" + variant_name + "_" + val_name + ".json";
    std::ofstream out(filename);
    if (out.is_open()) {
      out << "{\n";
      out << "  \"store\": \"" << store_name << "\",\n";
      out << "  \"variant\": \"" << variant_name << "\",\n";
      out << "  \"value_size\": \"" << val_name << "\",\n";
      out << "  \"timeline\": [\n";
      for (int i = 0; i < kDurationSeconds; ++i) {
        out << "    {\"sec\": " << (i + 1) << ", \"scan_ops\": " << total_scan[i]
            << ", \"insert_ops\": " << total_insert[i] << ", \"t2_checkpoint_ops\": " << total_checkpoint_t2[i]
            << ", \"t2_forced_checkpoint_ops\": " << total_forced_checkpoint_t2[i] << "}";
        if (i < kDurationSeconds - 1) out << ",";
        out << "\n";
      }
      out << "  ],\n";
      out << "  \"forced_events\": [\n";
      for (size_t i = 0; i < forced_events.size(); ++i) {
        const auto &ev = forced_events[i];
        out << "    {\"scheduled_sec\": " << ev.scheduled_sec << ", \"fired_sec\": " << ev.fired_sec << ", \"kind\": \""
            << ev.kind << "\", \"elapsed_sec\": " << ev.elapsed_sec << "}";
        if (i + 1 < forced_events.size()) out << ",";
        out << "\n";
      }
      out << "  ]\n";
      out << "}\n";
    }
  }
};

// Fixed schedule for YCSB-E's forced checkpoint() calls (see the trigger site in
// register_ycsb_e_benchmark() below): natural triggering is unreliable within the 30s window
// (especially under LTM's lower throughput), so YCSB-E's timeline would otherwise show little to
// no checkpoint activity for some scenarios. Firing these deterministically guarantees comparable
// data points every run instead of leaving it to chance.
//
// t=10s and t=25s: two checkpoint() calls, spaced 15s apart -- enough room for each to complete
// and for surrounding throughput to resettle before/after. Two calls is enough to see whether a
// second cycle looks like the first.
//
// No forced reorganize() here: post-sharding, store->reorganize() forces every T1 shard through a
// full synchronous merge (see store_adapter.hpp's doc comment), so its cost scales with total
// corpus size like checkpoint()'s does -- it is no longer a cheap, distinct reference point worth
// contrasting against checkpoint() in this chart. It would also duplicate
// run_background_jobs_probe.sh's dedicated, cleaner measurement of that same forced-job cost.
//
// Deliberately no guard against a late-running call pushing a later trigger's fire time past its
// own schedule mark, or even past kDurationSeconds entirely: if checkpoint() at t=10s runs long
// enough to blow through the t=25s mark, the next check (on this thread's very next loop
// iteration, see the trigger site) fires it immediately back-to-back -- itself a legitimate,
// informative result, not a bug to engineer around.
struct ForcedTrigger {
  int second_mark;
};
constexpr std::array<ForcedTrigger, 2> kForcedTriggers{{
    {10},
    {25},
}};

constexpr std::size_t kIndexKeyBufferBytes = 32;
constexpr std::size_t kIndexKeyBytes = 16;
constexpr std::size_t kInlineValueBytes = 8;
constexpr std::size_t kInMemoryInlineCorpusEntries = 20'000'000;
// 1KB in-memory is fixed at roughly the same scale as the LTM/1KB scenario's own corpus (~8.26M
// keys/8.6GB, itself sized from a small fixed budget x ratio, not host RAM) rather than scaling
// with the host's real RAM (target_ratio=0.5) like every other non-8B case -- this keeps
// in_memory/1KB directly comparable to ltm/1KB (same data, different memory pressure) and fast to
// populate on real NVMe. 8B keeps its own separate, smaller fixed cap (kInMemoryInlineCorpusEntries)
// since even this same entry count at 8B's much smaller footprint would be needlessly small.
// Background and a ruled-out hang theory: implementation/vmemkv/docs/benchmark/20260805_in_memory_1kb_corpus_sizing.md.
constexpr std::size_t kInMemory1KBCorpusEntries = 8'000'000;
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

// Sweeps get_db_dir() for every file/directory this harness could have left behind (T2 files,
// WAL/manifest/.chkN.* checkpoints, rival on-disk directories, and the
// "*_master"/"*_master.building"/"*_clone_N" families in for_each_store_variant()), based purely
// on the "bench_" filename prefix every one of these paths shares (see
// for_each_store_variant()'s `filename` construction). `except_prefixes` spares any entry whose
// name starts with one of them -- the caller's own path, either about to be freshly rebuilt (safe
// to keep whatever partial state is there; the fresh construction wipes it anyway) or an
// already-built master a sibling holder (Get/Update/Delete/Scan/YCSB-E can all share one master
// -- see master_corpus_path() in for_each_store_variant()) may still need.
//
// Called from two places, deliberately not more: once unconditionally at process startup (see
// main()), and once per *fresh* (never a reuse/clone-of-existing-master) construction --
// make_vmemkv_fresh() below, and cleanup_before_master_build_if_needed() the instant before a
// rival or VMemKV master build that doesn't exist on disk yet. Never called on a path that's
// reusing an already-built master or checkpoint: doing so there would delete a sibling holder's
// still-needed data out from under it, not just this holder's own. This is what keeps at most one
// store-variant/value-size "generation" of files on disk at a time instead of letting every
// variant/value-size this process ever touches pile up simultaneously -- which is what was
// actually filling the disk (a single run's own *peak* usage, one generation at a time, is much
// smaller than its *cumulative* usage across every variant it touches without ever clearing the
// previous one).
static void cleanup_stale_benchmark_files(const std::set<std::string> &except_prefixes = {}) {
  const std::filesystem::path dir(get_db_dir());
  std::error_code list_error;
  std::size_t removed_count = 0;
  for (const auto &entry : std::filesystem::directory_iterator(dir, list_error)) {
    const std::string entry_name = entry.path().filename().string();
    if (!entry_name.starts_with("bench_")) {
      continue;
    }
    const bool spared = std::any_of(except_prefixes.begin(), except_prefixes.end(), [&](const std::string &prefix) {
      return entry_name.starts_with(prefix);
    });
    if (spared) {
      continue;
    }
    std::error_code remove_error;
    const auto removed = std::filesystem::remove_all(entry.path(), remove_error);
    if (!remove_error) {
      removed_count += static_cast<std::size_t>(removed > 0);
    }
  }
  if (removed_count > 0) {
    std::cout << "Cleaned up " << removed_count << " stale bench_* path(s) in " << dir.string() << std::endl;
  }
}

// Opt-out for main()'s startup cleanup_stale_benchmark_files() sweep and for the "sweep other
// stale corpora" step of every master build (cleanup_before_master_build_if_needed() for rivals,
// make_vmemkv_clone_from_checkpoint()'s own call into make_vmemkv_fresh() for VMemKV) -- *not*
// for make_vmemkv_fresh()'s ordinary (non-master) caller, Insert's non-LTM path, which stays
// unconditional (see its own comment).
//
// Needed by a two-pass "prime masters unconstrained, then measure under a memory-constrained
// cgroup" runner strategy (see run_bench.sh's LTM path). The priming pass builds every shared
// master corpus a run will need, at full (uncapped) disk speed, *before* the actual measurement
// pass enters the cgroup -- a real per-master populate measured there was 200-1200s (vs. ~20-30s
// unconstrained). Both passes need this flag:
// - The measurement pass's own *startup* cleanup must not wipe every master priming just spent
//   real time building, the instant it starts -- that master was built by a separate, now-exited
//   process, so nothing in this process's own state can tell "just built, keep it" apart from
//   "genuinely stale" without this flag.
// - The priming pass itself needs it for the same reason across *retries*: primed_master_
//   prefixes() (see register_primed_master_prefix()) already keeps one store's build from
//   sweeping another's within a single successful run, but that tracking is empty again on a
//   fresh process, so a retried priming invocation still needs this flag to avoid re-sweeping
//   masters a previous, separate priming attempt already finished.
//
// Callers using this flag are expected to have already done their own "start from a clean
// slate" sweep once, up front, before priming begins (see run_bench.sh) -- this flag does not
// replace that, it only suppresses the *automatic*, per-construction/per-startup sweeps that
// would otherwise fight a deliberate multi-process, multi-master-generation priming pass.
static inline bool should_skip_cleanup() {
  const char *env = std::getenv("VMEMKV_BENCH_SKIP_CLEANUP");
  return env != nullptr && std::string(env) == "1";
}

// Every master path this process has registered as one to never sweep away as "stale" for the
// rest of this run, whether because it already existed or because this process is about to build
// it -- see register_primed_master_prefix() below.
static auto primed_master_prefixes() -> std::set<std::string> & {
  static std::set<std::string> prefixes;
  return prefixes;
}
static std::mutex primed_master_prefixes_mutex;

// Registers `master_path`'s filename into primed_master_prefixes() and returns a snapshot of the
// full, up-to-date set (self included) for immediate use as a cleanup_stale_benchmark_files()
// spare-list -- copied out under the lock, safe to read lock-free afterward. A run that builds
// several stores' masters in one process (e.g. an LTM priming pass covering VMemKV, RocksDB,
// RocksDB-BlobDB, LMDB, ...) needs every prior store's master spared here, not just the one
// currently being built, or each store's build would sweep the previous stores' just-built
// masters right back out.
static auto register_primed_master_prefix(const std::string &master_path) -> std::set<std::string> {
  std::lock_guard<std::mutex> lock(primed_master_prefixes_mutex);
  primed_master_prefixes().insert(std::filesystem::path(master_path).filename().string());
  return primed_master_prefixes();
}

// Called immediately before a rival's CloneFromMasterTag constructor hands off to the actual
// build. If that master doesn't exist on disk yet, this is about to be a real build -- sweep away
// every *other* stale corpus first (sparing every master primed so far this process, per
// register_primed_master_prefix()), so switching which master gets built next never lets more
// than one generation of files pile up. A no-op once the master already exists (the ordinary
// repeat-reuse/clone case) -- see cleanup_stale_benchmark_files()'s comment for why this must
// stay skipped there.
//
// VMEMKV_BENCH_SKIP_CLEANUP (should_skip_cleanup()) still gates this sweep on top of
// register_primed_master_prefix()'s in-process tracking, for the one thing an in-process set can
// never know: whether an on-disk master this same process hasn't touched yet was actually built
// by an *earlier, separate* process invocation (e.g. a retried priming pass) rather than being
// genuinely stale.
static void cleanup_before_master_build_if_needed(const std::string &master_path) {
  const std::set<std::string> primed = register_primed_master_prefix(master_path);
  if (!should_skip_cleanup() && !std::filesystem::exists(master_path)) {
    cleanup_stale_benchmark_files(primed);
  }
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

// Shared by for_each_store_variant()'s registration loop (which needs corpus_size to size
// Insert/YCSB-E's own workload) and make_fresh_corpus()/make_fresh_corpus_checkpoint()'s
// rival-clone paths (which need it to know how many keys to bulk_load into a master corpus
// they haven't seen the caller compute yet).
static inline std::size_t corpus_size_for_value(std::size_t val_size) {
  if (!is_ltm_mode() && val_size == kInlineValueBytes) {
    return kInMemoryInlineCorpusEntries;
  }
  if (!is_ltm_mode() && val_size == 1024ULL) {
    return kInMemory1KBCorpusEntries;
  }
  const size_t target_corpus_bytes = get_target_corpus_bytes();
  return std::max<std::size_t>(1, target_corpus_bytes / (kIndexKeyBytes + val_size));
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
  benchmark::AddCustomContext("t1_shard_target_size_entries",
                              std::to_string(vmemkv::Config<>::T1ShardTargetSizeEntries));
  benchmark::AddCustomContext("t1_shard_split_threshold_percent",
                              std::to_string(vmemkv::Config<>::T1ShardSplitThresholdPercent));
}

class BenchmarkContextRegistrar {
 public:
  static void register_all() { register_benchmark_context(); }
};

static inline bool prefer_large_value_first() {
  const char *env = std::getenv("VMEMKV_BENCH_LARGE_VALUE_FIRST");
  return env != nullptr && std::string(env) == "1";
}
// Insert has no min-time-adaptive path via Google Benchmark's normal iteration counting, for the
// same reason Delete/YCSB-E don't (see delete_time_budget_seconds()'s comment): a fixed iteration
// count baked in for one machine/engine's write latency badly over- or under-shoots a reasonable
// wall-clock budget elsewhere. Calibrating a fixed iteration count against a generic
// write()+fsync() probe at registration time would not work here: that probe reflects only raw
// filesystem fsync latency, not any given engine's actual per-insert cost, which measurably
// diverges -- under an LTM-sized corpus, LMDB's real Insert cost (B+Tree page-split/COW cost grows
// with tree depth) would run 26-42s against a 5s target using that calibration. Instead it runs
// its own bounded wall-clock loop (same technique as Delete/YCSB-E): insert unique keys until the
// time budget elapses, so every engine and environment gets the same wall-clock cap regardless of
// its actual per-insert cost.
static inline double insert_time_budget_seconds() {
  if (const char *override_seconds = std::getenv("VMEMKV_BENCH_INSERT_TIME_BUDGET_SECONDS")) {
    char *end = nullptr;
    double parsed = std::strtod(override_seconds, &end);
    if (end != override_seconds && *end == '\0' && parsed > 0.0) {
      return parsed;
    }
  }
  return 5.0;
}

// Delete has no min_time-adaptive path via google-benchmark's normal iteration
// counting (each deleted key is gone for good, so re-running more iterations isn't
// meaningful once the corpus is exhausted). A *fixed iteration count* is not a safe
// substitute: per-delete cost can degrade non-linearly with how much has already been
// deleted (observed locally: LMDB/1KB went from ~212K deletes/sec at a 20M-key corpus
// to ~5K deletes/sec at a 16M-key corpus -- a 40x swing from value size alone), so any
// fixed count can still land in a slow regime for some engine/corpus combination.
// Instead, Delete runs its own bounded wall-clock loop (same technique as YCSB-E):
// walk the corpus deleting keys until either the time budget or the corpus is
// exhausted, whichever comes first. This caps worst-case per-cell time regardless of
// how badly a given engine degrades, while still populating and testing against the
// *full* corpus_size so LTM scenarios keep exercising genuine memory pressure (a
// fixed small delete-only corpus would fit in the LTM memory budget and stop being
// "larger than memory").
static inline double delete_time_budget_seconds() {
  if (const char *override_seconds = std::getenv("VMEMKV_BENCH_DELETE_TIME_BUDGET_SECONDS")) {
    char *end = nullptr;
    double parsed = std::strtod(override_seconds, &end);
    if (end != override_seconds && *end == '\0' && parsed > 0.0) {
      return parsed;
    }
  }
  return 5.0;
}

// Rival backends (RocksDB, RocksDB-BlobDB, LMDB, ...) report a bare name with no
// "VMemKV/" prefix; only actual VMemKV configurations use that prefix. This lets a
// new rival be added without touching this string matching.
static auto store_label(std::string_view store_name) -> std::string {
  constexpr std::string_view kVMemKVPrefix = "VMemKV/";
  return store_name.rfind(kVMemKVPrefix, 0) == 0 ? "VMemKV" : std::string(store_name);
}

static auto variant_label(std::string_view store_name) -> std::string {
  constexpr std::string_view kPrefix = "VMemKV/";
  if (store_name.rfind(kPrefix, 0) != 0) {
    return std::string(store_name);
  }
  std::string variant = std::string(store_name.substr(kPrefix.size()));
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

// Folds every byte of `value` into a checksum so that DoNotOptimize() actually forces
// the whole span to be read (and, in the LTM case, paged in from swap if needed).
// Without this, a callback that only touches the span's pointer/length never faults in
// pages beyond the one containing the first few bytes, which silently skips the real
// cost of fetching large values.
static auto touch_bytes(std::span<const std::byte> value) noexcept -> uint64_t {
  uint64_t checksum = 0;
  for (std::byte b : value) {
    checksum = (checksum * 131) + static_cast<uint8_t>(b);
  }
  return checksum;
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

// Fills the value with pseudo-random bytes (xorshift64, seeded from `index`) rather than a
// repeated character. A repeated-byte value is trivially compressible and skews any benchmark
// that involves a compression layer (e.g. zswap) toward an unrealistically large win; xorshift64
// output has no such structure while staying deterministic across repeated populate() calls.
static auto make_value_for_key(std::size_t index, std::size_t target_size) -> std::string {
  const std::size_t size = get_value_size_for_key(index, target_size);
  std::string value(size, '\0');
  uint64_t state = static_cast<uint64_t>(index) * 0x9E3779B97F4A7C15ULL + 1;
  for (std::size_t offset = 0; offset < size; offset += sizeof(uint64_t)) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    std::memcpy(value.data() + offset, &state, std::min(sizeof(uint64_t), size - offset));
  }
  return value;
}

struct PopulateOptions {
  size_t key_count;
  size_t value_size = kInlineValueBytes;
};

// Populates via the uniform bulk_load() entry point (StoreAdapter -- see store_adapter.hpp),
// which every backend implements: rivals via their own batched WriteBatch/txn machinery,
// VMemKV by writing straight into T1/T2 and skipping the WAL entirely. Callers that need the
// result to be durable (survive a crash, or be reusable by a later construction) must trigger
// that separately -- see bulk_load()'s own doc comment -- typically via store.checkpoint()
// immediately after this returns.
template <typename Store>
static void populate(Store &store, PopulateOptions options) {
  store.bulk_load(
      options.key_count,
      [](std::size_t index) { return make_key(index); },
      [value_size = options.value_size](std::size_t index) { return make_value_for_key(index, value_size); });
}

// Same logical key set {0..key_count) as populate(), but inserted in a fixed-seed-shuffled
// order instead of ascending key order. Used by reorg_probe (below) so its reorganize()/
// checkpoint() timing measurements exercise a T2 layout that's genuinely scattered relative to
// key order, instead of the already-key-ordered layout ascending-order populate() would leave
// (bulk_load_impl() appends in call order). Deterministic (fixed kBenchmarkSeed) so repeated
// builds of the same master are byte-identical.
template <typename Store>
static void populate_random_order(Store &store, PopulateOptions options) {
  std::vector<std::size_t> order(options.key_count);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::mt19937_64 rng(kBenchmarkSeed);
  std::shuffle(order.begin(), order.end(), rng);
  store.bulk_load(
      options.key_count,
      [&order](std::size_t i) { return make_key(order[i]); },
      [&order, value_size = options.value_size](std::size_t i) { return make_value_for_key(order[i], value_size); });
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

// Always starts from a genuinely wiped, empty store: the T2 file, WAL, manifest, and the T1/T2
// checkpoint files (path + ".t1chk" / ".t2chk"). Used for rival backends (their own on-disk
// format has no relationship to VMemKV's checkpoint reuse below) and for scenarios that genuinely
// need a fresh/empty corpus every time -- Insert (measures inserting into an empty store).
// Leaving the manifest/checkpoint files behind would let a "fresh" store silently fast-boot from
// an unrelated scenario's leftover checkpoint whenever it happened to share this same unscoped
// path and had called checkpoint() -- corrupting later scenarios sharing that path (a
// fresh-looking Insert silently rejecting every key as already-present, since it isn't actually
// empty).
//
// `extra_spared_prefixes`/`sweep_other_stale_paths` default to "spare nothing extra, always
// sweep" for the ordinary (Insert non-LTM) caller, whose own files are genuinely
// one-generation-at-a-time even across a whole measurement pass and so deliberately ignore
// VMEMKV_BENCH_SKIP_CLEANUP (see should_skip_cleanup()'s comment). make_vmemkv_clone_from_
// checkpoint() overrides both instead: its own in-process master registry as
// `extra_spared_prefixes`, so building this path's master doesn't sweep away a sibling store
// variant's master this same process already primed, and `!should_skip_cleanup()` as
// `sweep_other_stale_paths`, for parity with rivals' cleanup_before_master_build_if_needed() --
// this path's own wipe/rebuild of `path` itself (below) always happens regardless.
template <typename Constructor>
static auto make_vmemkv_fresh(const std::string &path,
                              Constructor &&constructor,
                              std::set<std::string> extra_spared_prefixes = {},
                              bool sweep_other_stale_paths = true) {
  // See cleanup_stale_benchmark_files()'s comment: every fresh (non-reuse) construction also
  // sweeps away whatever other variant/value-size's files are still sitting on disk, so at most
  // one generation ever accumulates at a time.
  if (sweep_other_stale_paths) {
    extra_spared_prefixes.insert(std::filesystem::path(path).filename().string());
    cleanup_stale_benchmark_files(extra_spared_prefixes);
  }

  std::error_code error_code;
  std::filesystem::remove(path, error_code);
  vmemkv::remove_wal_segments(vmemkv::derive_wal_path(path));
  std::filesystem::remove(vmemkv::derive_manifest_path(path), error_code);
  std::filesystem::remove(vmemkv::derive_t1_chk_path(path), error_code);
  std::filesystem::remove(vmemkv::derive_t2_chk_path(path), error_code);

  return constructor();
}

// Copies only `[0, live_bytes)` of `source` into `dest` (creating/truncating `dest` first),
// preserving `source`'s full logical size as a sparse hole beyond that point instead of
// materializing it. Needed because a T2 checkpoint file's logical size is its *capacity*
// (DefaultT2CapacityBytes, currently 1 TiB -- see load_checkpoint_if_present()'s fstat()-based
// capacity detection, which relies on the file's apparent size matching what the store was
// originally constructed with), almost all of which is an untouched sparse hole beyond
// `live_bytes` for any real corpus. std::filesystem::copy_file() has no notion of holes and
// copies the full logical extent byte-for-byte, turning a multi-GB-real/1-TiB-logical sparse
// master into a 1-TiB-real, fully-allocated clone -- easily exhausting disk space on a real host.
static void copy_t2_checkpoint_sparse(const std::filesystem::path &source,
                                      const std::filesystem::path &dest,
                                      uint64_t live_bytes) {
  const int src_fd = ::open(source.c_str(), O_RDONLY);
  if (src_fd < 0) {
    throw std::runtime_error("Failed to open T2 checkpoint source for clone: " + source.string());
  }
  struct FDGuard {
    int fd;
    ~FDGuard() {
      if (fd >= 0) ::close(fd);
    }
  } src_guard{src_fd};

  struct stat source_stat {};
  if (::fstat(src_fd, &source_stat) != 0) {
    throw std::runtime_error("Failed to stat T2 checkpoint source for clone: " + source.string());
  }
  const auto logical_size = static_cast<uint64_t>(source_stat.st_size);

  const int dst_fd = ::open(dest.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (dst_fd < 0) {
    throw std::runtime_error("Failed to create T2 checkpoint clone: " + dest.string());
  }
  FDGuard dst_guard{dst_fd};
  if (::ftruncate(dst_fd, static_cast<off_t>(logical_size)) != 0) {
    throw std::runtime_error("Failed to size T2 checkpoint clone: " + dest.string());
  }

  std::vector<std::byte> buffer(4ULL * 1024 * 1024);
  uint64_t offset = 0;
  while (offset < live_bytes) {
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), live_bytes - offset));
    const ssize_t bytes_read = ::pread(src_fd, buffer.data(), chunk, static_cast<off_t>(offset));
    if (bytes_read < 0) {
      throw std::runtime_error("Failed to read T2 checkpoint for clone: " + source.string());
    }
    if (bytes_read == 0) {
      break;  // Live region ends before live_bytes (e.g. a still-sparse tail) -- nothing more to copy.
    }
    if (::pwrite(dst_fd, buffer.data(), static_cast<size_t>(bytes_read), static_cast<off_t>(offset)) != bytes_read) {
      throw std::runtime_error("Failed to write T2 checkpoint clone: " + dest.string());
    }
    offset += static_cast<uint64_t>(bytes_read);
  }
}

// Builds (once) a VMemKV checkpoint at `master_path` -- a real populate() + checkpoint() -- if
// one isn't already there, then clones it into a fresh instance path and writing a matching
// manifest, with *no* WAL at the new path -- so constructing a VMemKVImpl there fast-boots
// straight from the checkpoint with nothing to replay.
//
// The T1 checkpoint file is hardlinked (checkpoint.hpp's write_t1_checkpoint(): always written to
// a temp path and rename()'d onto the final one, so a later cycle on the clone replaces the
// clone's directory entry with a fresh inode rather than mutating the one still shared with the
// master -- exactly like RocksDB's SST files, see RocksDBStore::clone_from()'s comment for the
// same reasoning). The T2 checkpoint file cannot use the same trick: checkpoint_internal()
// durabilizes T2's tail via pwrite() directly into the persistent file, in place, so a hardlinked
// T2 file would let the clone's own later checkpoint cycles corrupt the master (and any other
// clone sharing that inode) -- it's copied instead (see copy_t2_checkpoint_sparse() above).
//
// This is what lets Get/Update/Delete/YCSB-E/Scan (see their registrations below) all get a
// fresh, fully-populated, fully-reorganized instance on every construction without paying
// bulk_load's cost more than once per (variant, val_size) -- and, since they all pass the same
// (val_size, key_count), they transparently share that one on-disk master rather than each
// building their own. Every clone here is independent and starts with no WAL of its own, so a
// later mutation (e.g. Delete removing keys, or YCSB-E's insert mix growing the corpus) on one
// clone never touches the master or any sibling clone taken from it.
template <typename Store>
static auto make_vmemkv_clone_from_checkpoint(const std::string &master_path,
                                              std::size_t val_size,
                                              std::size_t key_count) -> std::unique_ptr<Store> {
  const std::filesystem::path master_manifest_path = vmemkv::derive_manifest_path(master_path);
  auto manifest = vmemkv::read_manifest(master_manifest_path);
  if (!manifest.has_value()) {
    // First time for this (variant, val_size): build the master for real, then discard the
    // in-memory instance -- only the checkpoint files it leaves behind on disk matter from here
    // on. register_primed_master_prefix() spares every master this process has already primed
    // (spanning other store variants sharing this same run -- see its comment) from
    // make_vmemkv_fresh()'s sweep, so building this one doesn't wipe them back out; should_skip_
    // cleanup() additionally suppresses that sweep entirely, for parity with rivals' cleanup_
    // before_master_build_if_needed() -- see should_skip_cleanup()'s comment.
    auto master_store = make_vmemkv_fresh(
        master_path,
        [master_path]() { return std::make_unique<Store>(master_path, Store::ConfigType::DefaultT2CapacityBytes); },
        register_primed_master_prefix(master_path),
        !should_skip_cleanup());
    populate(*master_store, {key_count, val_size});
    master_store->impl().checkpoint();
    master_store.reset();
    manifest = vmemkv::read_manifest(master_manifest_path);
    if (!manifest.has_value()) {
      throw std::runtime_error("VMemKV master checkpoint build did not produce a valid manifest: " + master_path);
    }
  }

  // Fixed path, not an ever-incrementing counter: g_active_store_holder (see StoreHolder below)
  // is a single global slot, so at most one clone of any given master is ever live process-wide
  // -- switching holders always resets the previous one before this runs again. Reusing one path
  // means every subsequent clone's own remove-then-rebuild below (already required, to discard
  // whatever the *previous* master-build or clone left there) also bounds this master's cumulative
  // on-disk clone footprint to one clone's worth instead of accumulating a fresh multi-GB copy per
  // benchmark case for the rest of the process's run.
  const std::string instance_path = master_path + "_clone";
  std::error_code ignored;
  std::filesystem::remove(instance_path, ignored);
  vmemkv::remove_wal_segments(vmemkv::derive_wal_path(instance_path));
  std::filesystem::remove(vmemkv::derive_manifest_path(instance_path), ignored);

  const auto master_t1 = vmemkv::derive_t1_chk_path(master_path);
  const auto master_t2 = vmemkv::derive_t2_chk_path(master_path);
  const auto instance_t1 = vmemkv::derive_t1_chk_path(instance_path);
  const auto instance_t2 = vmemkv::derive_t2_chk_path(instance_path);
  std::filesystem::remove(instance_t1, ignored);
  std::filesystem::remove(instance_t2, ignored);
  std::error_code link_error;
  std::filesystem::create_hard_link(master_t1, instance_t1, link_error);
  if (link_error) {
    throw std::runtime_error("Failed to hardlink T1 checkpoint for clone: " + link_error.message());
  }
  copy_t2_checkpoint_sparse(master_t2, instance_t2, manifest->t2_bytes_used);
  vmemkv::write_manifest(vmemkv::derive_manifest_path(instance_path), manifest->generation, manifest->t2_bytes_used);

  return std::make_unique<Store>(instance_path, Store::ConfigType::DefaultT2CapacityBytes);
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

// Rival backends take a bare path (they manage their own on-disk capacity/layout);
// only VMemKV configurations take the extra T2 capacity argument.
template <typename Store>
inline constexpr bool kUsesSingleArgConstructor = std::is_same_v<Store, vmemkv::variants::VMemKV_RocksDB> ||
                                                  std::is_same_v<Store, vmemkv::variants::VMemKV_RocksDBBlobDB> ||
                                                  std::is_same_v<Store, vmemkv::variants::VMemKV_LMDB>;

// A per-(val_size, key_count) master-corpus path shared by every scenario in visit_one() below
// that wants a fully-populated, fully-reorganized/committed corpus (Get/Update/Delete/YCSB-E/
// Scan/Insert's LTM pre-populate): as long as they pass the same (val_size, key_count), they all
// clone from -- and, the first time, jointly pay to build -- exactly one on-disk master, instead
// of each independently paying its own real-populate cost. Not used by Insert's non-LTM path
// (measures inserting into a genuinely empty store, so there is nothing to share) -- that one
// keeps using make_bench_store_fresh() unchanged.
static auto bench_master_corpus_path(const std::string &filename,
                                     std::size_t val_size,
                                     std::size_t key_count) -> std::string {
  return filename + "_v" + std::to_string(val_size) + "_n" + std::to_string(key_count) + "_master";
}

template <typename Store>
static auto make_bench_store_fresh(const std::string &filename) {
  return make_vmemkv_fresh(filename, [filename]() {
    if constexpr (kUsesSingleArgConstructor<Store>) {
      return std::make_unique<Store>(filename);
    } else {
      return std::make_unique<Store>(filename, Store::ConfigType::DefaultT2CapacityBytes);
    }
  });
}

// The rival-backend half of make_bench_fresh_corpus_checkpoint() below: rivals have no
// VMemKV-specific checkpoint/manifest concept, so their "clone from a shared master" mechanism is
// instead CloneFromMasterTag, driven off bench_master_corpus_path() directly. VMemKV itself never
// takes this branch (see make_bench_fresh_corpus_checkpoint()'s own branch). Takes key_count
// explicitly (rather than deriving it from val_size) so callers whose corpus size doesn't match
// corpus_size_for_value() still get a correctly-sized master instead of silently
// reusing/misreporting a mismatched one.
template <typename Store>
static auto make_bench_fresh_corpus(const std::string &filename, std::size_t val_size, std::size_t key_count) {
  if constexpr (kUsesSingleArgConstructor<Store>) {
    const std::string master_path = bench_master_corpus_path(filename, val_size, key_count);
    cleanup_before_master_build_if_needed(master_path);
    return std::make_unique<Store>(
        typename Store::Impl::CloneFromMasterTag{},
        master_path,
        key_count,
        [](std::size_t index) { return make_key(index); },
        [val_size](std::size_t index) { return make_value_for_key(index, val_size); });
  } else {
    return make_vmemkv_fresh(filename, [filename]() {
      return std::make_unique<Store>(filename, Store::ConfigType::DefaultT2CapacityBytes);
    });
  }
}

// Get/Update/Delete/YCSB-E/Scan all want the *same* thing: a fresh, fully-populated,
// fully-reorganized/committed instance, cheaply, on every construction -- so for VMemKV this
// clones from a shared checkpoint (see make_vmemkv_clone_from_checkpoint()'s comment) instead of
// make_bench_fresh_corpus()'s plain make_vmemkv_fresh(). As long as callers pass the same
// (val_size, key_count) -- which all of the scenarios above do, since they all measure the same
// corpus_size -- they transparently share one on-disk master and its build cost is paid at most
// once per (variant, val_size), no matter which scenario happens to trigger it first. For rival
// backends, this is exactly make_bench_fresh_corpus() above (already clone-based for them
// regardless of which reorganize-state concept applies, since they have none). base_mmap is
// established unconditionally by T2FlatFile's constructor on every fresh construction, including
// one that adopts an existing checkpoint (see load_checkpoint_if_present()), so this one shared,
// checkpoint-cloned master already carries it for every VMemKV variant.
template <typename Store>
static auto make_bench_fresh_corpus_checkpoint(const std::string &filename,
                                               std::size_t val_size,
                                               std::size_t key_count) {
  if constexpr (kUsesSingleArgConstructor<Store>) {
    return make_bench_fresh_corpus<Store>(filename, val_size, key_count);
  } else {
    // Scoped by (val_size, key_count), same as the rival masters above (see
    // bench_master_corpus_path()'s comment) -- different value sizes (or a YCSB_E_POPULATE
    // override) must never collide on the same master checkpoint.
    return make_vmemkv_clone_from_checkpoint<Store>(
        bench_master_corpus_path(filename, val_size, key_count), val_size, key_count);
  }
}

template <typename Visitor>
static void for_each_store_variant(Visitor &&visitor) {
  auto visit_one = [&](auto *dummy) {
    (void)dummy;
    using Store = std::remove_pointer_t<decltype(dummy)>;
    if constexpr (Store::kIsEnabled) {
      std::string filename = get_db_dir() + "/bench_" + Store::name() + ".bin";
      std::replace(filename.begin() + get_db_dir().size() + 1, filename.end(), '/', '_');

      auto make = [filename]() { return make_bench_store_fresh<Store>(filename); };
      auto make_fresh_corpus_checkpoint = [filename](std::size_t val_size, std::size_t key_count) {
        return make_bench_fresh_corpus_checkpoint<Store>(filename, val_size, key_count);
      };

      visitor(Store::name().c_str(), make, make_fresh_corpus_checkpoint);
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
    // Started before make() (not just init_fn) so Populate_Sec reflects the full cost of
    // getting a ready-to-measure store, including construction itself -- the make() passed in
    // here (see for_each_store_variant() in this file) can do real work there (a one-time
    // master bulk_load, or a clone from it; see rivals/*.hpp's CloneFromMasterTag constructors);
    // starting the timer only after make() returns would leave that work completely unmeasured.
    auto init_start = std::chrono::steady_clock::now();
    holder.store = make();
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
  state.counters["T1_Splits"] =
      benchmark::Counter(static_cast<double>(stats.t1_split_count), benchmark::Counter::kDefaults);
  state.counters["Checkpoints"] =
      benchmark::Counter(static_cast<double>(stats.checkpoint_count), benchmark::Counter::kDefaults);
  state.counters["Reorganize_Wait_Duration_us"] =
      benchmark::Counter(static_cast<double>(stats.total_reorganize_wait_duration_us), benchmark::Counter::kDefaults);
  state.counters["Append_Region_Live"] =
      benchmark::Counter(static_cast<double>(stats.append_region_live_count), benchmark::Counter::kDefaults);
  state.counters["Append_Region_Peak"] =
      benchmark::Counter(static_cast<double>(stats.append_region_peak_count), benchmark::Counter::kDefaults);
}

template <typename StorePtr>
static void release_store(StoreHolder<StorePtr> &holder,
                          bool destroy_store_after_benchmark,
                          std::optional<double> &reset_seconds) {
  std::lock_guard<std::mutex> lock(holder.mutex);
  holder.active_threads--;
  if (holder.active_threads != 0 || !destroy_store_after_benchmark) {
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
                           bool destroy_store_after_benchmark = true) {
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
                       destroy_store_after_benchmark,
                       populate_seconds,
                       reset_seconds](benchmark::State &state) {
      ensure_store_ready(holder, make, init_fn_copy, name, state, *populate_seconds);
      set_benchmark_metadata(state, metadata);
      execute_benchmark_run(*holder, run_fn_copy, name, state);
      record_store_statistics(state, *holder);
      release_store(*holder, destroy_store_after_benchmark, *reset_seconds);

      if (state.thread_index() == 0) {
        state.counters["Populate_Sec"] =
            benchmark::Counter(populate_seconds->value_or(0.0), benchmark::Counter::kDefaults);
        state.counters["Reset_Sec"] = benchmark::Counter(reset_seconds->value_or(0.0), benchmark::Counter::kDefaults);
      }
    };

    auto *reg = benchmark::RegisterBenchmark(name.c_str(), bench_func)->Threads(threads)->UseRealTime();
    if (name.find("/Op=YCSB-E/") != std::string::npos || name.find("/Op=Delete/") != std::string::npos ||
        name.find("/Op=Insert/") != std::string::npos) {
      // YCSB-E, Delete, and Insert all enforce their own wall-clock window internally.
      // Keep Google Benchmark to a single execution so it does not add another
      // adaptive MinTime pass on top of the benchmark's own timing.
      reg->Iterations(1);
    }
  }
}

template <typename MakeStore, typename InitFn, typename RunFn>
static void register_bench(const std::string &name,
                           const BenchmarkMetadata &metadata,
                           MakeStore make,
                           InitFn &&init_fn,
                           RunFn &&run_fn,
                           const std::vector<int> &thread_counts) {
  auto holder = std::make_shared<StoreHolder<decltype(make())>>();
  register_bench(
      holder, name, metadata, make, std::forward<InitFn>(init_fn), std::forward<RunFn>(run_fn), thread_counts, true);
}

// YCSB-E Benchmark (Short Range Scans, 30s mixed workload, hw_threads threads max), registered
// once per (store variant, value size) by register_all_benchmarks() below.
template <typename Holder, typename MakeFreshCorpusCheckpoint>
static void register_ycsb_e_benchmark(Holder ycsb_holder,
                                      int hw_threads,
                                      const std::string &sname,
                                      const std::string &value_name,
                                      MakeFreshCorpusCheckpoint make_fresh_corpus_checkpoint,
                                      std::size_t val_size,
                                      std::size_t corpus_size) {
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
      ycsb_holder,
      benchmark_name(sname, "YCSB-E", std::nullopt, "Zipf", value_name),
      ycsb_meta,
      [make_fresh_corpus_checkpoint, val_size, ycsb_populate_size]() {
        return make_fresh_corpus_checkpoint(val_size, ycsb_populate_size);
      },
      [ycsb_state](auto & /*store*/) {
        // Both VMemKV (checkpoint clone) and rival backends (native snapshot clone) already
        // hand back a fresh, fully-populated instance above (see
        // make_fresh_corpus_checkpoint()'s comment; same reasoning as
        // noop_already_populated_init). Safe even though this scenario's
        // own run phase grows the corpus past ycsb_populate_size afterward: nothing ever
        // mutates the master/checkpoint itself, only this clone, so a later YCSB-E
        // construction cloning the same still-pristine source is unaffected. Deliberately
        // *not* calling store.checkpoint() here as a "defensive" no-op: checkpoint() always
        // forces a real, non-trivial-cost cycle, even on an already-pristine clone, for zero
        // benefit here.

        // NOTE: collector setup and background reorg thread are launched per-run inside the
        // benchmark body (via epoch synchronization) to handle multiple trial/warmup runs correctly.
      },
      [ycsb_state, ycsb_populate_size, sname, variant_label = variant_label(sname), value_name, val_size](
          benchmark::State &state, auto &store) {
        // NOTE: This benchmark drives its own 30s wall-clock window via the
        // `while (true)` loop below instead of google-benchmark's normal
        // iteration-count loop. We still wrap it in `for (auto _ : state)`
        // (which runs exactly once, since Iterations(1) is set at
        // registration) purely so gbench's StartKeepRunning/FinishKeepRunning
        // bracket the whole run -- otherwise `state.iterations()` stays 0 and
        // `real_time`/`items_per_second` in the aggregate JSON are always
        // 0 / Infinity (the actual per-second data still comes from the
        // YCSBTimelineCollector JSON dump either way).
        for (auto _ : state) {
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
          // thread_idx==0 only (see below): advances monotonically through kForcedTriggers, so a
          // plain local index is fine -- no other thread ever touches it.
          std::size_t next_forced_trigger_idx = 0;

          ycsb_state->ready_threads.fetch_add(1, std::memory_order_acq_rel);
          // Inside the loop: start the timeline on the very first iteration of this run epoch
          if (thread_idx == 0) {
            if (ycsb_state->start_done_epoch.load(std::memory_order_relaxed) < local_epoch) {
              while (ycsb_state->ready_threads.load(std::memory_order_acquire) < static_cast<size_t>(state.threads())) {
                std::this_thread::yield();
              }
              col->start();
              auto stats = store.get_statistics();
              col->last_recorded_checkpoint_t2.store(stats.checkpoint_count, std::memory_order_relaxed);
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

            // Thread 0: poll and record Checkpoint stats (background thread handles actual triggering)
            if (thread_idx == 0) {
              auto stats = store.get_statistics();

              // Track T2 Checkpoint
              uint64_t current_checkpoint_t2 = stats.checkpoint_count;
              uint64_t prev_t2 = col->last_recorded_checkpoint_t2.load(std::memory_order_relaxed);
              if (current_checkpoint_t2 > prev_t2) {
                uint64_t diff = current_checkpoint_t2 - prev_t2;
                col->t2_checkpoint_counts[elapsed].fetch_add(diff, std::memory_order_relaxed);
                col->last_recorded_checkpoint_t2.store(current_checkpoint_t2, std::memory_order_relaxed);
              }

              // Fire the next scheduled forced checkpoint() call once elapsed reaches its mark --
              // see kForcedTriggers' own comment for the schedule and why a late-running call is
              // deliberately allowed to push a later trigger's fire time (or cause it to be
              // skipped entirely, if the window ends first).
              if (next_forced_trigger_idx < kForcedTriggers.size() &&
                  elapsed >= kForcedTriggers[next_forced_trigger_idx].second_mark) {
                const ForcedTrigger &trigger = kForcedTriggers[next_forced_trigger_idx];
                const auto call_t0 = std::chrono::steady_clock::now();
                store.checkpoint();
                const double call_elapsed_sec =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - call_t0).count();

                // Re-derive elapsed/bucket after the call: it may have taken long enough that the
                // original `elapsed` from the top of this loop iteration is stale. Clamped since a
                // long enough call can push this past the last valid bucket.
                const auto fired_elapsed =
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - col->start_time)
                        .count();
                const int bucket = static_cast<int>(
                    std::clamp<int64_t>(fired_elapsed, 0, YCSBTimelineCollector::kDurationSeconds - 1));

                auto post_stats = store.get_statistics();
                uint64_t post_t2 = post_stats.checkpoint_count;
                uint64_t pre_force_t2 = col->last_recorded_checkpoint_t2.load(std::memory_order_relaxed);
                if (post_t2 > pre_force_t2) {
                  col->t2_forced_checkpoint_counts[bucket].fetch_add(post_t2 - pre_force_t2, std::memory_order_relaxed);
                  col->last_recorded_checkpoint_t2.store(post_t2, std::memory_order_relaxed);
                }

                col->forced_events.push_back({trigger.second_mark, bucket, "checkpoint", call_elapsed_sec});
                ++next_forced_trigger_idx;
              }
            }

            int op_choice = op_dist(rng);
            if (op_choice < 95) {
              // 95% Scan (100 items)
              uint64_t max_keys = col->next_key_index.load(std::memory_order_relaxed);
              ZipfDistribution dynamic_zipf({max_keys > 100 ? max_keys - 100 : 1, 1.0});
              uint64_t scan_start = dynamic_zipf(rng);

              size_t result_count = store.scan(make_key(scan_start),
                                               make_key(scan_start + 99),
                                               [](std::span<const std::byte>, std::span<const std::byte> value) {
                                                 benchmark::DoNotOptimize(touch_bytes(value));
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
            col->dump_json(sname, variant_label, value_name, is_ltm_mode() ? "ltm" : "in_memory");
          }
          state.SetItemsProcessed(local_ops);
        }  // for (auto _ : state)
      },
      ycsb_threads,
      false);
}

// Shared no-op init_fn for scenarios whose make_fn (make_corpus / make_fresh_corpus_checkpoint)
// already hands back a fresh, fully-populated, durably-committed instance -- there is nothing
// left to initialize.
inline constexpr auto noop_already_populated_init = [](auto & /*store*/) {};

// Insert, registered once per (store variant, value size) by register_all_benchmarks() below. In
// LTM mode this pre-populates the scenario-sized corpus first, then measures incremental inserts
// on top of it; in in-memory mode it starts from an empty store.
template <typename Holder, typename Make, typename MakeFreshCorpusCheckpoint>
static void register_insert_benchmark(Holder insert_holder,
                                      const std::string &sname,
                                      const std::string &value_name,
                                      Make make,
                                      MakeFreshCorpusCheckpoint make_fresh_corpus_checkpoint,
                                      std::size_t val_size,
                                      std::size_t corpus_size,
                                      const std::vector<int> &thread_counts) {
  const bool ltm_insert_mode = is_ltm_mode();
  auto insert_meta = make_metadata(corpus_size, val_size);
  register_bench(
      insert_holder,
      benchmark_name(sname, "Insert", std::nullopt, std::nullopt, value_name),
      insert_meta,
      // LTM mode's pre-populated starting corpus is exactly the same (val_size, corpus_size)
      // fully-populated/committed state every other scenario's shared master represents (see
      // make_fresh_corpus_checkpoint()'s comment) -- clone from it instead of independently
      // bulk_load()ing + checkpoint()ing once per thread-count registration, so that cost is paid
      // once (outside the cgroup, via run_bench.sh's priming pass) rather than once per
      // thread-count entry. Non-LTM mode keeps `make` unchanged (measures inserting into a
      // genuinely empty store).
      [make, make_fresh_corpus_checkpoint, val_size, corpus_size, ltm_insert_mode]() {
        if (ltm_insert_mode) {
          return make_fresh_corpus_checkpoint(val_size, corpus_size);
        }
        return make();
      },
      noop_already_populated_init,
      [corpus_size, val_size](benchmark::State &state, auto &store) {
        std::string dummy_large(val_size, 'a');
        std::string dummy_8b(8, 'a');
        const std::size_t insert_start = is_ltm_mode() ? corpus_size : 0;
        std::size_t threads = static_cast<std::size_t>(state.threads());
        std::size_t thread_idx = static_cast<std::size_t>(state.thread_index());
        const auto time_budget = std::chrono::duration<double>(insert_time_budget_seconds());
        for (auto _ : state) {
          const auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::duration_cast<std::chrono::steady_clock::duration>(time_budget);
          std::size_t i = 0;
          while (std::chrono::steady_clock::now() < deadline) {
            std::size_t key_index = insert_start + thread_idx + i * threads;
            bool inserted;
            if (val_size != 8 && key_index % 5 == 0) {
              inserted = store.insert(make_key(key_index), dummy_8b);
            } else {
              inserted = store.insert(make_key(key_index), dummy_large);
            }
            benchmark::DoNotOptimize(inserted);
            ++i;
          }
          state.SetItemsProcessed(i);
        }
      },
      thread_counts);
}

// Get/Hit (all Dist values) and Get/Miss, registered once per (store variant, value size) by
// register_all_benchmarks() below. Both are read-only against the shared corpus `make_corpus`
// clones.
template <typename Holder, typename MakeCorpus>
static void register_get_benchmarks(Holder crud_holder,
                                    const std::string &sname,
                                    const std::string &value_name,
                                    MakeCorpus make_corpus,
                                    std::size_t val_size,
                                    std::size_t corpus_size,
                                    const std::vector<int> &thread_counts) {
  for (const char *dist : {"Zipf", "Uniform"}) {
    auto get_hit_meta = make_metadata(corpus_size, val_size);
    register_bench(
        crud_holder,
        benchmark_name(sname, "Get", "Hit", dist, value_name),
        get_hit_meta,
        make_corpus,
        noop_already_populated_init,
        [corpus_size, dist](benchmark::State &state, auto &store) {
          std::mt19937_64 rng(kBenchmarkSeed + state.thread_index());
          ZipfDistribution zipf({corpus_size, 1.0});
          std::uniform_int_distribution<std::size_t> uniform_index(0, corpus_size - 1);
          for (auto _ : state) {
            std::size_t key_index = (std::string(dist) == "Zipf") ? zipf(rng) : uniform_index(rng);
            store.get(make_key(key_index),
                      [](std::span<const std::byte> value) { benchmark::DoNotOptimize(touch_bytes(value)); });
          }
          state.SetItemsProcessed(state.iterations());
        },
        thread_counts,
        false);
  }

  auto get_miss_meta = make_metadata(corpus_size, val_size);
  register_bench(
      crud_holder,
      benchmark_name(sname, "Get", "Miss", "Zipf", value_name),
      get_miss_meta,
      make_corpus,
      noop_already_populated_init,
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
      false);
}

// Update (stateful, against the shared corpus), registered once per (store variant, value size)
// by register_all_benchmarks() below.
template <typename Holder, typename MakeCorpus>
static void register_update_benchmark(Holder crud_holder,
                                      const std::string &sname,
                                      const std::string &value_name,
                                      MakeCorpus make_corpus,
                                      std::size_t val_size,
                                      std::size_t corpus_size,
                                      const std::vector<int> &thread_counts) {
  auto update_meta = make_metadata(corpus_size, val_size);
  register_bench(
      crud_holder,
      benchmark_name(sname, "Update", std::nullopt, "Zipf", value_name),
      update_meta,
      make_corpus,
      noop_already_populated_init,
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
      false);
}

// Delete, registered once per (store variant, value size) by register_all_benchmarks() below.
// Mutates its corpus, so (unlike Get/Update/Scan) it starts from its own fresh clone for every
// thread-count instance rather than sharing crud_holder's.
template <typename MakeFreshCorpusCheckpoint>
static void register_delete_benchmark(const std::string &sname,
                                      const std::string &value_name,
                                      MakeFreshCorpusCheckpoint make_fresh_corpus_checkpoint,
                                      std::size_t val_size,
                                      std::size_t corpus_size,
                                      const std::vector<int> &thread_counts) {
  auto delete_meta = make_metadata(corpus_size, val_size);
  register_bench(
      benchmark_name(sname, "Delete", std::nullopt, std::nullopt, value_name),
      delete_meta,
      [make_fresh_corpus_checkpoint, val_size, corpus_size]() {
        return make_fresh_corpus_checkpoint(val_size, corpus_size);
      },
      noop_already_populated_init,
      [corpus_size](benchmark::State &state, auto &store) {
        std::size_t threads = static_cast<std::size_t>(state.threads());
        std::size_t thread_idx = static_cast<std::size_t>(state.thread_index());
        const auto time_budget = std::chrono::duration<double>(delete_time_budget_seconds());
        for (auto _ : state) {
          const auto deadline = std::chrono::steady_clock::now() +
                                std::chrono::duration_cast<std::chrono::steady_clock::duration>(time_budget);
          std::size_t i = 0;
          while (std::chrono::steady_clock::now() < deadline) {
            std::size_t key_index = thread_idx + i * threads;
            if (key_index >= corpus_size) {
              break;  // this thread's slice of the corpus is exhausted
            }
            bool removed = store.remove(make_key(key_index));
            benchmark::DoNotOptimize(removed);
            ++i;
          }
          state.SetItemsProcessed(i);
        }
      },
      thread_counts);
}

// Scan (both Dist values), registered once per (store variant, value size) by
// register_all_benchmarks() below.
template <typename Make, typename MakeCorpus>
static void register_scan_benchmarks(const std::string &sname,
                                     const std::string &value_name,
                                     Make make,
                                     MakeCorpus make_corpus,
                                     std::size_t val_size,
                                     std::size_t corpus_size,
                                     const std::vector<int> &thread_counts) {
  constexpr int scan_count_reorg = 100;
  const size_t reorg_dataset_size = corpus_size;
  auto scan_holder = std::make_shared<StoreHolder<decltype(make())>>();
  for (int dist_idx = 0; dist_idx < 2; ++dist_idx) {
    const char *dist = (dist_idx == 0) ? "Zipf" : "Uniform";
    auto scan_meta = make_metadata(reorg_dataset_size, val_size);
    register_bench(
        scan_holder,
        benchmark_name(sname, "Scan", std::nullopt, dist, value_name),
        scan_meta,
        make_corpus,
        noop_already_populated_init,
        [reorg_dataset_size, dist](benchmark::State &state, auto &store) {
          std::mt19937_64 rng(kBenchmarkSeed + state.thread_index());
          const std::size_t scan_span = reorg_dataset_size > static_cast<std::size_t>(scan_count_reorg)
                                            ? reorg_dataset_size - static_cast<std::size_t>(scan_count_reorg)
                                            : 1;
          ZipfDistribution zipf({scan_span, 1.0});
          std::uniform_int_distribution<std::size_t> uniform_index(0, scan_span - 1);
          for (auto _ : state) {
            std::size_t scan_start = (std::string(dist) == "Zipf") ? zipf(rng) : uniform_index(rng);
            size_t result_count = store.scan(make_key(scan_start),
                                             make_key(scan_start + scan_count_reorg - 1),
                                             [](std::span<const std::byte>, std::span<const std::byte> value) {
                                               benchmark::DoNotOptimize(touch_bytes(value));
                                             });
            benchmark::DoNotOptimize(result_count);
          }
          state.SetItemsProcessed(state.iterations());
        },
        thread_counts,
        false);
  }
}

// =============================================================================
// Benchmark definitions
// =============================================================================
void register_all_benchmarks() {
  int hw_threads = static_cast<int>(std::thread::hardware_concurrency());

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

  for_each_store_variant([&](const char *name, auto make, auto make_fresh_corpus_checkpoint) {
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
      const size_t corpus_size = corpus_size_for_value(val_size);
      // Bound to this val_size: constructs at the shared, checkpoint-reusable corpus path
      // (see for_each_store_variant()). Used by every scenario below that shares this
      // read-mostly corpus (Get/Update/YCSB-E/Scan) instead of `make`.
      auto make_corpus = [make_fresh_corpus_checkpoint, val_size, corpus_size]() {
        return make_fresh_corpus_checkpoint(val_size, corpus_size);
      };
      auto crud_holder = std::make_shared<StoreHolder<decltype(make())>>();
      auto insert_holder = std::make_shared<StoreHolder<decltype(make())>>();
      // YCSB-E gets its own holder/path rather than sharing crud_holder's: its own run phase
      // legitimately inserts new keys past corpus_size (a 5% insert mix), which would leave a
      // *live, in-process* corpus with extra, unexpected keys if crud_holder's instance were
      // reused afterward -- and could make Get/Miss's "these indices don't exist" assumption
      // false. A separate holder means YCSB-E always clones its own fresh instance from the
      // shared master (see make_fresh_corpus_checkpoint()'s comment) instead of ever risking a
      // reuse of an instance it (or something else) already grew.
      auto ycsb_holder = std::make_shared<StoreHolder<decltype(make())>>();

      // 1. Insert
      register_insert_benchmark(
          insert_holder, sname, value_name, make, make_fresh_corpus_checkpoint, val_size, corpus_size, thread_counts);

      // 2. Get/Hit (all Dist values) and 3. Get/Miss (both read-only: shared corpus)
      register_get_benchmarks(crud_holder, sname, value_name, make_corpus, val_size, corpus_size, thread_counts);

      // 4. Update (Stateful: shared corpus)
      register_update_benchmark(crud_holder, sname, value_name, make_corpus, val_size, corpus_size, thread_counts);

      // 5. YCSB-E Benchmark (Short Range Scans, 30s mixed workload, hw_threads threads max)
      register_ycsb_e_benchmark(
          ycsb_holder, hw_threads, sname, value_name, make_fresh_corpus_checkpoint, val_size, corpus_size);

      // 6. Delete (mutates its corpus, so gets a fresh clone per thread-count instance -- kept in
      // both scenarios so the T1 path remains comparable against RocksDB)
      register_delete_benchmark(sname, value_name, make_fresh_corpus_checkpoint, val_size, corpus_size, thread_counts);

      // 7. Scan (both Dist values), over the same shared corpus as Get/Update -- see
      // make_fresh_corpus_checkpoint()'s comment for why no separate Scan-only master is needed.
      register_scan_benchmarks(sname, value_name, make, make_corpus, val_size, corpus_size, thread_counts);
    }
  });
}

// ─── Reorg-scaling probe (standalone CLI mode, bypasses Google Benchmark) ───────────────────
//
// Measures how long a single reorganize()/checkpoint() call takes as a function of corpus size,
// for T1-only vs T1+T2 modes. Google Benchmark's own registration model assumes the same
// operation repeats many times to build a statistic; here we want to time exactly one, possibly
// very slow, blocking call and be able to tell a driver script "this is taking too long" without
// waiting indefinitely -- hence a standalone CLI mode instead of a registered benchmark case.
//
// Population/checkpoint/churn are deliberately NOT time-limited here -- only the outer shell
// driver's own generous backstop timeout (wrapping this whole process) covers them. Only the
// reorganize()/checkpoint() call itself is capped (timed_run(), below), via a background thread
// plus future::wait_for(), so a runaway call is reported on its own without also charging setup
// time against the same budget (conflating the two would make a timeout ambiguous: slow setup, or
// slow reorganize/checkpoint?).
//
// Four modes, selected via --mode:
//   t1only / t1t2 (run_bootstrap(), below): a single fresh populate followed by exactly one timed
//     reorganize()/checkpoint() call.
//   t1t2_steady (run_steady(), below): measures a *second* (or later) checkpoint() call against a
//     corpus that already has one checkpointed generation. No automated driver script currently
//     invokes this mode (its own sweep script was retired in favor of background_job_probe below)
//     -- reachable directly via this CLI for ad-hoc use.
//   background_job_probe (run_background_job_probe(), below): fixed 10,000,000-record corpus;
//     measures one reorganize()/checkpoint() call's own duration plus the QPS degradation it
//     causes to concurrent Insert/Update/Scan workloads -- see run_background_jobs_probe.sh. This
//     is a forced, whole-store operation (every shard synchronously merged in one call) -- not
//     what happens during ordinary operation, where ShardedT1Index's own background workers
//     maintain each shard independently and incrementally. See organic_split_probe below for that.
//   organic_split_probe (run_organic_split_probe(), below): starts from an empty store with real
//     background workers running (not the forced calls above) and inserts continuously, polling
//     for each organic per-shard split as it completes and comparing Insert QPS just before it to
//     Insert QPS during it -- see run_organic_split_probe.sh.
namespace reorg_probe {

constexpr int kReorgTimeoutSecondsDefault = 60;

// Overridable so a single-call reorganize()/checkpoint() timeout can be tightened for
// a specific experiment without changing the default used everywhere else this timeout applies.
static inline int reorg_timeout_seconds() {
  if (const char *override_seconds = std::getenv("VMEMKV_BENCH_REORG_TIMEOUT_SECONDS")) {
    char *end = nullptr;
    const long parsed = std::strtol(override_seconds, &end, 10);
    if (end != override_seconds && *end == '\0' && parsed > 0) {
      return static_cast<int>(parsed);
    }
  }
  return kReorgTimeoutSecondsDefault;
}

enum class ProbeMode {
  kT1Only,
  kT1T2,
  kT1T2Steady,
  kBackgroundJobProbe,
  kOrganicSplitProbe,
};

struct ProbeArgs {
  bool is_ltm = false;
  std::size_t val_size = 0;
  ProbeMode mode = ProbeMode::kT1Only;
  double ratio = 1.0;
  double churn_ratio = 0.0;
  std::string sweep_tag = "default";
  std::string job;  // "reorganize" | "checkpoint" -- only read by kBackgroundJobProbe.
};

[[noreturn]] void fail(const std::string &msg) {
  std::cerr << "[reorg-probe] " << msg << std::endl;
  std::exit(1);
}

auto parse_args(int argc, char **argv) -> ProbeArgs {
  ProbeArgs args;
  bool has_scenario = false;
  bool has_value_size = false;
  bool has_mode = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--reorg-probe") {
      continue;
    }
    const auto eq = arg.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    const std::string_view key = arg.substr(0, eq);
    const std::string_view value = arg.substr(eq + 1);
    if (key == "--scenario") {
      if (value == "ltm") {
        args.is_ltm = true;
      } else if (value == "in_memory") {
        args.is_ltm = false;
      } else {
        fail("unknown --scenario: " + std::string(value));
      }
      has_scenario = true;
    } else if (key == "--value-size") {
      if (value == "8B") {
        args.val_size = 8;
      } else if (value == "1KB") {
        args.val_size = 1024;
      } else if (value == "64KB") {
        args.val_size = 65536;
      } else {
        fail("unknown --value-size: " + std::string(value));
      }
      has_value_size = true;
    } else if (key == "--mode") {
      if (value == "t1only") {
        args.mode = ProbeMode::kT1Only;
      } else if (value == "t1t2") {
        args.mode = ProbeMode::kT1T2;
      } else if (value == "t1t2_steady") {
        args.mode = ProbeMode::kT1T2Steady;
      } else if (value == "background_job_probe") {
        args.mode = ProbeMode::kBackgroundJobProbe;
      } else if (value == "organic_split_probe") {
        args.mode = ProbeMode::kOrganicSplitProbe;
      } else {
        fail("unknown --mode: " + std::string(value));
      }
      has_mode = true;
    } else if (key == "--ratio") {
      args.ratio = std::stod(std::string(value));
    } else if (key == "--churn-ratio") {
      args.churn_ratio = std::stod(std::string(value));
    } else if (key == "--sweep-tag") {
      // Only read by run_steady() (t1t2_steady mode) -- every other mode ignores this value.
      args.sweep_tag = std::string(value);
    } else if (key == "--job") {
      // Only read by kBackgroundJobProbe.
      args.job = std::string(value);
    }
  }
  if (!has_scenario || !has_value_size || !has_mode) {
    fail(
        "usage: --reorg-probe --scenario=<in_memory|ltm> --value-size=<8B|1KB|64KB> "
        "--mode=<t1only|t1t2|t1t2_steady|background_job_probe|organic_split_probe> "
        "--ratio=<0.0-1.0> [--churn-ratio=<0.0-1.0>] [--sweep-tag=<name>] "
        "[--job=<reorganize|checkpoint>]");
  }
  if (args.mode == ProbeMode::kBackgroundJobProbe && args.job != "reorganize" && args.job != "checkpoint") {
    fail("--mode=background_job_probe requires --job=<reorganize|checkpoint>");
  }
  if (args.ratio <= 0.0 || args.ratio > 1.0) {
    fail("--ratio must be in (0.0, 1.0]");
  }
  if (args.churn_ratio < 0.0 || args.churn_ratio > 1.0) {
    fail("--churn-ratio must be in [0.0, 1.0]");
  }
  return args;
}

// Times a single call to `fn` (reorganize()/checkpoint()) in a detached background thread capped
// at reorg_timeout_seconds() -- see the file-level comment above this namespace for why
// google-benchmark's iteration model doesn't fit timing exactly one, possibly very slow, blocking
// call.
template <typename Fn>
auto timed_run(Fn &&fn) -> std::pair<double, bool> {
  const int timeout_seconds = reorg_timeout_seconds();
  std::promise<void> done_promise;
  auto done_future = done_promise.get_future();
  const auto t0 = std::chrono::steady_clock::now();
  std::thread worker([fn = std::forward<Fn>(fn), promise = std::move(done_promise)]() mutable {
    fn();
    promise.set_value();
  });
  worker.detach();

  const auto status = done_future.wait_for(std::chrono::seconds(timeout_seconds));
  const bool timed_out = status != std::future_status::ready;
  const double elapsed_sec = timed_out ? static_cast<double>(timeout_seconds)
                                       : std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return {elapsed_sec, timed_out};
}

// _Exit(), not return: on timeout, timed_run()'s worker thread is still inside reorganize()/
// checkpoint() touching `store` -- abandoning it via detach() and terminating without running
// destructors (skipping `store`'s ~VMemKVImpl(), which would otherwise join reorg_worker_ and
// could itself block forever) is what makes this process's exit actually bounded. On success,
// _Exit() is just the simplest way to avoid the same destructor path racing the
// now-finished-but-still-detached worker thread.
[[noreturn]] void report_and_exit(const ProbeArgs &args, std::size_t key_count, double elapsed_sec, bool timed_out) {
  const char *mode_name = "t1t2_steady";
  switch (args.mode) {
    case ProbeMode::kT1Only:
      mode_name = "t1only";
      break;
    case ProbeMode::kT1T2:
      mode_name = "t1t2";
      break;
    case ProbeMode::kT1T2Steady:
      mode_name = "t1t2_steady";
      break;
    case ProbeMode::kBackgroundJobProbe:
      mode_name = "background_job_probe";  // Unreachable: this mode reports via its own print, below.
      break;
    case ProbeMode::kOrganicSplitProbe:
      mode_name = "organic_split_probe";  // Unreachable: this mode reports via its own print, below.
      break;
  }
  // churn_ratio only means anything for t1t2_steady (run_steady() is the only mode that applies
  // it) -- every other mode never touches args.churn_ratio, so emitting its default 0.0 there
  // would misleadingly read as "measured with zero churn" rather than "not applicable."
  const bool churn_ratio_applicable = args.mode == ProbeMode::kT1T2Steady;
  std::cout << "{\"scenario\":\"" << (args.is_ltm ? "ltm" : "in_memory") << "\"," << "\"value_size\":" << args.val_size
            << "," << "\"mode\":\"" << mode_name << "\"," << "\"ratio\":" << args.ratio << ","
            << "\"churn_ratio\":" << (churn_ratio_applicable ? std::to_string(args.churn_ratio) : "null") << ","
            << "\"key_count\":" << key_count << "," << "\"elapsed_sec\":" << elapsed_sec << ","
            << "\"timed_out\":" << (timed_out ? "true" : "false") << "}" << std::endl;
  std::_Exit(timed_out ? 124 : 0);
}

static std::string reorg_probe_path(const ProbeArgs &args, const std::string &suffix) {
  return get_db_dir() + "/reorg_probe_" + (args.is_ltm ? "ltm" : "inmem") + "_" + std::to_string(args.val_size) + "_" +
         suffix;
}

static std::size_t resolve_writer_threads() {
  return std::min<std::size_t>({std::max(1u, std::thread::hardware_concurrency()), 32});
}

// Samples `churn_count` random indices in [0, key_count) and update()s each, sharded across up
// to 16 threads. Used by run_steady()'s kUpdateCold pre-churn.
static void apply_random_churn(vmemkv::variants::VMemKVStore &store,
                               std::size_t key_count,
                               std::size_t churn_count,
                               uint64_t seed,
                               std::size_t val_size) {
  std::mt19937_64 churn_rng(seed);
  std::uniform_int_distribution<std::size_t> churn_index_dist(0, key_count - 1);
  std::vector<std::size_t> churn_indices(churn_count);
  for (auto &idx : churn_indices) {
    idx = churn_index_dist(churn_rng);
  }
  const std::size_t churn_threads =
      std::min<std::size_t>({std::max(1u, std::thread::hardware_concurrency()), std::size_t{16}, churn_count});
  std::vector<std::thread> workers;
  workers.reserve(churn_threads);
  for (std::size_t t = 0; t < churn_threads; ++t) {
    workers.emplace_back([&store, &churn_indices, val_size, t, churn_threads]() {
      for (std::size_t i = t; i < churn_indices.size(); i += churn_threads) {
        const std::size_t idx = churn_indices[i];
        store.update(make_key(idx), make_value_for_key(idx, val_size));
      }
    });
  }
  for (auto &worker : workers) {
    worker.join();
  }
}

// T1-only / T1+T2 bootstrap modes: a single fresh populate (scattered insert order, see
// populate_random_order()'s comment) followed by exactly one timed reorganize()/checkpoint()
// call, against a corpus with no prior checkpoint.
[[noreturn]] void run_bootstrap(const ProbeArgs &args) {
  using Store = vmemkv::variants::VMemKVStore;

  const std::size_t full_key_count = corpus_size_for_value(args.val_size);
  const std::size_t key_count = std::max<std::size_t>(1, static_cast<std::size_t>(full_key_count * args.ratio));
  const bool force_checkpoint = args.mode == ProbeMode::kT1T2;

  const std::string path = reorg_probe_path(
      args,
      (force_checkpoint ? "t1t2" : "t1only") + std::string("_") + std::to_string(static_cast<int>(args.ratio * 100)));

  auto store = make_vmemkv_fresh(
      path, [&path]() { return std::make_unique<Store>(path, Store::ConfigType::DefaultT2CapacityBytes); });
  populate_random_order(*store, {key_count, args.val_size});

  auto [elapsed_sec, timed_out] = timed_run([&store, force_checkpoint]() {
    if (force_checkpoint) {
      store->checkpoint();
    } else {
      store->reorganize();
    }
  });
  report_and_exit(args, key_count, elapsed_sec, timed_out);
}

// Steady-state mode: measures a *second* (or later) checkpoint() call, after the corpus already
// has one checkpointed generation -- unlike run_bootstrap() above, which always measures a
// from-scratch corpus's first checkpoint.
//
// Persists across invocations at a fixed, ratio/churn-independent path (keyed only by
// --sweep-tag/scenario/value-size) so a driver script can call this repeatedly -- with an
// ascending --ratio sequence ("Corpus-Size Invariance across Generations") or a fixed --ratio and
// varying --churn-ratio ("Churn-Ratio Scaling") -- and have each call reuse/grow the *same*
// on-disk corpus instead of re-populating it from scratch every time: populating 25/50/75/100%
// independently costs 250% of a full corpus in total insert work, growing incrementally costs
// 100%. A sidecar "<path>.steady_count" file tracks how many keys are already durably populated
// at `path` so this process (a fresh one each invocation, same as the bootstrap modes) knows how
// much delta to load.
[[noreturn]] void run_steady(const ProbeArgs &args) {
  using Store = vmemkv::variants::VMemKVStore;

  const std::size_t full_key_count = corpus_size_for_value(args.val_size);
  const std::size_t key_count = std::max<std::size_t>(1, static_cast<std::size_t>(full_key_count * args.ratio));

  const std::string path = get_db_dir() + "/reorg_probe_steady_" + args.sweep_tag + "_" +
                           (args.is_ltm ? "ltm" : "inmem") + "_" + std::to_string(args.val_size);
  const std::string count_marker_path = path + ".steady_count";

  const bool manifest_exists = std::filesystem::exists(vmemkv::derive_manifest_path(path));
  std::size_t prev_count = 0;
  if (manifest_exists) {
    if (std::ifstream marker(count_marker_path); marker) {
      marker >> prev_count;
    }
  }

  std::unique_ptr<Store> store;
  if (manifest_exists) {
    store = std::make_unique<Store>(path, Store::ConfigType::DefaultT2CapacityBytes);
  } else {
    store = make_vmemkv_fresh(
        path, [&path]() { return std::make_unique<Store>(path, Store::ConfigType::DefaultT2CapacityBytes); });
  }

  if (key_count > prev_count) {
    const std::size_t delta = key_count - prev_count;
    store->bulk_load(
        delta,
        [prev_count](std::size_t i) { return make_key(prev_count + i); },
        [prev_count, val_size = args.val_size](std::size_t i) { return make_value_for_key(prev_count + i, val_size); });
  }

  // Untimed: establishes a clean single-generation baseline before churn, same as any real
  // caller would do -- not part of what this experiment measures.
  store->checkpoint();

  // Untimed, and deliberately multi-threaded: update() is WAL-durable (an fsync-equivalent wait
  // per call, see update_impl()'s comment), so a single sequential thread issuing hundreds of
  // thousands of them serializes on fsync latency alone -- observed locally taking >300s for
  // 400K updates regardless of corpus size or churn ratio, i.e. a property of this churn-
  // application loop, not of the checkpoint() call being measured. Splitting across threads lets
  // concurrent WAL appends benefit from group commit (see wal.cpp) the same way a real concurrent
  // write workload would.
  const std::size_t churn_count = std::max<std::size_t>(1, static_cast<std::size_t>(key_count * args.churn_ratio));
  apply_random_churn(
      *store,
      key_count,
      churn_count,
      kBenchmarkSeed + static_cast<uint64_t>(args.ratio * 1000) + static_cast<uint64_t>(args.churn_ratio * 1000000),
      args.val_size);

  auto [elapsed_sec, timed_out] = timed_run([&store]() { store->checkpoint(); });

  if (!timed_out) {
    std::ofstream marker(count_marker_path, std::ios::trunc);
    marker << key_count;
  }

  report_and_exit(args, key_count, elapsed_sec, timed_out);
}

// Background-job probe: for a fixed corpus (args.val_size, kBackgroundJobProbeKeyCount records --
// in-memory unconstrained, or LTM cgroup-constrained by the *caller* script, this binary itself
// applies no memory limit), measures one call to reorganize()/checkpoint() (args.job) and, for
// each of three concurrent workloads (Insert, Update, Scan), the percentage QPS degradation that
// workload suffers while the job call is in flight. See run_background_jobs_probe.sh.
//
// Each workload gets its own paired isolated/concurrent measurement, both windows sized to the
// SAME wall-clock duration (the job call's own measured elapsed time). This is a deliberate fix
// over an earlier design (run_contention_probe(), removed) whose isolated phase ran a fixed
// op-count per thread regardless of how long that took, while the concurrent phase's window was
// set by the job's own (often very different) duration -- comparing TPS across two differently-
// sized windows is not a fair measurement. Here, phase B (concurrent) runs the job once while
// workload threads hammer continuously (stop-flag controlled) and measures its own elapsed time;
// phase A (isolated) then runs the identical thread/workload setup for exactly that same elapsed
// time (timer-controlled, no concurrent job) as the baseline.
enum class BackgroundJobWorkload { kInsert, kUpdate, kScan };

// kInsert draws from `next_fresh_key` (shared across a phase's threads, never reused within one
// measure_workload_degradation() call); kUpdate/kScan pick a uniformly random existing key.
// kScan issues a small (100-key) ranged scan, mirroring the main Google Benchmark-registered
// Op=Scan implementation above.
template <typename Store>
static void background_job_do_one_op(Store &store,
                                     uint32_t val_size,
                                     BackgroundJobWorkload workload,
                                     std::mt19937_64 &rng,
                                     std::uniform_int_distribution<std::size_t> &key_dist,
                                     std::atomic<std::size_t> &next_fresh_key) {
  switch (workload) {
    case BackgroundJobWorkload::kInsert: {
      const std::size_t idx = next_fresh_key.fetch_add(1, std::memory_order_relaxed);
      store->insert(make_key(idx), make_value_for_key(idx, val_size));
      return;
    }
    case BackgroundJobWorkload::kUpdate: {
      const std::size_t idx = key_dist(rng);
      store->update(make_key(idx), make_value_for_key(idx, val_size));
      return;
    }
    case BackgroundJobWorkload::kScan: {
      const std::size_t start = key_dist(rng);
      std::size_t result_count = store->scan(
          make_key(start), make_key(start + 99), [](std::span<const std::byte>, std::span<const std::byte> value) {
            benchmark::DoNotOptimize(touch_bytes(value));
          });
      benchmark::DoNotOptimize(result_count);
      return;
    }
  }
}

struct WorkloadDegradation {
  double job_elapsed_sec = 0.0;
  double degradation_pct = 0.0;
  bool timed_out = false;
};

template <typename Store, typename JobFn>
static auto measure_workload_degradation(Store &store,
                                         std::size_t key_count,
                                         uint32_t val_size,
                                         BackgroundJobWorkload workload,
                                         std::size_t thread_count,
                                         JobFn &&run_job) -> WorkloadDegradation {
  // Phase B: concurrent -- workload threads run continuously while the job executes once.
  std::atomic<std::size_t> next_fresh_key_b{key_count};
  std::atomic<bool> stop_b{false};
  std::vector<std::thread> workers_b;
  std::vector<std::size_t> counts_b(thread_count, 0);
  workers_b.reserve(thread_count);
  for (std::size_t t = 0; t < thread_count; ++t) {
    workers_b.emplace_back([&, t]() {
      std::mt19937_64 rng(kBenchmarkSeed + 9000 + t);
      std::uniform_int_distribution<std::size_t> key_dist(0, key_count > 100 ? key_count - 100 : 1);
      std::size_t done = 0;
      while (!stop_b.load(std::memory_order_relaxed)) {
        background_job_do_one_op(store, val_size, workload, rng, key_dist, next_fresh_key_b);
        ++done;
      }
      counts_b[t] = done;
    });
  }
  auto [job_elapsed_sec, timed_out] = timed_run(std::forward<JobFn>(run_job));
  stop_b.store(true, std::memory_order_relaxed);
  for (auto &w : workers_b) {
    w.join();
  }
  if (timed_out || job_elapsed_sec <= 0.0) {
    return {job_elapsed_sec, 0.0, timed_out};
  }
  std::size_t concurrent_ops = 0;
  for (auto c : counts_b) {
    concurrent_ops += c;
  }
  const double concurrent_tps = static_cast<double>(concurrent_ops) / job_elapsed_sec;

  // Phase A: isolated -- same setup, run for exactly job_elapsed_sec (timer-controlled), not a
  // fixed op count -- matches Phase B's own window so the two TPS figures are comparable.
  std::atomic<std::size_t> next_fresh_key_a{key_count};
  std::atomic<bool> stop_a{false};
  std::vector<std::thread> workers_a;
  std::vector<std::size_t> counts_a(thread_count, 0);
  workers_a.reserve(thread_count);
  for (std::size_t t = 0; t < thread_count; ++t) {
    workers_a.emplace_back([&, t]() {
      std::mt19937_64 rng(kBenchmarkSeed + 5000 + t);
      std::uniform_int_distribution<std::size_t> key_dist(0, key_count > 100 ? key_count - 100 : 1);
      std::size_t done = 0;
      while (!stop_a.load(std::memory_order_relaxed)) {
        background_job_do_one_op(store, val_size, workload, rng, key_dist, next_fresh_key_a);
        ++done;
      }
      counts_a[t] = done;
    });
  }
  std::this_thread::sleep_for(std::chrono::duration<double>(job_elapsed_sec));
  stop_a.store(true, std::memory_order_relaxed);
  for (auto &w : workers_a) {
    w.join();
  }
  std::size_t isolated_ops = 0;
  for (auto c : counts_a) {
    isolated_ops += c;
  }
  const double isolated_tps = static_cast<double>(isolated_ops) / job_elapsed_sec;
  const double degradation_pct = isolated_tps > 0.0 ? (1.0 - concurrent_tps / isolated_tps) * 100.0 : 0.0;
  return {job_elapsed_sec, degradation_pct, false};
}

// Fixed corpus size -- a single reproducible reference point (see this file's own doc comment
// above) rather than a sweep across the matrix's 4 scenario/value-size combos.
constexpr std::size_t kBackgroundJobProbeKeyCount = 10'000'000;

[[noreturn]] void run_background_job_probe(const ProbeArgs &args) {
  using Store = vmemkv::variants::VMemKVStore;

  const std::size_t key_count = kBackgroundJobProbeKeyCount;
  const std::string path = reorg_probe_path(args, "backgroundjob_" + args.job);
  const std::size_t resolved_writer_threads = resolve_writer_threads();

  // Suppressed during setup only (population + initial checkpoint): an auto-triggered
  // reorganize/checkpoint mid-populate would pollute the measurement below with exactly the
  // cost this probe exists to isolate.
  setenv("VMEMKV_SUPPRESS_AUTO_REORG", "1", 1);
  auto store = make_vmemkv_fresh(
      path, [&path]() { return std::make_unique<Store>(path, Store::ConfigType::DefaultT2CapacityBytes); });
  populate_random_order(*store, {key_count, args.val_size});
  store->checkpoint();
  unsetenv("VMEMKV_SUPPRESS_AUTO_REORG");

  std::function<void()> run_job;
  if (args.job == "reorganize") {
    run_job = [&store]() { store->reorganize(); };
  } else {
    run_job = [&store]() { store->checkpoint(); };
  }

  bool any_timed_out = false;
  double reported_elapsed_sec = 0.0;
  std::optional<double> insert_pct;
  std::optional<double> update_pct;
  std::optional<double> scan_pct;
  const std::array<std::pair<BackgroundJobWorkload, std::optional<double> *>, 3> workloads{{
      {BackgroundJobWorkload::kInsert, &insert_pct},
      {BackgroundJobWorkload::kUpdate, &update_pct},
      {BackgroundJobWorkload::kScan, &scan_pct},
  }};
  for (const auto &[workload, out_pct] : workloads) {
    auto result =
        measure_workload_degradation(store, key_count, args.val_size, workload, resolved_writer_threads, run_job);
    reported_elapsed_sec = result.job_elapsed_sec;
    if (result.timed_out) {
      any_timed_out = true;
      break;
    }
    *out_pct = result.degradation_pct;
  }

  auto pct_json = [](std::optional<double> v) -> std::string { return v.has_value() ? std::to_string(*v) : "null"; };
  // Phase breakdown: only meaningful for job=="checkpoint" (reorganize()'s T1Only path never runs
  // checkpoint_internal(), so these fields would just be stale/zero) -- last_checkpoint_* reflects
  // whichever of the 3 measure_workload_degradation() calls above most recently ran checkpoint(),
  // representative since corpus size barely changes call to call.
  std::optional<double> t1_reorganize_ms;
  std::optional<double> wal_rotate_ms;
  std::optional<double> msync_ms;
  if (!any_timed_out && args.job == "checkpoint") {
    const auto stats = store->get_statistics();
    t1_reorganize_ms = stats.last_checkpoint_t1_reorganize_duration_us / 1000.0;
    wal_rotate_ms = stats.last_checkpoint_wal_rotate_duration_us / 1000.0;
    msync_ms = stats.last_checkpoint_msync_duration_us / 1000.0;
  }
  std::cout << "{\"job\":\"" << args.job << "\"," << "\"scenario\":\"" << (args.is_ltm ? "ltm" : "in_memory") << "\","
            << "\"value_size\":" << args.val_size << "," << "\"key_count\":" << key_count << ","
            << "\"writer_threads\":" << resolved_writer_threads << "," << "\"job_elapsed_sec\":" << reported_elapsed_sec
            << "," << "\"insert_degradation_pct\":" << pct_json(insert_pct) << ","
            << "\"update_degradation_pct\":" << pct_json(update_pct) << ","
            << "\"scan_degradation_pct\":" << pct_json(scan_pct) << ","
            << "\"t1_reorganize_phase_ms\":" << pct_json(t1_reorganize_ms) << ","
            << "\"wal_rotate_phase_ms\":" << pct_json(wal_rotate_ms) << ","
            << "\"msync_phase_ms\":" << pct_json(msync_ms) << ","
            << "\"timed_out\":" << (any_timed_out ? "true" : "false") << "}" << std::endl;
  std::_Exit(any_timed_out ? 124 : 0);
}

// Organic per-shard split probe: unlike run_background_job_probe() above (a forced, whole-store
// reorganize()/checkpoint() call), this measures the Insert-QPS impact of ShardedT1Index's own
// automatic background splitting under sustained write load -- the maintenance path that actually
// runs during ordinary operation. Starts from an empty store with real background workers active
// and inserts continuously with monotonically increasing keys (so essentially all new writes land
// in whichever shard currently owns the tail of the keyspace, the same access pattern
// harness_new_incremental_scaling.cpp used to confirm one shard's own split cost stays flat
// regardless of total corpus size -- see docs/t1_sharding_design.md), polling
// get_statistics().t1_split_count at a fine interval to detect each split as it completes. For
// every observed split, compares this event's own local Insert QPS just before it (not a single
// global average -- steady-state QPS drifts slowly as the corpus grows, which would bias a global
// baseline against later events) to Insert QPS in the window overlapping the split.
constexpr int kOrganicSplitProbeDurationSec = 90;
constexpr auto kOrganicSplitPollInterval = std::chrono::milliseconds(100);
// Wide enough to fully cover one shard's own reorganize/split cost (observed flat at ~0.6-1.0s
// regardless of corpus size, see docs/t1_sharding_design.md) without averaging too much
// surrounding steady-state activity into the "during" window.
constexpr double kOrganicSplitWindowSec = 1.2;

[[noreturn]] void run_organic_split_probe(const ProbeArgs &args) {
  using Store = vmemkv::variants::VMemKVStore;

  const std::string path = reorg_probe_path(args, "organic_split");
  const std::size_t writer_threads = resolve_writer_threads();

  auto store = make_vmemkv_fresh(
      path, [&path]() { return std::make_unique<Store>(path, Store::ConfigType::DefaultT2CapacityBytes); });

  std::atomic<std::size_t> next_key{0};
  std::atomic<bool> stop{false};
  std::vector<std::thread> workers;
  workers.reserve(writer_threads);
  for (std::size_t t = 0; t < writer_threads; ++t) {
    workers.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        const std::size_t idx = next_key.fetch_add(1, std::memory_order_relaxed);
        store->insert(make_key(idx), make_value_for_key(idx, args.val_size));
      }
    });
  }

  struct Sample {
    double t_sec;
    std::size_t inserted;
    uint64_t split_count;
  };
  std::vector<Sample> samples;
  const auto t0 = std::chrono::steady_clock::now();
  auto elapsed_sec = [&]() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
  while (elapsed_sec() < kOrganicSplitProbeDurationSec) {
    const auto stats = store->get_statistics();
    samples.push_back({elapsed_sec(), next_key.load(std::memory_order_relaxed), stats.t1_split_count});
    std::this_thread::sleep_for(kOrganicSplitPollInterval);
  }
  stop.store(true, std::memory_order_relaxed);
  for (auto &w : workers) {
    w.join();
  }
  const uint64_t final_split_count = store->get_statistics().t1_split_count;

  // Nearest sample at or before `t_sec` (clamped to the first sample if `t_sec` predates the run).
  auto sample_at_or_before = [&](double t_sec) -> const Sample & {
    auto it = std::upper_bound(
        samples.begin(), samples.end(), t_sec, [](double t, const Sample &s) { return t < s.t_sec; });
    if (it == samples.begin()) {
      return samples.front();
    }
    return *std::prev(it);
  };
  auto qps_between = [&](double from_sec, double to_sec) -> std::optional<double> {
    if (from_sec < samples.front().t_sec) {
      return std::nullopt;
    }
    const Sample &a = sample_at_or_before(from_sec);
    const Sample &b = sample_at_or_before(to_sec);
    const double dt = b.t_sec - a.t_sec;
    if (dt <= 0.0 || b.inserted <= a.inserted) {
      return std::nullopt;
    }
    return static_cast<double>(b.inserted - a.inserted) / dt;
  };

  struct SplitEvent {
    double at_sec;
    uint64_t shard_count_after;
    double baseline_qps;
    double during_qps;
  };
  std::vector<SplitEvent> events;
  for (std::size_t i = 1; i < samples.size(); ++i) {
    if (samples[i].split_count <= samples[i - 1].split_count) {
      continue;
    }
    const double at = samples[i].t_sec;
    const auto baseline = qps_between(at - kOrganicSplitWindowSec, at);
    const auto during = qps_between(at, std::min(at + kOrganicSplitWindowSec, samples.back().t_sec));
    if (!baseline.has_value() || !during.has_value() || *baseline <= 0.0) {
      continue;
    }
    events.push_back({at, samples[i].split_count + 1, *baseline, *during});
  }

  std::cout << "{\"job\":\"organic_split\",\"scenario\":\"" << (args.is_ltm ? "ltm" : "in_memory") << "\","
            << "\"value_size\":" << args.val_size << ",\"writer_threads\":" << writer_threads << ","
            << "\"duration_sec\":" << kOrganicSplitProbeDurationSec << ","
            << "\"total_inserted\":" << next_key.load(std::memory_order_relaxed) << ","
            << "\"final_shard_count\":" << (final_split_count + 1) << ",\"splits\":[";
  for (std::size_t i = 0; i < events.size(); ++i) {
    const auto &e = events[i];
    const double degradation_pct = (1.0 - e.during_qps / e.baseline_qps) * 100.0;
    std::cout << (i > 0 ? "," : "") << "{\"at_sec\":" << e.at_sec << ",\"shard_count_after\":" << e.shard_count_after
              << ",\"baseline_qps\":" << e.baseline_qps << ",\"during_qps\":" << e.during_qps
              << ",\"degradation_pct\":" << degradation_pct << "}";
  }
  std::cout << "],\"timed_out\":false}" << std::endl;
  std::_Exit(0);
}

[[noreturn]] void run(const ProbeArgs &args) {
  if (args.is_ltm) {
    // Matches benchmark_matrix.sh's real scenario_env_prefix() for "ltm" -- corpus_size_for_value()
    // below only scales with VMEMKV_BENCH_TARGET_RATIO when VMEMKV_BENCH_LTM is set (see that
    // function's in-memory fixed-constant branches), so both must be set before it's first called.
    // overwrite=0 on the ratio: this is a default matching benchmark_matrix.sh's own "ltm"
    // scenario, not a mandate -- a caller (e.g. a ratio sweep) that already exported
    // VMEMKV_BENCH_TARGET_RATIO before invoking this binary must win.
    ::setenv("VMEMKV_BENCH_LTM", "1", 1);
    ::setenv("VMEMKV_BENCH_TARGET_RATIO", "8.0", 0);
  }
  if (args.mode == ProbeMode::kT1T2Steady) {
    run_steady(args);
  } else if (args.mode == ProbeMode::kBackgroundJobProbe) {
    run_background_job_probe(args);
  } else if (args.mode == ProbeMode::kOrganicSplitProbe) {
    run_organic_split_probe(args);
  } else {
    run_bootstrap(args);
  }
}

}  // namespace reorg_probe

int main(int argc, char **argv) {
  std::cout.setf(std::ios::unitbuf);
  for (int i = 1; i < argc; ++i) {
    if (std::string_view(argv[i]) == "--reorg-probe") {
      reorg_probe::run(reorg_probe::parse_args(argc, argv));
    }
  }
  if (!should_skip_cleanup()) {
    cleanup_stale_benchmark_files();
  }
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
