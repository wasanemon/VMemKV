# MAP_SHARED vs MAP_PRIVATE 再検証プラン

## 経緯（前回までの整理）

- `mmap_shared_or_private.md`(8/25執筆, 未コミット)が採用の根拠。`4a7084c`(8/21, MAP_PRIVATE)実測で
  ltm/64KB・32並行書き込みのcheckpointが**51%スループット劣化・60秒非完走**。MAP_SHARED+msync案は
  checkpoint自体を軽量化(72ms/13.7GB)する一方、実メモリ圧迫下で**-12%〜-19%の恒常タックス**。
  この比較の上で「タックスは許容範囲」と判断し`586f067`(8/26)でMAP_SHARED+msyncを採用。
- その2日後(8/28)に本item(TODO.md item 6)の症状(checkpoint/reorg contentionが完全ハング)が発見され、
  以降の調査は主にLTM `bulk_load()` の投入フェーズ側のスラッシング(memory.high同期reclaim問題、
  ratio-overrideバグ等)を掘っていた。**採用根拠そのもの(-12%〜-19%という数字、checkpoint軽量化の
  持続性)は8/25以降一度も現HEADで再検証していない。**

## 今確かめたいこと(3項目、範囲を絞る)

1. **Insert QPS**(通常時、比較の基準値)
2. **checkpoint実行中のInsert QPS低下率**
3. **checkpointの所要時間**(タイムアウト30秒)

対象: 現HEAD(MAP_SHARED, 全修正込み) vs `6c41d6d`(8/23, 586f067直前の最後のMAP_PRIVATE状態)。

## 課題

- `6c41d6d`と現HEADの間にはAppendRegion縮小・WAL segmentation・ratio-overrideバグ修正など
  mmapモードと無関係な差分が多数入っている。純粋なA/Bではないが、8/25の比較も「実際の当時のコード」
  同士の比較だったので方法論としては踏襲する。
- 今回発見した「ratio=8.0は投入(bulk_load)自体がmemory.highスラッシングで詰まりうる」問題があるため、
  投入に失敗した試行はcheckpoint測定のノイズになる。**投入成功を確認してから計測に入る**必要がある。
- 既存の`checkpoint_contention`プローブの内部タイムアウトは`kReorgTimeoutSeconds=60`で固定。
  30秒での打ち切りにするには定数変更 or 引数化が必要(小さい変更)。
- 「1回測って終わり」は前回の反省点(ratio=1.6のコイントス事例)。**各条件で複数回試行**する。

## 次の実験プラン

1. `kReorgTimeoutSeconds`を30秒に変更する(or CLI引数化)一行修正。
2. 現HEADと`6c41d6d`を同一AWSインスタンス上でそれぞれビルド。
3. `--reorg-probe --scenario=ltm --value-size=64KB --mode=checkpoint_contention`
   (実LTM cgroup: `MemoryHigh=1GiB`/`MemoryMax=2GiB`/`MemorySwapMax=1TiB`)を両バージョンで
   複数回(目安5回)実行し、`isolated_write_tps`(1)・`concurrent_write_tps`(2, 1との差分%)・
   `checkpoint_elapsed_sec`/`timed_out`(3)を記録。
4. 投入(bulk_load)がタイムアウトした試行は計測対象から除外し、別途「投入成功率」として記録する。
5. 結果を表にまとめ、8/25時点の数字(51%劣化・非完走 / -12%〜-19%)と比較する。

## 実験結果(2026-08-29)

### 方法論の見直し: setupフェーズの隔離

当初64KB/ratio=8.0で計測を試みたが、**population(投入)自体が両mmapモードで等しく
memory.highスラッシングに陥り、180〜400秒経っても計測フェーズに到達しない**ことが判明
(private/sharedとも同様に失敗 -- MAP_SHARED固有の問題ではないことも同時に確認)。

ユーザー提案により方式変更: `run_checkpoint_contention()`に`VMEMKV_BENCH_PREBUILD_ONLY`
(投入+baseline checkpoint+pre-churnだけ行って終了)と`VMEMKV_BENCH_REUSE_PREBUILT`
(既存manifestがあればそこから直接リカバリして計測へ進む)を追加(`076de82`)。
無制約環境でprebuildし(19秒/17秒で完了)、そのディレクトリを`cp -a`で複製してから
cgroup制約下の計測トライアルを実行する方式に変更。ratio=1.5・1KBで各3試行、計測は
毎回9〜17秒で完走(vs 修正前は同スケールでも300秒超えでタイムアウト)。

### 結果 (ltm/1KB, ratio=1.5, 32並行書き込み, 3試行平均)

| | isolated_write_tps | concurrent_write_tps | checkpoint_elapsed_sec |
|---|---|---|---|
| **shared** (MAP_SHARED+msync, 現HEAD) | 41,037 | 61,326 (**逆転**, +49%) | **0.40s** |
| **private** (MAP_PRIVATE+pwrite, `6c41d6d`) | **104,337** | 20,457 (-80%) | 3.29s |

生データ:
```
shared  t1: isolated=39330  concurrent=65280   checkpoint=0.51s
shared  t2: isolated=42574  concurrent=59671   checkpoint=0.34s
shared  t3: isolated=41208  concurrent=59026   checkpoint=0.34s
private t1: isolated=107965 concurrent=20242   checkpoint=2.52s
private t2: isolated=105099 concurrent=27147   checkpoint=2.87s
private t3: isolated=99948  concurrent=13982   checkpoint=4.49s
```

### 解釈

- **checkpoint速度**: shared が private の**約8倍速い**(0.40s vs 3.29s)。8/25の"msyncは軽量"
  という前提は現HEADでも再確認できた。
- **isolated write TPS**: private の方が **shared の約2.5倍速い**(104K vs 41K)。これは
  8/25には直接測定されていなかった軸で、新しい知見。
- **shared の isolated/concurrent 逆転**: 3試行とも一貫して concurrent > isolated という
  直感に反する結果になった。isolated フェーズは「複製直後で未ウォームなページへの
  ランダムアクセス」を含むため、コールドページフォールト分だけ不利になっている可能性が
  高いが、確定的な原因は未特定(private では同じ逆転が起きていない = shared 固有の何か)。
  **この逆転のせいで、shared の「isolated_write_tps」を素の書き込み性能の基準値として
  信頼するのは危険。**
- 8/25の「-12%〜-19%の恒常タックス」という具体的な数字は、今回とスケール・比較対象が違う
  ため直接比較できないが、**「checkpoint中にQPSが落ちる」こと自体は private でも
  shared でも起きている**(sharedはisolated基準の解釈が怪しいため率は保留)。

### 次のアクション candidate

1. shared の isolated/concurrent 逆転の原因を切り分ける(ウォームアップ有無で isolated
   フェーズを2回計測する、など)
2. 64KB値サイズでも同じ prebuild+reuse 方式で計測する(投入自体は無制約なら高速なはず)
3. ratio を変えて(0.5, 1.0, 2.0)傾向が変わるか確認する
4. 8/25の-12%〜-19%を再現する条件を特定し、直接比較できる形に揃える
