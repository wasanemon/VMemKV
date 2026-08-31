# 2026-08-23 defragment_internal() スケーリング実測値

AWS i4i.8xlarge、LTM は cgroup メモリ制約下(`MemoryHigh=1GiB`/`MemoryMax=2GiB`/`MemorySwapMax=1TiB`、
`target_ratio=8.0`)。`defragment_internal()`(逐次書き換え方式、単一スレッド版)の実測値。

> この計測当時の`defragment_internal()`は実際にT2レコードを再配置する実装だった。round 1で
> no-opに戻され、round 3で公開API `defragment()` ごとコードベースから完全に削除された
> (`docs/specification/defragment_redesign_proposal.md`参照)。

## コーパスサイズスイープ(churn=0、単独実行、フルコーパス=ratio 100%)

| combo | 所要時間 | スループット | Insert 実効レート(参考、同一実行内の isolated write TPS) | 負荷率 r |
|---|---|---|---|---|
| in_memory/8B | 3.10 s | 6,452,141 rec/s | 119,679 rec/s | 0.019 |
| in_memory/1KB | 21.9 s | 365,022 rec/s | 49,601 rec/s | 0.136 |
| ltm/1KB | 53.4 s | 154,566 rec/s | 49,340 rec/s | 0.319 |
| ltm/64KB | 27.4 s | 4,784 rec/s | 8,097 rec/s | 1.69(発散) |

25/50/75% の中間段階も含め全16点が完走。64KB のみ、単一スレッドでは Insert に追いつかない。

## Concurrent-Write Contention spot check(32 writer threads、ratio=100%)

| combo | isolated write TPS | concurrent write TPS | 低下率 | defragment() 所要時間 |
|---|---|---|---|---|
| in_memory/8B | 119,679/s | 107,735/s | -10% | 4.13 s |
| in_memory/1KB | 49,601/s | 17,683/s | -64% | 35.99 s |
| ltm/1KB | 49,340/s | 10,557/s | -79% | ≥60 s(タイムアウト) |
| ltm/64KB | 8,097/s | 168/s | -98% | ≥60 s(タイムアウト) |

単独実行では余裕を持って完走する同じフルコーパス(ltm/1KB 53.4s、ltm/64KB 27.4s)が、並行書き込み下
では60秒以内に完了しない。書き込み側のTPSも大きく低下する。
