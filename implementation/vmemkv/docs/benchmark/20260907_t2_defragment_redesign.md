# 2026-09-07 T2 defragment() 再設計方針

T2 の dead space（上書きで追い出された旧 record・delete 済み key の record）を回収する
仕組みは現在存在しない（round 1 で no-op 化、round 3 で API ごと削除）。
Update/Delete を繰り返す実運用では T2 フットプリントが単調増加し続けるため、
「実用可能な larger-than-memory」の看板に対する最大の残存 gap である。
本ドキュメントは旧実装が実測で失敗した理由の整理と、再設計の方向・受入基準を定める。
実装自体は下記のフェーズ計画に従う。

## 1. 現状の制約（2026-09-07 時点のアーキテクチャ）

- T2 は単一永続ファイル上の単一 `T2Memory`（プロセス寿命中不変、世代なし、`MAP_SHARED`）。
  追記は `bytes_used` の atomic append、収まる更新は in-place、収まらない更新は末尾追記 +
  T1 offset 書き換え。dead 判定・回収の機構も統計もない。
- T1 は `ShardedT1Index`（範囲シャード＋背景ワーカープール＋キュー）。T1 offset は
  `payload_bits`（offset＋block-count）として T1 checkpoint にそのまま載る。
- 永続化の順序制約: T2 バイトの `msync()` → T1 offset の公開 → checkpoint が新 offset を
  取り込む、の順でなければクラッシュ時に checkpoint が未 durabilize の offset を指す。
  逆に WAL replay は key/value 単位で再追記して T1 を張り替えるため、クラッシュ前の
  relocation は replay によって素直に supersede される。すなわち compactor は
  「新位置のバイトを durabilize してから T1 を張り替え、旧位置の解放は新 offset を
  取り込んだ checkpoint の後」とさえ守れば、既存の回復手順と追加の整合性なしに共存できる。
- 旧実装が使っていた `T2Memory` 世代・`swap_memory()`・`capture_watermark_` 等の機構は
  round 3〜4 で削除済みであり、再設計はそれらを復活させない（下記 3. の方式は世代を要しない）。

## 2. 旧実装が失敗した理由（実測ベース）

数値は `20260823_defragment_scaling_measurements.md`（逐次書き換え・単一スレッド版、
`20260823_maintenance_ops_priority_triage.md` のトリアージ前提）より。

- 単独実行のスループットは Insert に対して `8B:0.019x, 1KB:0.136x, ltm/1KB:0.319x` と
  余裕があるが、`64KB/LTM` で `r=1.69` と発散する。単一スレッドの全面書き換えは
  Insert レートに追いつけない値サイズが存在する。
- 32 writer 並行下では `in_memory/1KB:-64%, ltm/1KB:-79%, ltm/64KB:-98%` の concurrent-write
  劣化を伴い、LTM 2 点は 60 秒タイムアウトで未完。全面書き換え＋writer-stop（`stop_writers`
  窓）を前景パスと競合させる構造が根本原因であり、並列化やしきい値調整では治らない
  （トリアージ結論 2. と同旨）。
- 教訓: compaction の全量をアクセススレッドの経路で実行しないこと・writer を止めないこと。
  これは T1 シャーディングが背景ワーカープール＋シャードスコープ背圧で解決したのと
  同じ原則であり、T2 側も同型にする。

## 3. 再設計の方向：セグメントスコープの増分 background compaction

T2 ファイルを固定サイズのセグメント列とみなし、一度に 1 セグメントだけを背景で圧縮する。
全面書き換えを捨て、以下の性質で上記 2. の失敗を回避する。

- writer-stop なし: relocation は「live record を tail へ追記 → T1 offset を CAS で張り替え」
  の繰り返しで、append と同じ通常パスだけを使う。compaction 中も全 writer は無停止。
- 追いつき可能性: 1 セグメントの live バイト量は有界（セグメントサイズ以下）であり、
  追記レートとの競争はセグメント単位で完結する。全面書き換えのような O(corpus) の窓を持たない。
- 対象選択: セグメントごとの live 比率が低いものから処理する。live 比率の把握は T1 全走査
  （offset 集合の収集）を要するため、常時ではなく compaction 開始時の計測＋ perpetuum 間の
  減衰推定に留める（フェーズ 0）。
- 旧位置の解放: 新 offset を取り込んだ checkpoint の完了後に旧セグメント範囲を
  `fallocate(PUNCH_HOLE)` する（小 value が大半の workload ではブロック粒度の hole-punch が
  効きにくいという旧知見があるため、効果は値サイズ分布に依存することに注意。最悪の場合は
  ファイル末尾の切り詰めのみでも単調増加は止まる）。
- 並行更新との競合: relocation（旧 offset→新 offset の T1 張り替え）と前景の update が
  同一 key で競合したら、後勝ち（last-writer-wins）で正しい。T1 の put は upsert のため、
  新旧どちらの順序で着地しても live 集合は保たれる。必要なのは「張り替えた先のバイトが
  durable であること」（1. の順序制約）のみで、compactor 側の追加ロックは要しない。
- 背景実行基盤: `ShardedT1Index` の worker プール＋キューと同型の専用プールを用意し、
  compaction をアクセススレッドに代行させない（2. の教訓の直接適用）。

## 4. フェーズ計画

- フェーズ 0（観測）: T2 フットプリント内訳の公開（`bytes_used` に対する live 推定・
  dead 推定）。compaction トリガー条件の設計入力であり、論文の space amplification 評価
  そのものでもある。compactor 本体なしでも独立に価値がある。
- フェーズ 1（単一セグメント compaction の正しさ）: writer-stop なし・単一スレッド
  compactor での relocation＋T1 張り替え＋クラッシュ回復の round-trip テスト。
  性能は問わない（正しさのゲート）。
- フェーズ 2（背景化＋背圧）: 専用 worker プール化、セグメント選択、前景 TPS への干渉度測定。
  2. の表と同条件（`ltm/1KB`・`ltm/64KB`、32 writers）で劣化率を比較する。
- フェーズ 3（空間回収の実効化）: hole-punch／末尾切詰の実装と、長時間 churn workload での
  フットプリント頭打ちの実証。

## 5. 受入基準（フェーズ 2 完了の定義）

- `ltm/1KB`・`ltm/64KB` の concurrent-write contention probe で、compaction 実行中の
  concurrent-write TPS 劣化が 20% 以内（旧実装の 79〜98% に対して）。
- 同条件で compactor 単体の処理量が Insert レートを上回る（旧 `r > 1` の逆転）。
- 長時間 churn（update/delete 混在）で T2 フットプリントが有界に留まる（フェーズ 3）。
- 全 271+ テスト＋TSan が緑のまま（既存の並行性テスト群を壊さない）。

## 6. 非目標・未決事項

- T1 の Ordering Fragmentation（append 未整列・T2 物理 offset と key 順の乖離）は本件の
  対象外（TODO 5 の scan 側読み順変更で扱う）。
- セグメントサイズ・live 比率しきい値・worker 数は実測で決める（本ドキュメントでは固定しない）。
- hole-punch の実効性はファイルシステム・値サイズ分布に依存するため、フェーズ 3 の実測で
  可否を判断する（小 value 中心では効かない可能性を既知として残す）。
