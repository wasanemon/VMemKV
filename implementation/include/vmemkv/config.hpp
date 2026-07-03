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
// * AppendMap:
//   - [EN]: Ensures deterministic lock-free index updates by preventing write-path
//           degradation under multi-threaded concurrency.
//   - [JP]: 複数スレッドの並行書き込み時におけるハッシュ衝突と性能低下を防止し、
//           T1への確実なロックフリー書き込みを保証する補助マップ。
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
// * MemoryHints:
//   - [EN]: Uses active OS prefetching policies (madvise sequential/willneed) to ensure
//           uninterrupted throughput during background compaction.
//   - [JP]: Reorganize（メモリ圧縮・断片化解消）時などに、カーネルに対して動的に
//           プリフェッチ（madvise）を適用し、ページフォールトによる性能低下を極小化するヒント。
//
// * T1InlineValue:
//   - [EN]: Inlines values (1-8 bytes) directly in the T1 index slot by reclaiming unused
//           bits of the hash field for inline metadata, skipping T2.
//   - [JP]: ハッシュフィールドの空きビット（上位4ビット）を利用して、1〜8バイトの値を
//           T1スロット内に直接インライン格納する最適化。T2への書き込みをバイパスする。
//
// * CompetitorRocksDB:
//   - [EN]: Tag to swap the storage backend to RocksDB for academic performance comparison.
//   - [JP]: バックエンドのストレージ実装を RocksDB に差し替え、VMemKV 本体と
//           同一の統一インターフェース上で学術的性能比較（ベンチマーク）を行うためのタグ。
//

#pragma once

#include <cstddef>
#include <type_traits>

namespace vmemkv {

// ─── Optimization Tags ───────────────────────────────────────────────────────
struct AppendMap {};
struct BloomFilter {};
struct SimdScan {};
struct MemoryHints {};
struct T1InlineValue {};

// ─── Unified System Config Template (Tag-List Pattern) ──────────────────────
template <typename... Opts>
struct Config {
  template <typename Opt>
  static constexpr bool has_opt = (std::is_same_v<Opt, Opts> || ...);

  static constexpr bool UseAppendMap = has_opt<AppendMap>;
  static constexpr bool UseBloomFilter = has_opt<BloomFilter>;
  static constexpr bool UseSimdScan = has_opt<SimdScan>;
  static constexpr bool UseMemoryHints = has_opt<MemoryHints>;
  static constexpr bool UseT1InlineValue = has_opt<T1InlineValue>;

  // Reorganize thresholds for append-region usage.
  // These are intentionally conservative defaults to avoid hitting APPEND_CAP.
  static constexpr size_t kPercentBase = 100;
  static constexpr size_t kBitsPerByte = 8;
  static constexpr size_t T1ReorganizeSoftThresholdPercent = 75;
  static constexpr size_t T1ReorganizeHardThresholdPercent = 90;

  // T1 append region capacity (ablation knob).
  // Keep this as a power of two to preserve cache-friendly masking behavior.
  static constexpr size_t T1AppendCapacityLog2 = 21;
  static constexpr size_t T1AppendCapacityEntries = size_t{1} << T1AppendCapacityLog2;

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
using T1_AllOn = Config<AppendMap, BloomFilter, SimdScan, MemoryHints>;
using System_AllOn = Config<AppendMap, BloomFilter, SimdScan, MemoryHints, T1InlineValue>;
}  // namespace detail

}  // namespace vmemkv
