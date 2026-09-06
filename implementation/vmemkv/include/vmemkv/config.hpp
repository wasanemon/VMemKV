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
// * SimdScan (tag only, not in the ablation stack -- no implementation currently wired up):
//   - [EN]: Would accelerate the T1 append region's linear scan by matching 16-byte key
//           prefixes concurrently using CPU vector registers.
//   - [JP]: T1インデックス上の16バイトのキープレフィックスをベクトルレジスタに載せ、T1
//           append領域の線形走査を並列に一括高速化する最適化(未実装)。
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
struct SimdScan {};
struct T1InlineValue {};

// * GetPopulateRead (tag only, not in the ablation stack -- reduces fault count, not throughput;
//   see docs/benchmark/20260809_get_populate_read_prototype.md):
//   - [EN]: For a T2 value spanning more than one page, batch-faults its full page range with one
//           madvise(MADV_POPULATE_READ) call before copying it out, instead of letting each page
//           fault in one at a time as the copy touches it.
struct GetPopulateRead {};

// ─── Unified System Config Template (Tag-List Pattern) ──────────────────────
template <typename... Opts>
struct Config {
  template <typename Opt>
  static constexpr bool has_opt = (std::is_same_v<Opt, Opts> || ...);

  static constexpr bool UseBloomFilter = has_opt<BloomFilter>;
  static constexpr bool UseSimdScan = has_opt<SimdScan>;
  static constexpr bool UseT1InlineValue = has_opt<T1InlineValue>;
  static constexpr bool UseGetPopulateRead = has_opt<GetPopulateRead>;

  // Reorganize thresholds for append-region usage.
  // These are intentionally conservative defaults to avoid hitting APPEND_CAP.
  static constexpr size_t kPercentBase = 100;
  static constexpr size_t kBitsPerByte = 8;
  static constexpr size_t T1ReorganizeSoftThresholdPercent = 50;
  static constexpr size_t T1ReorganizeHardThresholdPercent = 95;

  // T1 append region capacity (ablation knob). Keep this as a power of two to preserve
  // cache-friendly masking behavior. Every AppendRegion mmaps T1AppendCapacityEntries *
  // sizeof(AppendSlot) unconditionally, regardless of actual corpus size, and open-addressed
  // placement drives that region to near-full residency even at low load factor -- so this value
  // trades RSS footprint against hard-stall headroom below the entry count a sustained insert
  // burst can reach before a reorganize() cycle drains it. See docs/benchmark/ for the sizing
  // measurement behind the current value.
  static constexpr size_t T1AppendCapacityLog2 = 21;
  static constexpr size_t T1AppendCapacityEntries = size_t{1} << T1AppendCapacityLog2;

  // Target live-entry count per shard for ShardedT1Index (src/t1_index/sharded_t1_index.hpp).
  // A shard's background maintenance reorganize() triggers a split once its live entry count
  // reaches T1ShardSplitThresholdPercent% of this target. See docs/t1_sharding_design.md's
  // "APPEND_CAPのスケーリング" section for how this and T1AppendCapacityEntries relate.
  static constexpr size_t T1ShardTargetSizeEntries = size_t{1} << 20;
  static constexpr size_t T1ShardSplitThresholdPercent = 200;

  // Default Tier 2 (T2) file storage capacity: 1 TiB.
  static constexpr size_t DefaultT2CapacityBytes = 1ULL << 40;

  // Checkpoint trigger: once this many WAL bytes accumulate since the last checkpoint, a
  // checkpoint fires regardless of T1 append-region occupancy, bounding replay time for workloads
  // that never trip the append-region-capacity trigger. See docs/specification/low_level_design.md
  // 4.4.
  static constexpr size_t WalMaxBytesSinceCheckpoint = 64ULL << 20;  // 64 MiB.

  static_assert(T1ReorganizeSoftThresholdPercent > 0 && T1ReorganizeSoftThresholdPercent < kPercentBase,
                "T1ReorganizeSoftThresholdPercent must be in (0, 100)");
  static_assert(T1ReorganizeHardThresholdPercent > 0 && T1ReorganizeHardThresholdPercent < kPercentBase,
                "T1ReorganizeHardThresholdPercent must be in (0, 100)");
  static_assert(T1ReorganizeSoftThresholdPercent <= T1ReorganizeHardThresholdPercent,
                "Soft threshold must be <= hard threshold");
  static_assert(T1AppendCapacityLog2 > 0 && T1AppendCapacityLog2 < (sizeof(size_t) * kBitsPerByte),
                "T1AppendCapacityLog2 must be in (0, bitwidth(size_t))");
};

namespace detail {
using T1_AllOff = Config<>;
using System_AllOn = Config<BloomFilter, T1InlineValue>;
}  // namespace detail

struct VMemKVStatistics {
  uint64_t t1_reorg_count = 0;
  uint64_t checkpoint_count = 0;
  uint64_t hard_stall_count = 0;

  // Phase breakdown for the most recently completed checkpoint_internal() cycle (auto-triggered
  // or manually-forced), for measuring how checkpoint's own cost scales with data volume and
  // trigger frequency. All 0 until the first checkpoint ever completes.
  uint64_t last_checkpoint_duration_us = 0;
  uint64_t last_checkpoint_msync_duration_us = 0;
  uint64_t last_checkpoint_t1_reorganize_duration_us = 0;
  uint64_t last_checkpoint_stop_writers_duration_us = 0;   // Writer-pause window for new appends.
  uint64_t last_checkpoint_barrier_drain_duration_us = 0;  // In-place-update drain.
  uint64_t last_checkpoint_wal_rotate_duration_us = 0;
  // Subset of last_checkpoint_wal_rotate_duration_us spent waiting to become the WAL's
  // group-commit leader (see Wal::last_rotate_leader_wait_us()'s own comment) -- found to
  // dominate wal_rotate under sustained concurrent writers, far more than the open()/close()/
  // unlink() calls rotate_segment() also makes.
  uint64_t last_checkpoint_wal_rotate_leader_wait_us = 0;
  uint64_t last_checkpoint_bytes_synced = 0;  // target - old_base_boundary: the msync()'d delta.
  uint64_t last_checkpoint_corpus_bytes = 0;  // target: total T2 footprint as of this cycle.

  // Cumulative wall-clock time, summed across all writer threads, actually spent blocked in
  // wait_until_reorg_not_running() (hard backpressure) -- the real writer-facing cost of a
  // reorg/checkpoint cycle, as opposed to that cycle's own wall-clock duration (most of which
  // overlaps unblocked writer progress).
  uint64_t total_hard_stall_duration_us = 0;

  // T1Index::AppendRegion instances currently resident, and the high-water mark across this
  // store's lifetime. See T1Index::append_region_live_count()'s own comment: each carries a
  // fixed-size mmap'd footprint that becomes almost fully resident regardless of occupancy, so
  // this bounds T1's own RSS contribution under sustained reorganize() churn.
  int64_t append_region_live_count = 0;
  int64_t append_region_peak_count = 0;
};

}  // namespace vmemkv
