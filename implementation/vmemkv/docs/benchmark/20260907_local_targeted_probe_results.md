# 2026-09-07 Targeted local probe results (tmpfs + cgroup, WSL2 box)

(1) の的を絞った3点（organic-split 両パターン、YCSB-E/Scan、LTM background-jobs）の
ローカル実行記録。条件: 20 cores / 31GB RAM、DB は in_memory 系のみ `/dev/shm`
(tmpfs、fsync 1.5µs。`/tmp` は fsync 3.9ms で WAL 律速になるため不使用)、
Release ビルド、20 writer threads、1KB values。

> 注意: 本機は AWS i4i.8xlarge ではないため、以下の絶対値（QPS・所要時間）は AWS 測定と
> 比較できない。本ドキュメントが保証するのは機構レベルの可否（ハングしない・回帰しない・
> probe が動作する）のみである。論文の headline 数字はフルマトリクス(2)で取り直す。

## 1. organic-split probe (in_memory, 90s)

- monotonic: 7.5M inserted、6 splits (shard 2→7)、pause 179〜251ms、
  degradation 78〜97%。shard 数の増加に伴う縮小傾向なし。
- random (`--key-pattern=random`): 7.7M inserted、3 splits (shard 1→2→4、
  2 件の split が同一 poll 間隔に重なり 2 イベントとして観測)、pause 201/284ms、
  degradation 99%/78%。書き込み分散後も縮小なし。
- 結論: shard 数 2〜7 の範囲では、1 split の影響は書き込み分布によらず high のまま。
  split 本体の CPU コスト(reorganize の sequential sort、約 200ms)が前景と競合する
  ためで、単純な writers 割合の希釈では説明できない。`docs/t1_sharding_design.md`
  の同旨の結論を random 側の証拠で補強した。
- 副産物: random パターンで QPS 計測が全滅するカウンタバグ
  (`total_inserted` が monotonic 専用の `next_key` を参照) を発見・修正。
  挿入完了数を数える `inserted` カウンタに分離した。

## 2. YCSB-E / Scan (in_memory, 1KB, VMemKV Baseline)

- YCSB-E (20 threads, 8M keys): pre-change 118.7k/s vs post-change 113.5k/s (単発)。
- Scan ベンチ pre/post 比較 (1KB):
  - post rep1: 低スレッドで +5〜+13%、16/20 threads で -4〜-9%。
  - post rep2: 全点で base 超え (t=20: 146.8k/s vs base 133.5k/s)。
- 結論: run 間変動が ±10% あり、単発の差はノイズ帯。scan バッチ化による
  in_memory での回帰なし。in_memory（全 resident）では offset 順の readahead 利得は
  出ないため、真の効果測定は LTM 側で行う（フルマトリクス(2)に含める）。

## 3. LTM background-jobs probe (MemoryHigh=1G/MemoryMax=2G, 10M x 1KB)

- populate (bulk_load + throttle) 完走。hang なし。
- ltm/reorganize: job 0.90s、insert -53.8% / update -11.3% / scan -0.0%。
- ltm/checkpoint: job 6.55s (内訳 t1_reorganize 5.58s + wal_rotate 0.6ms +
  msync 309ms)、insert -55.5% / update -11.5% / scan -0.0%。
- checkpoint 空シャードスキップ導入後の再測定: ltm/reorganize 0.35s、
  ltm/checkpoint 6.81s (内訳 t1_reorganize 5.20s + wal_rotate 0.9ms +
  msync 577ms)。T1 フェーズ -7%、reorganize -61%。populate 直後のため大半の
  シャードが dirty でスキップ余地が小さく改善は控えめ。msync の 309→577ms 振れは
  ディスク側のばらつき。
- 定常状態の A/B（6M x 8B、3 shards、スキップ有無のみ差し替え、単発）:
  bulk_load 直後 checkpoint #1 は 0.79s(base) vs 0.86s(skip) で互角（全 dirty のため
  スキップ不発、差はノイズ）。200K 件の新規 insert 後 checkpoint #2 は 0.69s →
  0.62s (-10%、dirty 1 shard のみマージ）。update-only 後 checkpoint #3 は 0.64s →
  0.42s (-34%、全 clean で walk のみ。base は sorted region 再構築分が残る）。
  定常運用（dirty が hot shard に偏る・update 主体）で効果が最大化する。
- 別途、50M/200M のタイト cgroup での 200K x 1KB bulk_load で
  `bulk_load_throttle_events=169` を確認（throttle の end-to-end 作動証明）。
  `VMemKVStatistics::bulk_load_throttle_events` として公開した。
