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
  static constexpr size_t T2StorageFragmentationThresholdPercent = 30;

  // T1 append region capacity (ablation knob).
  // Keep this as a power of two to preserve cache-friendly masking behavior.
  static constexpr size_t T1AppendCapacityLog2 = 22;
  static constexpr size_t T1AppendCapacityEntries = size_t{1} << T1AppendCapacityLog2;

  // Capacity of the dead old-base offset-range tracker that feeds checkpoint_and_defragment()'s
  // hole-punch targets (TODO.md item 5) -- one entry per out-of-place-redirected update or delete
  // whose superseded offset was in the old-base region. Nearing capacity forces an earlier cycle
  // (see space_amp_over_threshold()'s sibling check), same role T1ReorganizeHardThresholdPercent
  // plays for T1AppendCapacityEntries.
  static constexpr size_t DeadRangeCapacityLog2 = 20;
  static constexpr size_t DeadRangeCapacityEntries = size_t{1} << DeadRangeCapacityLog2;

  // Default Tier 2 (T2) file storage capacity: 1 TiB.
  static constexpr size_t DefaultT2CapacityBytes = 1ULL << 40;

  // Checkpoint trigger independent of T2StorageFragmentationThresholdPercent: once this many
  // WAL bytes accumulate since the last checkpoint, a checkpoint fires regardless of
  // fragmentation, bounding replay time for workloads that never trip the fragmentation
  // trigger. See docs/specification/low_level_design.md 4.4.
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
using T1_AllOn = Config<BloomFilter>;
using System_AllOn = Config<BloomFilter, T1InlineValue>;
}  // namespace detail

struct VMemKVStatistics {
  uint64_t t1_reorg_count = 0;
  uint64_t t2_reorg_count = 0;
  uint64_t hard_stall_count = 0;
};

}  // namespace vmemkv
