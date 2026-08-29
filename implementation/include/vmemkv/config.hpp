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
// * SimdScan (measured no signal, kept as a tag only -- not in the ablation stack; see
//   vmemkv.hpp's SimdScan comment for the measurement):
//   - [EN]: Would accelerate the T1 append region's linear scan by matching 16-byte key
//           prefixes concurrently using CPU vector registers. The tag remains for future
//           re-verification, but its implementation (AppendRegion::scan(), the sole caller of
//           the now-removed optimizations/simd_scan.hpp) was dead code -- t1_index.hpp's actual
//           range scan never called it -- so it was deleted; re-adding it is a prerequisite for
//           actually re-testing this tag.
//   - [JP]: T1インデックス上の16バイトのキープレフィックスをベクトルレジスタに載せ、T1
//           append領域の線形走査を並列に一括高速化する*はずだった*最適化。タグ自体は将来の
//           再検証のために残すが、その実装(AppendRegion::scan()、削除済みの
//           optimizations/simd_scan.hppの唯一の呼び出し元)はデッドコードだった
//           (t1_index.hppの実際のレンジスキャンは一度もこれを呼んでいなかった)ため削除済み。
//           再検証するには実装からやり直す必要がある。
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

// * GetPopulateRead (measured no signal, kept for future re-verification -- not in the ablation
//   stack; see SimdScan's identical disposition above):
//   - [EN]: For a T2 value spanning more than one page, batch-faults its full page range with one
//           madvise(MADV_POPULATE_READ) call before copying it out, instead of letting each page
//           fault in one at a time as the copy touches it -- collapses many page-fault exceptions
//           into one syscall without leaving the mmap (an fd-based read instead would race T2's
//           in-place updates, which are MAP_PRIVATE and never reach the underlying file). Measured
//           on a 64KB/LTM Get/Hit workload (docs/benchmark/20260809_ltm_64kb_get_hit_profiling.md,
//           docs/benchmark/20260809_get_populate_read_prototype.md): major-faults dropped 16.6x
//           (311,864 -> 18,819) but throughput was unchanged (within noise) -- /proc/diskstats
//           showed the same ~4KB average read size and the same total read count either way, so
//           the kernel issues the same number of small disk reads regardless of how many
//           user-space fault exceptions requested them. The bottleneck is disk I/O granularity,
//           not fault-handling overhead, so this optimization doesn't address it.
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
  // cache-friendly masking behavior.
  //
  // Every AppendRegion (one per reorganize() generation, so at least one is always live) mmaps
  // T1AppendCapacityEntries * sizeof(AppendSlot) unconditionally, regardless of actual corpus
  // size. Because AppendSlot placement is open-addressed (effectively uniform-random over the
  // capacity), even light occupancy touches nearly every page: measured locally, inserting only
  // 258K entries (6.15% load factor) into a 2^22-slot region drove RSS to 98.4% of the region's
  // full 224MB (see TODO.md item 6, 2026-08-29) -- a birthday-paradox effect, not a probabilistic
  // edge case. This makes T1's own baseline RSS contribution (>= one region, typically two while
  // a generation is being reclaimed -- measured peak 2, never more, under sustained unthrottled
  // churn) close to T1AppendCapacityEntries * sizeof(AppendSlot) almost regardless of how much
  // data is actually live, which is punishing on a memory-constrained (LTM) host.
  //
  // 21 (not 22) is a measured, not guessed, floor: the worst case for how many entries can pile
  // up in one generation before checkpoint_trigger_due()'s WalMaxBytesSinceCheckpoint (64MiB)
  // threshold forces a reorganize() is a sustained, unthrottled, maximally-concurrent insert
  // burst of the smallest value size the benchmark matrix tests (8 bytes, kInlineValueBytes in
  // bench_kv.cpp) -- measured locally at a peak append_size() of 1,198,470 (16 threads, 3M
  // inserts, 250s, 0 hard-stalls). 2^20's hard threshold (95% of 1,048,576 = 996,147) sits BELOW
  // that measured peak -- confirmed by the same measurement to already regress that workload with
  // real hard-stalls even outside any LTM/pressure scenario. 2^21's hard threshold (95% of
  // 2,097,152 = 1,992,294) keeps ~66% headroom above the measured peak while halving every
  // region's footprint (224MB -> 112MB).
  static constexpr size_t T1AppendCapacityLog2 = 21;
  static constexpr size_t T1AppendCapacityEntries = size_t{1} << T1AppendCapacityLog2;

  // Capacity of the tail-entry tracker that feeds checkpoint_internal()'s copy_live_entries()
  // -- one entry per key written into T2's tail region since the last cycle, so that pass can
  // enumerate exactly what needs copying into the new generation instead of scanning the entire
  // live keyspace. Unlike DeadRangeCapacityEntries above, an entry lost here is a correctness bug,
  // not a benign leak (its bytes only exist in the old generation's tail, which the cycle
  // discards) -- TailEntryHardThresholdPercent leaves generous headroom below 100% so writers
  // block (see maybe_reorganize_if_needed()) well before this tracker could ever actually fill.
  static constexpr size_t TailEntryCapacityLog2 = 20;
  static constexpr size_t TailEntryCapacityEntries = size_t{1} << TailEntryCapacityLog2;
  static constexpr size_t TailEntrySoftThresholdPercent = 50;
  static constexpr size_t TailEntryHardThresholdPercent = 90;

  // Default Tier 2 (T2) file storage capacity: 1 TiB.
  static constexpr size_t DefaultT2CapacityBytes = 1ULL << 40;

  // Checkpoint trigger independent of tail-tracker pressure: once this many WAL bytes accumulate
  // since the last checkpoint, a checkpoint fires regardless of tail occupancy, bounding replay
  // time for workloads that never trip the tail-capacity trigger. See
  // docs/specification/low_level_design.md 4.4. Ignored (the byte-based check is skipped
  // entirely) whenever CheckpointIntervalMs below is nonzero -- see that constant's own
  // comment.
  static constexpr size_t WalMaxBytesSinceCheckpoint = 64ULL << 20;  // 64 MiB.

  // Time-based alternative to WalMaxBytesSinceCheckpoint, for measuring how checkpoint()'s own
  // duration and steady-state write throughput scale with a deliberately larger/smaller tail than
  // the byte threshold would naturally produce -- e.g. to ask "what if checkpoint() only fired
  // every 10 seconds instead of every ~0.2 seconds" independent of how many WAL bytes that
  // happens to accumulate. 0 (the default) means disabled: reorg_worker_loop()'s checkpoint
  // trigger uses WalMaxBytesSinceCheckpoint as before. Nonzero replaces that check entirely
  // (never both at once) with "at least this many milliseconds since checkpoint_internal() last
  // completed" -- see wal_over_threshold()'s call site in reorg_worker_loop(). Milliseconds (not
  // seconds) so a sub-second interval like the "~0.2 seconds" comparison point above can actually
  // be expressed.
  static constexpr size_t CheckpointIntervalMs = 0;

  // defragment() auto-trigger (reorg_worker_loop()): fires once T2's total footprint has grown to
  // DefragGrowthThresholdPercent of its size as of the last defragment cycle (200 = doubled), and
  // only once that footprint has also passed DefragMinBytesBeforeTrigger -- avoids paying for a
  // cycle before a store has grown large enough for accumulated dead space to matter.
  static constexpr size_t DefragGrowthThresholdPercent = 200;
  static constexpr size_t DefragMinBytesBeforeTrigger = 64ULL << 20;  // 64 MiB.

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
using T1_AllOn = Config<BloomFilter>;
using System_AllOn = Config<BloomFilter, T1InlineValue>;
}  // namespace detail

struct VMemKVStatistics {
  uint64_t t1_reorg_count = 0;
  uint64_t t2_reorg_count = 0;
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
