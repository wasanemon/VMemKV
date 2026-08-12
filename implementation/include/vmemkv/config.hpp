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
// * SimdScan:
//   - [EN]: Accelerates search paths and range queries by matching 16-byte key prefixes
//           concurrently using CPU vector registers.
//   - [JP]: T1インデックス上の16バイトのキープレフィックスをベクトルレジスタに載せ、
//           レンジスキャンや衝突探索時の線形走査を並列に一括高速化するSIMD命令最適化。
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
struct Prefaulting {};

// * GetPopulateRead (measured no signal, kept for future re-verification -- not in the ablation
//   stack; see SimdScan's identical disposition below):
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

// * ScanBaseSequential:
//   - [EN]: T2's "base" region (offsets below the boundary written by the last full T1+T2
//           reorganize) is read through three mappings/handles, chosen per record from that
//           record's own length (the T1 index's embedded size hint, `kSizeEmbeddingShift`), not
//           once per generation -- since in-place updates to base offsets are redirected
//           out-of-place (see update_impl()), the region stays immutable and none of the three
//           ever needs a seqlock, and picking the "wrong" one for a given record is only ever a
//           readahead-policy mismatch, never incorrect data (all three cover the identical
//           bytes). Two are mmaps distinct from the main MADV_RANDOM mapping Get/Update use --
//           madvise is a property of a whole mapping, not of an individual read, so serving both
//           small- and large-record reads well requires two separately-advised mappings rather
//           than one shared policy:
//             - `base_mmap_scan_seq`, advised MADV_SEQUENTIAL, for records whose embedded size
//               hint is one page or smaller: a wide readahead window batches many small records'
//               worth of faults into few major faults (a 2.2x win measured at 1KB Scan).
//             - `base_mmap_scan`, left at the kernel's default (adaptive) policy, for larger
//               records: an unconditional MADV_SEQUENTIAL was measured to fetch ~25-30% more
//               bytes per major fault than needed for these: close to free under cold/uniform
//               access, but pure waste under skewed (Zipf-like) reuse of a large-record corpus,
//               where a narrow hot range staying page-cache-resident is the actual source of the
//               speedup -- a 2x loss measured at 64KB Scan if MADV_SEQUENTIAL is kept.
//           See docs/benchmark/20260807_scan_t2_base_tail_io_uring_read.md for the original
//           io_uring design these replace (a per-file readahead policy was the actual win, not
//           io_uring's async multiplexing -- a plain mmap gets the same benefit, measured within
//           ~4% of the io_uring read it replaces when cold and 6-26% *faster* once warm, no
//           per-read syscall floor). scan_impl() always reads through one of these two mmaps;
//           get_impl() reads small records (one page or smaller) through `base_mmap_scan_seq`
//           too, but larger records through the third handle instead:
//             - `read_fd`, a `dup()`'d file descriptor, with a bounded `pread()` sized to that
//               one record. get_impl() used to share `base_mmap_scan`'s large-record mmap
//               approach too (a since-removed policy on that same mapping), which suited its
//               one-record reads about as poorly as an unconditional MADV_SEQUENTIAL suited Scan
//               under skew, for the identical reason: mmap readahead speculatively pulls in
//               neighboring records nobody asked for, wasting disk bandwidth Get never gets to
//               reuse (unlike Scan's own multi-record window, where reused bytes at least
//               sometimes pay off). A bounded pread() reads exactly what's needed instead.
//           All three are best-effort: a failure to establish any one of them just means the
//           corresponding read falls back to the always-correct main-mmap-plus-seqlock path.
//   - [JP]: T2の「base」領域(直近のT1+T2フルreorganizeが書いた境界より下のオフセット)は、
//           世代ごとに1回ではなく、**レコードごとに、そのレコード自身の長さ**(T1インデックス
//           に埋め込まれたサイズヒント`kSizeEmbeddingShift`)から選ばれる3つのマッピング/
//           ハンドルのいずれかで読む。baseオフセットへのin-place更新はout-of-placeへ
//           リダイレクトされる(update_impl参照)ため領域は不変であり続け、どれもseqlockを
//           一切必要としない。あるレコードに「間違った」経路を選んでも、3つとも同一のバイト列
//           を指しているため、readahead方針の選択ミスにしかならず、データが誤ることはない。
//           そのうち2つは、Get/Updateが使うMADV_RANDOMの主mmapとは別のmmapである --
//           madviseはマッピング全体の属性であり個々の読み取り単位のものではないため、小さい
//           レコードと大きいレコードの両方にうまく対応するには、1つの共有方針ではなく別々に
//           advise済みの2つのマッピングが要る:
//             - `base_mmap_scan_seq`(`MADV_SEQUENTIAL`付き): 埋め込みサイズヒントが1ページ
//               以下のレコード用。広いreadahead窓で多数の小さいレコードのフォルトを少数の
//               major faultにまとめられる(1KB Scanで2.2倍の改善を実測)。
//             - `base_mmap_scan`(カーネルのデフォルト(適応的)方針のまま): それより大きい
//               レコード用。無条件に`MADV_SEQUENTIAL`を付けると、こちらでは1 major fault
//               あたり約25-30%余分にバイトを読み込むことが判明した -- Uniform(ほぼ毎回
//               コールド)ではほぼ無害だが、大きいレコードのコーパスをZipfのような偏った
//               アクセスで読む場合、狭いホット範囲がページキャッシュに残ることこそが速度
//               向上の源泉であり、この余分な先読みはそれを圧迫するだけの無駄になる --
//               `MADV_SEQUENTIAL`のままだと64KB Scanで2倍の悪化を実測。
//           scan_impl()は常にこの2つのmmapのどちらかを読む。get_impl()も小さいレコード
//           (1ページ以下)は同じ`base_mmap_scan_seq`を読むが、大きいレコードは3つ目の
//           ハンドルを使う:
//             - `read_fd`(`dup()`したファイルディスクリプタ): そのレコード1つぶんに
//               サイズを絞った`pread()`で読む。以前はget_impl()の大きいレコードも
//               `base_mmap_scan`と同じmmap方式(そのマッピング上の、廃止済みの方針)を
//               共用していたが、これはScanがskew下で無条件`MADV_SEQUENTIAL`によって
//               抱えていたのと全く同じ理由で不向きだった: mmapのreadaheadは頼んでもいない
//               隣のレコードまで投機的に読み込んでしまい、Getはそれを再利用する機会が
//               (Scanの複数レコード窓と違って)一切ないため、ディスク帯域を純粋に無駄に
//               する。境界を絞った`pread()`なら必要な分だけを読める。
//           3つともbest-effort -- 確立に失敗しても、対応する読み取りは常に正しい
//           主mmap+seqlock経路にフォールバックするだけである。
struct ScanBaseSequential {};

// ─── Unified System Config Template (Tag-List Pattern) ──────────────────────
template <typename... Opts>
struct Config {
  template <typename Opt>
  static constexpr bool has_opt = (std::is_same_v<Opt, Opts> || ...);

  static constexpr bool UseBloomFilter = has_opt<BloomFilter>;
  static constexpr bool UseSimdScan = has_opt<SimdScan>;
  static constexpr bool UseT1InlineValue = has_opt<T1InlineValue>;
  static constexpr bool UsePrefaulting = has_opt<Prefaulting>;
  static constexpr bool UseScanBaseSequential = has_opt<ScanBaseSequential>;
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
using System_AllOn = Config<BloomFilter, T1InlineValue, Prefaulting, ScanBaseSequential>;
}  // namespace detail

struct VMemKVStatistics {
  uint64_t t1_reorg_count = 0;
  uint64_t t2_reorg_count = 0;
  uint64_t hard_stall_count = 0;
};

}  // namespace vmemkv
