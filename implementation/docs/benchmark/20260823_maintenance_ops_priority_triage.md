# 2026-08-23 checkpoint()/defragment()/reorganize() 着手優先度トリアージ

> **注意**: 本ドキュメントの数値はすべてコミット `6fe46b41952d886764e2cbe270da7b07c2b0265b` 時点の実測値・実装のスナップショットである。以降のコード変更(特にcheckpoint()/defragment()の並列化やバックプレッシャー実装)により数値・結論とも変化しうる。再検証せずにこの結論を恒久的な事実として引用しないこと。
>
> **無効**: `defragment()` は round 1 で no-op 化され、round 3 で API ごとコードベースから
> 完全に削除された。本ドキュメントの3操作間の優先度トリアージは `defragment()` が実データを
> 動かすことを前提としており、その前提が失われた現在は結論全体が無効である。歴史的記録として
> のみ残す。

## 背景

VMemKVの3つのメンテナンス操作(`checkpoint()`・`defragment()`・`reorganize()`)は、それぞれ異なるコスト特性を持つ。`benchmark_results/pages/2026082101_charts.html`のsummary tabに各操作の素のスループット比較(Insert vs 各操作)と、並行書き込み下での干渉度(Maintenance Operations: Concurrent-Write Contention)が揃ったことで、初めて3操作を横並びで比較できるようになった。本ドキュメントはこのデータと、各操作の自動トリガー条件(`implementation/include/vmemkv/config.hpp`)を突き合わせ、どの操作から着手すべきか、どれは着手不要かを判断するための整理である。

## データソース

- スループット比較・干渉度: `benchmark_results/pages/2026082101_charts.html`(データ元は`benchmark_results/2026082101/`配下の各JSONL)
- 計測環境: AWS i4i.8xlarge、32 writer threads、LTMは`systemd-run`スコープでのcgroupメモリ制約下(`MemoryHigh=1GiB`/`MemoryMax=2GiB`/`MemorySwapMax=1TiB`、`target_ratio=8.0`)
- トリガー条件: `implementation/include/vmemkv/config.hpp`の該当定数、および`implementation/src/vmemkv_impl.hpp`の`reorg_worker_loop()`

## 1. 単体スループット負荷率

`r = Insert(32 threads)のrecords/sec ÷ 各操作の単体実行スループット`。`r > 1`は、その操作の処理速度がInsertの書き込み速度に追いつけないことを意味する。

| combo | checkpoint() | defragment() | reorganize() |
|---|---:|---:|---:|
| 8B/In-Memory | 0.03x | 0.02x | 0.02x |
| 1KB/In-Memory | 0.04x | 0.31x | 0.06x |
| 1KB/LTM | 0.07x | 0.70x | 0.05x |
| 64KB/LTM | 0.49x | **2.47x(Insertに負ける)** | 0.01x |

`defragment()`のltm/64KBのみが`r > 1`。他は全て余裕を持ってInsertに追いつく。`checkpoint()`は本セッションでpre-stopパスを並列化済みで、64KB/LTMでも0.49xまで改善している(並列化前は`r`が1を超えていた)。

## 2. 並行書き込みへの性能劣化(Slowdown%)と所要時間

Slowdown% = `(1 - concurrent_write_tps / isolated_write_tps) × 100`。32 writer threadsが継続的に書き込む中で各操作を実行し、操作自体の所要時間も同時に計測している。

| combo | checkpoint() Slowdown / 所要時間 | defragment() Slowdown / 所要時間 | reorganize() Slowdown / 所要時間 |
|---|---|---|---|
| 8B/In-Memory | 28% / 5.02s | 10% / 4.13s | -14% / <1ms(参考値) |
| 1KB/In-Memory | 61% / 2.89s | 64% / 35.99s | 31% / <1ms(参考値) |
| 1KB/LTM | 80% / 5.68s | 79% / ≥60s(timeout) | 64% / <1ms(参考値) |
| 64KB/LTM | 51% / ≥60s(timeout) | **98% / ≥60s(timeout)** | 16% / <1ms(参考値) |

