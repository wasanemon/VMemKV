# アウトライン作成時の根拠と留意点

確認日: 2026-09-17

対象: ローカル HEAD `b5bd271`。実装の変更・ベンチマーク実行は行っていない。

## 構成案の根拠

| 情報源 | 確認した内容 | アウトラインへの反映 |
| --- | --- | --- |
| [研究シナリオ](https://docs.google.com/document/d/1s7NWZoqebnq0dfh1O5n4k4OYWNX9eyKN0FKRpyNbhXg/edit)（更新日時 2026-09-14） | Read-path Trifecta を中心的な新規性として位置付けている | Trifecta を独立章にし、T1/T2 などを実現のための設計として説明 |
| [高レベル設計](../implementation/vmemkv/docs/specification/high_level_design.md)・[低レベル設計](../implementation/vmemkv/docs/specification/low_level_design.md) | 二層構造、WAL、checkpoint、保守処理、機能上の範囲 | 全体構造、永続性、適用範囲の章立て |
| [read_path.hpp](../implementation/vmemkv/src/vmemkv/read_path.hpp) | 操作・サイズ・常駐状態別の実際の選択規則 | 第3章の表。シナリオの Scan の説明はそのまま転記しない |
| [t2_flat_file.cpp](../implementation/vmemkv/src/t2_flat_file/t2_flat_file.cpp) | MAP_SHARED と追加のマッピングの構成 | 共有可視性とレコードの並行更新を区別 |
| [ベンチマーク行列](../implementation/vmemkv/benchmark/common/benchmark_matrix.sh)・[測定コード](../implementation/vmemkv/benchmark/bench_kv.cpp) | 比較対象、固定読み取り方針、測定操作 | 評価の問いと利用できる比較を対応付け |
| [2026091416 の結果](../benchmark_results/2026091416/) | CRUD・Scan・YCSB-E、保守処理・自動分割の測定ファイル | 既存データと追加評価候補を区別 |

## Slack の参照範囲

- [2026-04-06 からの設計議論](https://kawashima-ken.slack.com/archives/C08B41HFC85/p1775465672323359): 新しい larger-than-memory KVS と LineairDB の役割分担、ミニマルなコードベースという動機を確認。
- [2026-04-13 の参考文献案内](https://kawashima-ken.slack.com/archives/C08B41HFC85/p1776056473044599): mmap の課題、vmcache、LMDB が検討対象として挙げられている。
- [2026-05-26 からの議論](https://kawashima-ken.slack.com/archives/C08B41HFC85/p1779779641021629): TLB shootdown の調査提案、T1/T2 を組み合わせて評価する方針、初期実装での mutex に関する報告を確認。現実装のボトルネックが実証されたものとは扱わない。
- 指定の起点スレッドと取得できたチャンネル履歴を確認したが、全期間・全返信の網羅調査はしていない。Helios の設計・評価の議論は本案の根拠に含めていない。

## シナリオから修正・限定する点

### 1. Scan の方針は現実装に合わせる

シナリオには「大レコードの Scan は MADV_SEQUENTIAL、小レコードの Scan は MADV_RANDOM」とあるが、`BaseMappingSelector::for_scan()` は小レコードに MADV_SEQUENTIAL、大レコードに通常の mmap を選ぶ。第3章は後者に合わせた。

実装には主 mmap・通常の読み取り mmap・Sequential の mmap の三つに加え、pread 用のハンドルがある。「三種類の API」「三つの分岐」「三つのマッピング」を同一視しない。名称の定義は著者間で整理する必要がある。

### 2. 複数 mmap の可視性と操作の一貫性を分ける

「複数 mmap への書き込みは同期されないので mutable な構造では利用できない」という一般化は避ける。MAP_SHARED は同じファイル領域をマップする相手への変更の可視性を提供する。ただし、複数バイトからなるレコードの読み書きの原子性まで保証するわけではない。[Linux mmap(2)](https://www.man7.org/linux/man-pages/man2/mmap.2.html)

VMemKV の base の不変性は、参照するレコードの読み取りと更新の競合を単純化する設計として述べる。領域回収時の参照寿命の扱いは別途確認が必要。「immutable と KV 分離があらゆる経路選択方式の必要条件」とまでは主張しない。

### 3. 常駐判定と費用を過大評価しない

mincore はその時点での常駐状態を返し、直後のページ追い出しを防がない。したがって「ページフォルトを確実に排除する」「無料のページ管理」とは書かない。常駐確認のシステムコールにも費用がある。[Linux mincore(2)](https://www.man7.org/linux/man-pages/man2/mincore.2.html)

現実装の pread は通常のファイル記述子による読み出しであり、これをカーネルバイパスや Direct I/O と説明しない。

### 4. vmcache と LeanStore の比較軸を明確にする

vmcache も仮想メモリを活用するが、ページ取得・追い出しの制御を DBMS に保持する。「OS の機能を使うかどうか」より、「どの管理責務を OS に委譲するか」が比較軸になる。[vmcache の著者リポジトリ](https://github.com/viktorleis/vmcache)、[LeanStore 著者論文 §2.1](https://www.vldb.org/pvldb/vol17/p4536-leis.pdf)

[CIDR 2022 の mmap 論文](https://db.cs.cmu.edu/mmap-cidr2022/)は、mmap による DBMS の正しさと性能の課題を扱っている。VMemKV が応答する課題と、対象範囲を限定している課題を区別する。

### 5. 比較対象の永続性は実装版を確認する

[rivals.md](../implementation/vmemkv/docs/rivals.md) は LeanStore の応答が fsync に先行すると記すが、現在の [leanstore_store.hpp](../implementation/vmemkv/src/rivals/leanstore_store.hpp) は更新後に `await_group_durable()` を呼ぶ。古い文書の記述を評価条件として転記しない。各測定時の revision、アダプター、永続化設定を対応付ける。

### 6. OS に委譲する範囲を限定する

現実装には索引の再編成と T2 の領域回収がある。「compaction をすべて OS に任せる」「保守処理がない」とは書かない。T1 をメモリ上に持つ設計と、OS によって常駐が保証されることも区別する。

## 既存結果について今回確認したこと

`benchmark_results/2026091416/results_*.json` の四ファイルを読み、以下を確認した。

- in-memory 8 B / 1 KB、LTM 1 KB / 64 KB の条件が存在する。1 KB / 64 KB の測定名には 20% の 8 B 値の混合が示されている。
- 行数は順に 231、231、297、264。四ファイルの全 1,023 行で `error_occurred` が真の行はなかった。これは性能・比較条件・統計的妥当性の検証ではない。
- 操作は Insert、Get、Update、Delete、Scan、YCSB-E。スレッド数の集合は 1、4、16、32。
- `repetitions` はすべて 1。既存の集計値だけから誤差や p99 を示せるとはしない。
- LTM のメタデータには 1 GiB のメモリ予算と 8.0 のデータ量係数がある一方、`cgroup_memory_limit_bytes` は -1 である。実際の制限は実行スクリプト・ログと突き合わせる。
- 64 KB の LTM ファイルは revision `b84f4a550fc3`、他の三ファイルの context は `047300bf27de`。統合結果の来歴はファイル単位の context だけで確定しない。
- Baseline / Bloom / Bloom-T1InlineValue はすべて通常の読み取り方式を使う。読み取り方針の評価は Bloom-T1InlineValue と同じ補助最適化を持つ ReadRandom / ReadSeq を対応させる。
- 保守処理・自動分割のファイルは存在を確認した段階で、値の分析は未実施。長時間 churn、回復時間、強制障害試験の充足は未確認。

今回、性能倍率の算出・勝敗の集計・新規性の網羅調査・正しさの完全な監査は行っていない。アウトラインはそれらを後続の根拠整理に接続できる形で作成した。

## 追記: mmap 論文の4つの問題への対応（2026-09-17）

ユーザーの問題提起を受け、[mmap 論文](../must_read_papers/andy_mmap.pdf)第3章と現在の実装を照合した。以下は設計・ソースの確認であり、実装の完全な正しさの証明ではない。テストの再実行や新しい実験は行っていない。

| 問題 | VMemKV の対応 | 残る範囲 |
| --- | --- | --- |
| 1. トランザクションの安全性 | base の既存レコードを書き換えず、checkpoint の同期・公開と WAL の永続化・再生を組み合わせる | 単一操作の永続 KVS を対象とする設計。複数操作のトランザクションは未提供。任意の障害点での安全性は別途検証が必要 |
| 2. I/O による停止 | 大きい base レコードの Get は mincore で常駐を調べ、非常駐なら pread。Get/Scan の先読みも使い分ける | pread 自体は同期 I/O。常駐確認後の追い出し、小レコード・Scan・tail のページフォルトは残る |
| 3. エラー処理 | WAL、manifest、T1 checkpoint にチェックサムがあり、明示的なログ I/O の失敗も処理する | T2 の値のチェックサムと SIGBUS の処理は src/include の確認範囲では見つからない。ログ破損検出を値データの保護と同一視しない |
| 4. 性能 | 読み取り経路の選択で固定方針より高速な測定条件がある | ページテーブルの競合・追い出し・TLB shootdown 自体を解消したとは言えない。条件別の性能改善と OS 内部の限界の解消は別の主張 |

実装上の根拠:

- [checkpoint_coordinator.hpp](../implementation/vmemkv/src/vmemkv/checkpoint_coordinator.hpp): 更新対象の凍結、進行中の更新の完了待ち、msync、T1 checkpoint/manifest の公開、base 境界の前進。
- [write_path.hpp](../implementation/vmemkv/src/vmemkv/write_path.hpp)・[vmemkv_impl.hpp](../implementation/vmemkv/src/vmemkv_impl.hpp): base への更新の追記化、WAL 永続化待ち、起動時の再生。
- [read_path.hpp](../implementation/vmemkv/src/vmemkv/read_path.hpp): 先読み方針の選択と mincore/pread の分岐。pread の失敗時は主 mmap の経路へ戻る。
- [wal.cpp](../implementation/vmemkv/src/wal/wal.cpp)・[checkpoint.cpp](../implementation/vmemkv/src/checkpoint/checkpoint.cpp): チェックサム検証。[ValueRecordHeader](../implementation/vmemkv/src/t2_flat_file/t2_flat_file.hpp) に値のチェックサムはない。
- [test_crash_recovery.cpp](../implementation/vmemkv/tests/test_crash_recovery.cpp): 再起動、WAL 末尾破損、checkpoint と更新の競合などのテストが存在する。正常な破棄・再構築や破損注入を含むテストであり、任意の瞬間の電源断を網羅した試験とは扱わない。

追記時に算出した性能例: [2026091416 LTM 64KB](../benchmark_results/2026091416/results_ltm_64KB.json) の Get/Hit、Uniform、32 threads、値は64KBと20%の8B混合。`Bloom-T1InlineValue` は 61,060.8 items/s、同じ補助最適化を持つ ReadRandom は 14,694.5、ReadSeq は 33,698.5。比率はそれぞれ約4.16倍・1.81倍。既存の各1回の測定値の比較で、統計的な確定値や全条件での優位性ではない。たとえば同ファイルの1 thread の Scan/Uniform では、Trifecta は ReadSeq の約0.76倍だった。

[2026-08-05 の調査](../implementation/vmemkv/docs/benchmark/20260805_ltm_get_hit_profiling.md)には TLB shootdown が支配的でなかったという報告があるが、MAP_PRIVATE を使う旧実装・1 thread の条件である。現実装の多数スレッドで mmap の性能問題が解消した根拠には使わない。
