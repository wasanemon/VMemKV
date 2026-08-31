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

## 実験結果(2026-08-30) -- スケール修正版

### 前回(8/29)の数字は誤ったスケールで測っていた

`ratio`はホストの実メモリ(247GB)やcgroupの`memory.max`に対する倍率ではなく、正規の
`benchmark_matrix.sh`では**固定1GiB**(`VMEMKV_CONTEXT_memory_budget_bytes`)に対する倍率
(LTMは`ratio=8.0`固定)。8/29の`ratio=1.5`は実メモリ相対で計算してしまっており、意図しない
スケール(数百GB〜数千万キー)で測っていた疑いがある。正しくは
`VMEMKV_CONTEXT_memory_budget_bytes=1073741824` + `VMEMKV_BENCH_TARGET_RATIO=8.0`で
**固定8GiB corpus**(1KBなら key_count=8,259,552 -- 8/21の旧ベースラインと一致)。
**以後、ratioは常に8.0固定とする。**

### 計測を阻んでいた2つの副問題(今回切り分け・対処)

1. **defragment()の即時自動発火**: `bytes_used_at_last_defragment_`の初期値が0のため、
   `defrag_growth_over_threshold()`が新規storeでは初回から常にtrueになり、populate直後に
   ほぼ即defragmentが自動発火する。defragmentは既知の低速問題(future work)なので、
   `defragment_internal()`を一時的にno-op化(`bytes_used_at_last_defragment_`更新と
   `tail_entries_.drain_and_clear()`だけ行う)して両コードベースから除外して計測。
2. **populate中の自動checkpoint発火がprivateで極端に遅い**: `bulk_load()`の各keyごとに
   `maybe_reorganize_if_needed()`がT1 append領域のhard threshold超過を検知して
   `reorganize_internal(Checkpoint)`を自動発火しうる。privateのcheckpointは同時書き込み
   負荷下で壊滅的に遅い(後述の-91.6%)ため、populate中に繰り返し自動発火すると
   populateそのものが数十分〜完走不能になる(2回のgdbで同一スタックフレームを確認)。
   `VMEMKV_SUPPRESS_AUTO_REORG`環境変数を追加し、setup(populate+初回checkpoint+warm-up)
   期間中だけ**自動Checkpointのみ**を抑制(T1Only/Defragmentの自動発火は温存 -- T1 append
   領域は8.26Mキーに対し容量2^21しかなく、完全抑制すると領域が溢れてハングするため)。

### 結果 (ltm/1KB, ratio=8.0固定, key_count=8,259,552, 32並行書き込み, defragment無効化)

| workload=insert | isolated_write_tps | concurrent_write_tps | checkpoint_elapsed_sec |
|---|---|---|---|
| **shared** (MAP_SHARED+msync, 現HEAD) | 73,236 | 72,103 (-1.5%) | 12.47 |
| **private** (MAP_PRIVATE+pwrite, `6c41d6d`) | 101,695 | 8,518 (**-91.6%**) | 9.22 |

isolatedはprivateの方が速い(mmap経由でないぶん素の追記が軽い)が、checkpoint同時実行下の
書き込みスループットはprivateが壊滅的、sharedはほぼ無傷 -- 8/25の採用根拠を、defragmentの
混入を排除した正しいスケールで再確認できた。

`update_warm`(全キーをupdate()でtailへ移す既存データワークロード)はsharedでは成功
(isolated=40,662 concurrent=48,010 [+18%逆転、8/29のsharedと同傾向] checkpoint=12.44s)。
privateは`VMEMKV_SUPPRESS_AUTO_REORG`適用後もT1Only自動reorgの側で同様のスタック停滞
(2回のgdbで同一フレーム)が再現し、40分経過してもpopulate完了(初回checkpoint)にすら
到達しなかったため、**private側のupdate_warm比較は今回見送り**(T1Only側の深追いは
defragmentと同じくfuture work扱い)。

### 議論: なぜprivateでもcheckpointは完走したのか

当初「LTMならページフォルト連発でcheckpointが一切終わらないはず」という想定だったが、
上記の`insert`ワークロードでは私9.22秒で完走した。理由は**insertが常に新規キーのみを
追記するホット(直近書き込み済み)データしか触らない**ため: checkpointのpwrite()は
「スワップアウト済みのソースページを同期的にフォルトインする」必要がなく、8/25の
"60秒非完走"ほどには苦しまない。

8/25の壊滅的な非完走は`ltm/64KB checkpoint_contention`(populate後に corpus の25%を
**ランダムに** update()でpre-churnしてから計測)で観測されたもので、これは corpus 全体に
散らばった**コールドな**(スワップアウトされ得る)データにランダムアクセスする、insertとは
質的に異なるワークロード。今回`update_warm`(全キーをupdate()でtailへ移す、insertより
さらに徹底してコールドデータ全体を触る設計)がprivate側で深い停滞を踏んだのは、
**むしろこの仮説と整合的**(コールドデータへの全面アクセスが絡むと、まさに壊滅的に遅くなる
挙動が再現している)。今回計測を諦めたT1Only停滞も、根っこは同じ「同時書き込み+コールド
データアクセス下でreorg機構全体が破綻する」という8/25の発見の延長線上にあると考えられる。
