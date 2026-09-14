// config.hpp - Unified optimization configurations and academic ablation targets for VMemKV.
//
// ─── OVERVIEW ────────────────────────────────────────────────────────────────
//
// This file acts as the single source of truth for all optimization options and
// academic ablation configs in VMemKV. It unifies both T1 index optimizations
// and T2 value-inlining options into a single, type-safe Tag-List template
// pattern (`vmemkv::Config<Tags...>`).
//
// ─── OPTIMIZATION TAGS (TECHNICAL SPECIFICATION) ──────────────────────────────
//
// * BloomFilter:
//   - [EN]: Prevents unnecessary flash storage access (random read I/O) for non-existent
//           keys using an in-memory mmap filter.
//   - [JP]: 存在しないキーの探索（Get/Update ミス）時に、T2（フラッシュSSD/NVMe）への
//           不要なランダムリードディスクI/Oが発生するのをメモリ上で回避するフィルタ。
//
// * T1InlineValue:
//   - [EN]: Inlines values (1-8 bytes) directly in the T1 index slot by reclaiming unused
//           bits of the hash field for inline metadata, skipping T2.
//   - [JP]: ハッシュフィールドの空きビット（上位4ビット）を利用して、1〜8バイトの値を
//           T1スロット内に直接インライン格納する最適化。T2への書き込みをバイパスする。

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace vmemkv {

// ─── Optimization Tags ───────────────────────────────────────────────────────
struct BloomFilter {};
struct T1InlineValue {};
// Read-path policy ablations for the base/tail split (low_level_design.md 7.5).
// Neither tag (default): trifecta -- size- and reader-dependent choice among the
// primary (MADV_RANDOM) mapping, the base-only default-readahead mapping, and pread().
// ReadPolicyRandomOnly: every base-region read goes through the primary mapping.
// ReadPolicySeqOnly: every base-region read goes through the MADV_SEQUENTIAL mapping.
struct ReadPolicyRandomOnly {};
struct ReadPolicySeqOnly {};

// ─── Facet configs ─────────────────────────────────────────────────────────
// Single-responsibility value groups. Config<...> below aggregates them and
// re-exports the legacy flat names, so existing ConfigT::X uses keep working.
struct T1Config {
  // T1 append region capacity (ablation knob). Keep this as a power of two to preserve
  // cache-friendly masking behavior. Every AppendRegion mmaps T1AppendCapacityEntries *
  // sizeof(AppendSlot) unconditionally, regardless of actual corpus size, and open-addressed
  // placement drives that region to near-full residency even at low load factor -- so this value
  // trades RSS footprint against hard-stall headroom below the entry count a sustained insert
  // burst can reach before a reorganize() cycle drains it. See docs/benchmark/ for the sizing
  // measurement behind the current value.
  static constexpr size_t kAppendCapacityLog2 = 21;
  static constexpr size_t kAppendCapacityEntries = size_t{1} << kAppendCapacityLog2;

  // Target live-entry count per shard for ShardedT1Index (src/t1_index/sharded_t1_index.hpp).
  // A shard's background maintenance reorganize() triggers a split once its live entry count
  // reaches T1ShardSplitThresholdPercent% of this target. See docs/t1_sharding_design.md's
  // "APPEND_CAPのスケーリング" section for how this and T1AppendCapacityEntries relate.
  static constexpr size_t kShardTargetSizeEntries = size_t{1} << 20;
  static constexpr size_t kShardSplitThresholdPercent = 200;
};

struct T2Config {
  // Default Tier 2 (T2) file storage capacity: 1 TiB.
  static constexpr size_t kDefaultCapacityBytes = 1ULL << 40;
  // T2 defragment (storage reclamation) parameters. Defrag relocates live records out of
  // garbage-heavy 8MiB segments to the append frontier, then hole-punches the evacuated
  // segments once their moves are WAL-durable. See docs/t2_defragment_design.md.
  static constexpr uint64_t kSegmentBytes = 8ULL << 20;  // 8 MiB. Fixed, not tuned.
  // Overhead trigger, in percent: a cycle runs once live bytes fall to this fraction of
  // bytes_used or below (i.e. garbage reaches 100 - this value). Single public knob.
  static constexpr uint64_t kDefragSpaceOverheadPercent = 20;
  // Upper bound on relocated bytes per cycle. Caps one cycle's memcpy + WAL cost so a cycle
  // stays a bounded background chore rather than a full-corpus rewrite.
  static constexpr uint64_t kDefragMaxMoveBytesPerCycle = 1ULL << 30;  // 1 GiB.
};

struct WalConfig {
  // Checkpoint trigger: once this many WAL bytes accumulate since the last checkpoint, a
  // checkpoint fires regardless of T1 append-region occupancy, bounding replay time for workloads
  // that never trip the append-region-capacity trigger. See docs/specification/low_level_design.md
  // 4.4.
  static constexpr size_t kMaxBytesSinceCheckpoint = 64ULL << 20;  // 64 MiB.
};

// Read-path policy facet. Neither flag (default): trifecta -- size- and reader-dependent
// choice among the primary (MADV_RANDOM) mapping, the base-only default-readahead mapping,
// and pread(). kRandomOnly: every base-region read goes through the primary mapping.
// kSeqOnly: every base-region read goes through the MADV_SEQUENTIAL mapping.
template <bool RandomOnly, bool SeqOnly>
struct ReadPolicy {
  static constexpr bool kRandomOnly = RandomOnly;
  static constexpr bool kSeqOnly = SeqOnly;
};
// ─── Unified System Config Template (Tag-List Pattern) ──────────────────────
// Aggregates the facet configs above; the legacy flat names are re-exported here so
// existing ConfigT::X uses keep compiling unchanged.
template <typename... Opts>
struct Config : T1Config,
                T2Config,
                WalConfig,
                ReadPolicy<(std::is_same_v<ReadPolicyRandomOnly, Opts> || ...),
                           (std::is_same_v<ReadPolicySeqOnly, Opts> || ...)> {
  template <typename Opt>
  static constexpr bool has_opt = (std::is_same_v<Opt, Opts> || ...);

  static constexpr bool UseBloomFilter = has_opt<BloomFilter>;
  static constexpr bool UseT1InlineValue = has_opt<T1InlineValue>;
  static constexpr bool UseReadPolicyRandomOnly = has_opt<ReadPolicyRandomOnly>;
  static constexpr bool UseReadPolicySeqOnly = has_opt<ReadPolicySeqOnly>;

  static constexpr size_t kBitsPerByte = 8;

  static constexpr size_t T1AppendCapacityLog2 = T1Config::kAppendCapacityLog2;
  static constexpr size_t T1AppendCapacityEntries = T1Config::kAppendCapacityEntries;
  static constexpr size_t T1ShardTargetSizeEntries = T1Config::kShardTargetSizeEntries;
  static constexpr size_t T1ShardSplitThresholdPercent = T1Config::kShardSplitThresholdPercent;
  static constexpr size_t DefaultT2CapacityBytes = T2Config::kDefaultCapacityBytes;
  static constexpr size_t WalMaxBytesSinceCheckpoint = WalConfig::kMaxBytesSinceCheckpoint;
  static constexpr uint64_t T2SegmentBytes = T2Config::kSegmentBytes;
  static constexpr uint64_t T2DefragSpaceOverheadPercent = T2Config::kDefragSpaceOverheadPercent;
  static constexpr uint64_t T2DefragMaxMoveBytesPerCycle = T2Config::kDefragMaxMoveBytesPerCycle;

  static_assert(T1AppendCapacityLog2 > 0 && T1AppendCapacityLog2 < (sizeof(size_t) * kBitsPerByte),
                "T1AppendCapacityLog2 must be in (0, bitwidth(size_t))");
  static_assert(!(UseReadPolicyRandomOnly && UseReadPolicySeqOnly),
                "ReadPolicyRandomOnly and ReadPolicySeqOnly are mutually exclusive");
};

namespace detail {
using T1_AllOff = Config<>;
using System_AllOn = Config<BloomFilter, T1InlineValue>;
}  // namespace detail

// X-macro: the checkpoint phase-breakdown fields of VMemKVStatistics below, and the
// matching atomics CheckpointCoordinator publishes them from. One list drives field
// declaration, snapshot, and publish, so the three can never drift apart.
#define VMEMKV_CHECKPOINT_STATS_FIELDS(X)                    \
  X(last_checkpoint_duration_us)                             \
  X(last_checkpoint_msync_duration_us)                       \
  X(last_checkpoint_t1_reorganize_duration_us)               \
  X(last_checkpoint_stop_writers_duration_us)                \
  X(last_checkpoint_barrier_drain_duration_us)               \
  X(last_checkpoint_wal_rotate_duration_us)                  \
  X(last_checkpoint_wal_rotate_leader_wait_us)               \
  X(last_checkpoint_bytes_synced)                            \
  X(last_checkpoint_corpus_bytes)

#define VMEMKV_DECLARE_STAT_FIELD(name) uint64_t name = 0;

struct VMemKVStatistics {
  // ShardedT1Index::total_splits(): count of shard splits completed, organic (background workers)
  // and explicit (split_shard_containing()) alike -- the real signal for T1 background maintenance
  // activity now that splitting, not a single global reorganize(), is how T1 stays bounded.
  uint64_t t1_split_count = 0;
  // ShardedT1Index::last_split_pause_us()/last_split_pause_end_ns(): the writer-visible pause
  // window of the most recently completed split (see those methods' own comments for exactly what
  // this does and doesn't include). end_ns is a process-local std::chrono::steady_clock epoch
  // timestamp -- only meaningful compared against the caller's own steady_clock::now() calls in
  // this same process, e.g. to place the pause on a benchmark's own polling timeline.
  uint64_t t1_last_split_pause_us = 0;
  uint64_t t1_last_split_pause_end_ns = 0;
  uint64_t checkpoint_count = 0;

  // Phase breakdown for the most recently completed checkpoint_internal() cycle (auto-triggered
  // or manually-forced), for measuring how checkpoint's own cost scales with data volume and
  // trigger frequency. All 0 until the first checkpoint ever completes.
  // stop_writers: writer-pause window for new appends. barrier_drain: in-place-update drain.
  // wal_rotate_leader_wait: subset of wal_rotate spent waiting to become the WAL's group-commit
  // leader (see Wal::last_rotate_leader_wait_us()'s own comment); this typically dominates
  // wal_rotate under sustained concurrent writers. bytes_synced: target - old_base_boundary, the
  // msync()'d delta. corpus_bytes: target, the total T2 footprint as of this cycle.
  VMEMKV_CHECKPOINT_STATS_FIELDS(VMEMKV_DECLARE_STAT_FIELD)

  // Cumulative wall-clock time, summed across all explicit reorganize()/checkpoint() callers,
  // actually spent blocked in wait_until_reorg_not_running() waiting for a concurrently running
  // cycle (organic or another explicit caller's) to finish -- as opposed to that cycle's own
  // wall-clock duration (most of which overlaps unblocked writer progress).
  uint64_t total_reorganize_wait_duration_us = 0;

  // T1Index::AppendRegion instances currently resident, and the high-water mark across this
  // store's lifetime. See T1Index::append_region_live_count()'s own comment: each carries a
  // fixed-size mmap'd footprint that becomes almost fully resident regardless of occupancy, so
  // this bounds T1's own RSS contribution under sustained reorganize() churn.
  int64_t append_region_live_count = 0;
  int64_t append_region_peak_count = 0;

  // CgroupMemoryThrottle::throttle_events(): backpressure sleeps taken by bulk_load_impl()
  // against cgroup v2 memory pressure over this store's lifetime. 0 without a cgroup limit.
  uint64_t bulk_load_throttle_events = 0;

  // T2 defragment: completed cycles, and the last cycle's wall-clock cost, relocated bytes,
  // and hole-punched bytes. t2_live_bytes tracks live T2 bytes (sum of per-record size hints)
  // for the overhead trigger; rebuilt from the T1 checkpoint plus WAL replay at startup.
  uint64_t defrag_cycle_count = 0;
  uint64_t last_defrag_duration_us = 0;
  uint64_t last_defrag_moved_bytes = 0;
  uint64_t last_defrag_punched_bytes = 0;
  uint64_t t2_live_bytes = 0;
};

}  // namespace vmemkv
