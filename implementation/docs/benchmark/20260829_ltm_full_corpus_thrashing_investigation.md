# 2026-08-28〜29 LTM full-corpus checkpoint()/write-path stall 調査ログ

`run_maintenance_contention_probe.sh`の`checkpoint_contention`/`reorg_contention`スポットチェック
(フルコーパス、32並行書き込みスレッド、LTM cgroup)がltm/1KB・ltm/64KBの両方で完全にハングし、
プローブドライバの300秒外部タイムアウトに達するまで測定値が全く出ない、という症状の調査記録。

## 症状と初期調査

`run_defrag_scaling_probe.sh`のLTMスイープと`run_checkpoint_throughput_probe.sh`の`ltm/1KB`/
`ltm/64KB`地点も同じパターン(フル〜ほぼフルコーパスで発生)。`benchmark_results/2026082101`
(commit `4a7084c`)ではこれらの地点が5.2-5.68秒で完走していた記録がある。

2026-08-28のcheckpoint自動トリガー修正(`6aec324`/`db31a6b`/`a3bb115`)が原因ではないことをAWS A/Bで
確認: 修正前後どちらも同じ300秒ハングを再現。さらに`4a7084c`まで遡ってbisectを試みたが、
`benchmark_results/2026082101`を生成した当のコミット自体が同条件で再現できず(カーネル/AMI/
`MemorySwapMax`を厳密に一致させても再現不可)、bisect対象となる「導入コミット」が存在しないと結論。

## gdbによる直接確認

`4a7084c`(commit直後): メインスレッドは`write_entry_lockfree()`→`T2FlatFile::append_default()`の
memcpy内でブロック(通常の書き込みパス、checkpointではない)。`reorg_worker_`スレッドは
`checkpoint_internal()`(当時はmsync()ではなくpwrite()per-record方式)の`pwrite64()`内でブロック。
両スレッドとも本物の(が非常に遅い)進行中であり、デッドロックではない。

現HEAD(`c13fe40`、msync()方式): gdbプロセス自身(bench_kvとは無関係な別プロセス)が自分自身の
67MB相当のメモリタッチだけで9分以上`mem_cgroup_handle_over_high()`内に留まった。cgroupの
`memory.pressure`は`full avg10=92.41`(直近10秒の92%、全タスクがスワップ待ちで停止)。
`memory.stat`の`pgscan_kswapd=0`/`pgscan_direct=1,322,992`——バックグラウンド回収は皆無で、全て
同期直接回収経由。`memory.high`が設計通りに動作しているだけで、OOM近辺ではない
(`memory.current`は`memory.high`をわずか12%超過、`memory.max`にはほど遠い)。

文献: Johannes Weinerの2020年patch("mm: memcontrol: asynchronous reclaim for memory.high")が
同じ症状を明記——`memory.high`超過cgroupにはデフォルトで専用バックグラウンド回収スレッドが無く、
超過分は常にそのスレッド自身の同期直接回収で処理される。

## target_ratioスイープと「8倍オーバーサブスクリプション」仮説の棄却

`VMEMKV_BENCH_TARGET_RATIO`を8.0から1.0まで振ったが、**1.0を含む全ての比率でハング**。つまり
「8倍だから」という規模依存の崖ではなく、`memory.high`をわずかでも超過した時点で回収が追いつかず
発散する、書き込みバースト対dirty-page回収速度のレースだと判明。

dirty-pageバックログ仮説は`file_dirty`が常に64KB未満(スタール中も)であることから否定。
`anon`メモリが際限なく成長し続ける一方`file`(T2のmmap)は数百MBから数MBまで急減——RSSの99.9%が
`anon`(`smaps_rollup`実測)。

## 根本原因: T1Index::AppendRegionの固定容量が軽い占有率でもほぼ全ページ常駐化

`T1AppendCapacityLog2=22`時代、`AppendRegion`は`4,194,304 * 56 = 224MB`を`reorganize()`サイクルごとに
無条件mmapしていた。オープンアドレッシング配置(実質一様ランダム)のため、258,000エントリ挿入
(6.15%の占有率)だけでRSSが224MBの98.4%まで到達することをスタンドアロンprobeで確認
(birthday-paradox効果)。EBR回収が追いつかない状況下では複数世代の`AppendRegion`が同時生存し、
このコストが積み重なる。

`madvise(MADV_DONTNEED)`をmunmap前に挟む案は、素のmunmap()自体が既に7-8msで全体を解放しており
効果なしと確認(棄却)。

## 適用した修正と効果

`T1AppendCapacityLog2`を22→21に縮小(measured: 8バイト値・16スレッド・3M insertでの実測ピーク
`append_size()`=1,198,470に対し、2^21のhard threshold(1,992,294)は約66%の余裕を残す一方、
各世代のフットプリントは224MB→112MBに半減)。

AWS再テストの結果は完全な解決ではなかった: `target_ratio=1.0`(容量ちょうど)は3回中1回のみ回復、
2回はやはり180秒タイムアウト——ゼロヘッドルームでのタイミング依存レースであることが裏付けられた。

## 決定的な発見: ratio上書きバグ

この調査全体を通じて使っていた`VMEMKV_BENCH_TARGET_RATIO`は、`reorg_probe::run()`内の
`::setenv("VMEMKV_BENCH_TARGET_RATIO", "8.0", 1)`(overwrite=1)によって**常に8.0へ強制上書き**
されていた。つまりこの調査で「ratio=1.0を試した」つもりの試行は全て実際にはratio=8.0で走っていた。
`overwrite=0`に修正後、初めて正真正銘の`target_ratio=1.0`が14秒で完走(RSSは750-1040MBで安定、
`hard_stall=0`)。修正後に真の`target_ratio=8.0`を再テストすると従来通りハングし、AppendRegion
縮小自体は無効化されていないことも確認。

## 境界のマッピング(修正後、正しいratioで実施)

二分探索の結果、明確な閾値ではなく`target_ratio≈1.5〜1.65`付近の確率的な際どいゾーンと判明:

```
1.0  -> success,  14s
1.5  -> success,  47s
1.6  -> MIXED (2試行中1回成功、1回ハング)
1.65 -> hang, 180s
1.7  -> hang, 180s
1.75 -> hang, 180s
2.0  -> hang, 180s
8.0  -> hang, 180s
```

`target_ratio<=1.5`で信頼できる成功、`>=1.65`で信頼できる失敗。これは文献が指摘する
「memory.high超過時点でのdirty化対回収レースの結果次第」という性質と整合する。

## 副産物として実装されたもの

- `append_region_live_count`/`append_region_peak_count`統計(`T1Index`/`VMemKVStatistics`) —
  現在も本番コードに残存、恒久的な診断用カウンタとして維持。
- `run_steady()`のフェーズ別テレメトリ(`VMEMKV_STEADY_TELEMETRY`)は本調査完了後に削除済み
  (調査終了、必要なら本コミットから復元可能)。
