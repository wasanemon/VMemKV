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
// * InlineShort:
//   - [EN]: Inlines short values (1-7 bytes) directly in the T1 index slot by reclaiming unused
//           bits for size metadata, skipping T2. deterministic size-based inlining: has NO false positives or false negatives.
//   - [JP]: 1〜7バイトの極小値を、サイズメタデータ用に空いたビット領域を利用してT1スロット内に直接インライン格納する最適化。
//           サイズ（1〜7B）のみに依存する決定論的インライン化であり、偽陽性（False Positive）や偽陰性（False Negative）のリスクは一切存在しない。
//
// * Inline8B:
//   - [EN]: Inlines 8-byte integral values in the T1 index slot when LSB is 1 (e.g. aligned pointers, odd IDs),
//           reusing the value domain space since there are no spare bits for metadata.
//           Value-based inlining: carries a theoretical risk of False Positives if a T2 storage offset's LSB is 1.
//           VMemKV prevents this by enforcing that all T2 file offsets are even-aligned (LSB=0) to avoid bit collisions.
//   - [JP]: 8バイトの特定整数値（LSBが1）をT1スロット内に直接インライン格納する最適化。メタデータ用の空きビットがないため、
//           値自体のドメイン（LSB=1）に依存して格納判定を行う値依存のインライン化。
//           理論上、T2ストレージオフセット自体のLSBが1になった場合にインラインデータと誤認する「偽陽性（False Positive）」のリスクがあるため、
//           VMemKVではT2ファイルオフセットを偶数アライメント（LSB=0）に強制マスクすることでビット衝突を回避している。
//           サイズ依存の `InlineShort` とはインライン化の条件とエンコード機構が異なるため、個別に分離されている。
//
// * CompetitorRocksDB:
//   - [EN]: Tag to swap the storage backend to RocksDB for academic performance comparison.
//   - [JP]: バックエンドのストレージ実装を RocksDB に差し替え、VMemKV 本体と
//           同一の統一インターフェース上で学術的性能比較（ベンチマーク）を行うためのタグ。
//

#pragma once

#include <type_traits>

namespace vmemkv {

// ─── Optimization Tags ───────────────────────────────────────────────────────
struct AppendMap {};
struct BloomFilter {};
struct SimdScan {};
struct MemoryHints {};
struct InlineShort {};
struct Inline8B {};

// ─── Unified System Config Template (Tag-List Pattern) ──────────────────────
template <typename... Opts>
struct Config {
    template <typename Opt>
    static constexpr bool has_opt = (std::is_same_v<Opt, Opts> || ...);

    static constexpr bool UseAppendMap   = has_opt<AppendMap>;
    static constexpr bool UseBloomFilter = has_opt<BloomFilter>;
    static constexpr bool UseSimdScan    = has_opt<SimdScan>;
    static constexpr bool UseMemoryHints = has_opt<MemoryHints>;
    static constexpr bool UseInlineShort = has_opt<InlineShort>;
    static constexpr bool UseInline8B    = has_opt<Inline8B>;
};

namespace detail {
    using T1_AllOff = Config<>;
    using T1_AllOn  = Config<AppendMap, BloomFilter, SimdScan, MemoryHints>;
    using System_AllOn = Config<AppendMap, BloomFilter, SimdScan, MemoryHints, InlineShort, Inline8B>;
}

} // namespace vmemkv
