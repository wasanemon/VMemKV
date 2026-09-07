# T1のインデックス構造: flat arrayを維持する

T1は`src/t1_index/t1_index.hpp`のflat array(append region + sorted region +
`reorganize()`によるマージ)を採用する。pskiplist(mmap'd・lock-freeなskip list、
`benchmark/20260903_t1_pskiplist_design_survey.md`参照)はT1としては採用しない。

## 理由

flat arrayは探索・Scanのいずれも配列の添字計算のみで完結し、Scanは物理的に連続した領域を
読むためハードウェアのプリフェッチが効く。skip list系の構造はこれを、独立に確保された
オブジェクト間の、依存関係のあるポインタチェイシングに置き換えるため、探索・Scanの双方で
flat arrayより遅くなる。この差はskip list側の実装をどれだけ最適化しても構造的に埋まらない。

`reorganize()`/`checkpoint()`のO(corpus)コストへの対処は、ポインタ構造への転換ではなく
範囲シャーディング(T1Indexを独立した複数シャードに分割し、`reorganize()`をシャード単位に
局所化する)として実装済みである。設計は
[`t1_sharding_design.md`](t1_sharding_design.md)を参照。

## 関連コード

- `src/t1_index/t1_index.hpp`: シャード1個分のT1実装。
- `src/t1_index/sharded_t1_index.hpp`: 複数シャードを束ねるルーティング層(現行のT1)。
- `src/vmemkv_impl.hpp`: `ShardedT1Index`を介した`VMemKVImpl`の実装。

## 背景・調査の詳細

[`docs/benchmark/20260905_t1_structure_alternatives_survey.md`](benchmark/20260905_t1_structure_alternatives_survey.md)
を参照。pskiplistへの移行の実測結果、複数ラウンドのパフォーマンスチューニングとその効果、
perfプロファイリングによる根本原因の特定、Packed Memory Array・Bw-tree・Adaptive Radix Tree
の検討経緯、範囲シャーディングの先行研究をまとめている。