`reorganize()`のSlowdown%は、単発呼び出しでは計測窓が短すぎてノイズになるため(`20260823`の同セッション内で修正済み、`bench_kv.cpp`の`run_contention_probe()`参照)、1秒以上reorganize()を連続で叩き続けた場合の平均値である。実運用でのトリガー間隔(後述、約200万件書き込みに1回)よりはるかに密な条件での測定であり、実際の総合影響はこの数字が示すより小さいと考えてよい。

## 3. トリガー頻度(自動発火条件)

`reorg_worker_loop()`は起床のたびに以下の優先順位で判定する(`vmemkv_impl.hpp`)。

```
if (tail_entries_.near_capacity() || wal_over_threshold()) {
  reorganize_internal(ReorgMode::Checkpoint);
} else if (defrag_growth_over_threshold()) {
  reorganize_internal(ReorgMode::Defragment);
} else {
  reorganize_internal(ReorgMode::T1Only);
}
```

| 操作 | 発火条件 | 閾値の実数 |
|---|---|---|
| checkpoint() | tail_entries_ が容量の50%到達、または前回checkpoint以降のWALが64MiB到達のどちらか早い方 | `TailEntryCapacityEntries`(2^20=1,048,576)の50% = 524,288件 / `WalMaxBytesSinceCheckpoint` = 64MiB |
| defragment() | T2の総フットプリントが前回defragment完了時の200%(倍)に到達、かつ64MiB以上 | `DefragGrowthThresholdPercent` = 200% / `DefragMinBytesBeforeTrigger` = 64MiB |
| reorganize()(T1-only) | T1 append領域が容量の50%到達、かつ上記2条件を満たさない場合のfallback | `T1AppendCapacityEntries`(2^22=4,194,304)の50% = 2,097,152件 |

churn(update/delete)の多いワークロードほどtail_entries_/WALが早く埋まるためcheckpoint()が優勢に、insert中心のワークロードほどT1 append領域が先に埋まるためreorganize()(T1-only)が優勢になる。defragment()は「倍増」という他の2つより高いハードルのため、3つの中では最も稀にしか発火しない設計になっている。

## 4. 総合影響 ≈ 頻度 × 所要時間 × 劣化率

- **checkpoint()**: 3操作の中で最も低い閾値(524,288件 or 64MiB)で発火するため最も頻繁。かつltm/64KBでは所要時間がタイムアウト(≥60s)まで伸び、51%劣化を伴う。頻度・規模の掛け算で総合影響が最大と見積もる。
- **defragment()**: 発火頻度は最も低い(footprint倍増が必要)。しかし一度起きると最悪のケースになる(ltm/64KB: 98%劣化・タイムアウト、単体スループットでもInsertに2.47倍の差で負ける)。churnの多いワークロードでは倍増までの時間が短くなり発火頻度が上がりうるため、稀だからと軽視はできない。
- **reorganize()(T1-only)**: 発火頻度自体はinsertヘビーな場面で高いが、所要時間がほぼ0秒(サブミリ秒)。2節のSlowdown%の数値は密な連続呼び出し下の測定であり、実運用の疎な発火間隔(約200万件書き込みに1回)ではさらに小さくなると考えられる。総合影響は無視できるレベル。

## 結論・着手順序

1. **checkpoint()から着手(最優先)** — 頻度・規模とも最大。特にltm/64KBのタイムアウト解消が急務。
2. **defragment()が次点** — 発火頻度は低いが、一度起きると最悪(ltm/64KBで単体スループットもInsertに負け、並行実行時はほぼ書き込み停止に近い)。`defragment_redesign_proposal.md` 3.5節の並列化と、バックプレッシャー機構の両方が候補。
3. **reorganize()(T1-only)は着手不要** — 所要時間がほぼゼロで総合影響が小さく、労力対効果が薄いと判断する。
