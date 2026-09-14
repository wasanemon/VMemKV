# T1のインデックス構造: flat arrayを維持する

T1は`src/t1_index/t1_index.hpp`のflat array(append region + sorted region +
`reorganize()`によるマージ)を採用する。pskiplist(mmap'd・lock-freeなskip list、
`benchmark/20260903_t1_pskiplist_design_survey.md`参照)はT1としては採用しない。

範囲シャーディングは [`t1_sharding_design.md`](t1_sharding_design.md) に定義する。

## 関連コード

- `src/t1_index/t1_index.hpp`: シャード1個分のT1実装。
- `src/t1_index/sharded_t1_index.hpp`: 複数シャードを束ねるルーティング層(現行のT1)。
- `src/vmemkv_impl.hpp`: `ShardedT1Index`を介した`VMemKVImpl`の実装。
