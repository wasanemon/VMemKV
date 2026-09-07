# T1の範囲シャーディング設計

`reorganize()`/`checkpoint()`のO(corpus)コスト([`t1_index_structure_decision.md`](t1_index_structure_decision.md)参照)を、
単一の`T1Index`を独立したK個のシャードに分割することで解消する設計。各シャードは既存の
`src/t1_index/t1_index.hpp`をそのまま(変更なしで)使う。

## 全体構造

T1は2層になる:

- **ディレクトリ**: 境界キーでソートされた配列。各区間を1つのシャードへマッピングする。
- **シャード**: `T1Index<Config>`のインスタンスがK個。各シャードは自分が担当するキー範囲内の
  データだけを持つ、独立したsorted region + append regionのペア。

キー空間はレンジパーティション(ハッシュではない)する。理由: シャードが常に非重複かつ
順序を保つため、Scanがシャード跨ぎでもk-wayマージを必要とせず、交差するシャードそれぞれの
`scan()`結果をシャード順に連結するだけで全体がソート済みになる。

初期状態はK=1(既存の単一T1Indexと同一の挙動)で、データ量が増えるにつれ動的にsplitする。

## ディレクトリの並行性

ディレクトリはT1Index自身がすでに使っているRCU的パターン(`AppendGeneration`/`SortedRegion`の
atomicポインタ差し替え+epoch-basedな回収)をそのまま1段上に適用する。ディレクトリはイミュータブルな
オブジェクトで、split/merge時にのみ新しいインスタンスへatomicに差し替えられる。読み取り側は
操作開始時に1回ディレクトリをロードし、そのシャードへの参照を操作完了まで使い続ける。

## Splitting

`reorganize()`はすでに全生存エントリをマージ・重複排除したソート済み配列(`merged`)を計算している。
splitはこの結果を再利用するが、`merged`をそのまま使うだけでは書き込みロストが起きるため、
2段階のprotocolを踏む。

**書き込みロストが起きる理由**: `reorganize()`は`merged`を計算する**前**に、旧active regionを
immutableへfreezeし新しいactive regionをpublishする(既存の二重バッファリング)。つまり`merged`が
確定した時点で、対象シャードは既に新しい(空の)active regionへの書き込みを受け付け始めている。
この新しいactive regionへ書き込まれたエントリは`merged`に含まれないため、`merged`をそのまま
2分割してS1・S2を構築すると、split決定後にそのシャードへ着弾した書き込みが消える。

**2段階protocol(Closing → Split)**:

1. 通常の`reorganize()`が完了し、結果(`merged`)のサイズが目標シャードサイズの閾値(例: 2倍)を
   超えていたら、split対象と判定する。
2. 対象シャードの`ShardSlot.superseded`に`Closing`をセットする(S1・S2はまだ存在しない。単に
   「これ以上の新規書き込みを受け付けない」という印)。put/removeは`superseded`をチェックし、
   `Closing`を見たらスピン待ちして新規書き込みを行わない(get/scanは`Closing`中も対象シャードを
   直接読んでよい——データは壊れない、書き込みが止まるだけ)。
3. 対象シャードの`reorganize()`をもう一度呼ぶ。既存の仕組み(`reorg_epoch_`/`active_epochs_`に
   よるepoch drain)がそのまま、`Closing`セット時点で対象シャードに残っていた書き込み中の操作を
   正しく`merged`へ含める。`Closing`以降に到達した新規書き込みは(2)で止まっているため、この
   2回目の`merged`はそのシャードの完全な最終状態になる。T1Index自体には一切変更を加えない。
4. この最終`merged`を中央で2分割し、それぞれから新しいシャード(空のappend region + 分割された
   sorted region)を構築する。
5. `superseded`を`Closing`から`Split{S1, S2, 分岐キー}`へ更新する(`Closing`でスピン待ちしていた
   put/removeはここで起床し、S1かS2へリダイレクトされる)。
6. ディレクトリを新しいエントリ(S1, S2への境界)で置き換える。
7. epoch-basedな回収により、旧ディレクトリ経由で対象シャードを直接参照し得るすべての操作が
   完了したことを確認してから、そのシャードを解放する。

コストは償却でO(1)/insert、単発の最悪コストはO(シャードサイズ)であり、コーパス全体のサイズに
依存しない(2回目の`reorganize()`もO(シャードサイズ)であり、この定数倍が増えるだけ)。
`bench_kv.cpp`の`ikey()`(`"k"`+16進15桁)のような単調増加キーでも、成長し続けるのは常に最右端の
1シャードだけなので、この特性は保たれる。

## APPEND_CAPのスケーリング

各シャードは独自の`AppendRegion`(固定のmmapフットプリントを持ち、疎な占有でもほぼ全ページが
residentになる、オープンアドレッシングゆえの特性)を持つため、シャード数Kに比例して固定オーバー
ヘッドが増える。

このオーバーヘッドを抑えるため、`APPEND_CAP`はシャード目標サイズに比例させる: 目標サイズより
append領域が大きくなる構成は本末転倒(サイズベースのsplitトリガーが発火する前にappend領域自体の
hard thresholdで頭打ちになる)。これにより`APPEND_CAP`は`Config`のコンパイル時定数から、シャード
生成/split時に目標サイズ×比率で決まる実行時パラメータへ変更する。

結果として、チューニングすべきノブは「シャード目標サイズ」の1つに集約される: 大きくすればシャード数
Kが減り固定オーバーヘッドは下がるが1回のreorganize/splitの最悪コストは増え、小さくすればその逆になる。

## Shrink(マージバック)

削除主体のワークロードで生存件数が目標サイズを大きく下回ったシャードは、隣接シャードとマージする。

判定はsplitと同じタイミング(そのシャードの`reorganize()`完了時)で行う: 生存件数が下限(例: 目標の
0.5倍)を下回り、隣接シャードとの合算が上限(例: 目標の1.5倍)以内に収まるならマージする。

両シャードのキー範囲は非重複かつ順序があるため、双方の最終`merged`が確定した後は、ソートやマージ
処理なしに単純な配列連結だけで正しい順序になる(シャード跨ぎScanの連結と同じ理屈)。「最終」
`merged`を得る手順はsplitと対称: 両シャードに`Closing`をセットしてから、それぞれもう一度
`reorganize()`を呼び、`Closing`セット後に着弾する新規書き込みを止めた上で完全な最終状態を得る
(理由はsplitの項を参照——省略すると書き込みロストが起きる)。`superseded`は両シャードとも
`Closing`から`Merged{結合後の新シャード}`へ更新し、ディレクトリから該当の境界を削除する。
コストはO(対象2シャードのサイズ)。

split/mergeはいずれも「背景ワーカーのスレッドプール」節で述べる仕組みにより、シャードごとに独立して
並行に実行できる。ディレクトリの更新だけが共有オブジェクトへのCASを介して直列化される。

## シャード跨ぎの並行アルゴリズム

### Forwarding pointer(B-link tree方式)

各シャードは`superseded`という1ワードのatomicフィールドを持つ(初期値null)。取りうる状態は
`null`(通常)・`Closing`(split/merge中、新規書き込み拒否、S1/S2未確定)・
`Split{S1, S2, 分岐キー}`(split完了、リダイレクト先確定)の3つで、遷移は
`null → Closing → Split{...}`の一方向。具体的な手順は「Splitting」節を参照。

すべての操作(get/put/remove/scan)は、シャードに入る前に`superseded`をチェックする:
`Split{...}`ならキー比較でS1かS2へ転送してから操作をやり直す。`Closing`はput/removeだけが
スピン待ちし(get/scanはそのままSを直接読んでよい——「Scanの一貫性モデルのスコープ」節の
per-keyの鮮度の範囲内)、`Closing`が`Split{...}`に遷移するのを待って転送する。

### `null → Closing`のCASは必ずrouting epoch guard配下で行う

splitを開始する`null → Closing`のCAS自体が対象`ShardSlot`の参照外し(dereference)であるため、
このCASは必ず(get/put/scanと同じ)routing epoch guardの内側で行い、CASに成功して初めて
guardの外へ出て残りの処理(2回目のreorganize以降、`continue_split`相当)を行う。理由:
CASの直前に対象を解決(resolve)した時点では`superseded == null`だったとしても、guardなしで
CASを試みるまでの間に**別のスレッドによる同じシャードへのsplitが完了し、対象が既に解放されて
いる**可能性がある。CASに成功した後は、そのシャードを解放できるのは自分だけになるため、
残りの重い処理(2回目のreorganize、S1/S2構築、ディレクトリ更新、epoch drain、解放)は
guardなしで行ってよい——むしろguard配下のままにすると、自分自身のepoch drain
(`wait_until_epoch`)が自分自身のguard解放を待つ形になり**デッドロックする**。

この「CAS試行だけをguard配下で行い、成功後の重い処理はguard外で行う」という分割は、
splitを開始しうる**すべての**経路(明示呼び出し・背景ワーカーの自動発火のいずれも)が
守るべき不変条件である。同様に、`Directory`を参照外しするだけの読み取り専用メソッド
(例: シャード数を返すアクセサ)も、get/put/scanと同じrouting epoch guardの内側で
`directory_`を読む必要がある——実装時、これを怠ったことでTSanが実際にuse-after-freeを
検出した(背景ワーカーの完了したsplitが解放したディレクトリを、guardなしのアクセサが
読みに行っていた)。

この仕組みはLehman & Yao (1981)のB-link treeにおける"high key + right sibling link"と同型であり、
「ディレクトリがまだ更新されていない古い参照でシャードに到達した操作」を、ロックなしで正しい行き先へ
導く。これにより、split中に古いディレクトリ参照でSへ到達したput/removeが、破棄されるSへ書き込んで
ロストする、という事態を避ける。

### Get/Scanがsplit中のシャードに到達する場合

Sのメモリは「Splitting」節の手順7(epoch回収)が完了するまで解放されないため、直接読んでも壊れない。
read系操作もforwarding pointerを辿ることで、split後にS1/S2へ書き込まれた最新データを見逃さない。

### Scanの一貫性モデルのスコープ

T1(シャーディング層を含む)がScanに対して保証するのは以下の2点のみである:

- **構造的完全性**: 交差するキー範囲は漏れなく、かつ重複なく1回ずつ返される。並行するsplit/merge
  (SMO)が走っていても、これは崩れない。
- **per-keyの鮮度**: 返される各キーの値は、Scanの開始から終了までの間のいずれかの時点で実際に
  成立していた値である。

意図的に保証しない(スコープ外とする)のは、**異なるキーにまたがる書き込み間の実時間順序**である。
たとえば無関係な2クライアントが`put(A)`→`put(B)`を実時間で逐次実行したとき、Scanの結果が「Bは
新しい値、Aは古い値」を返すことは許容する。

この保証を省く根拠は「上位層がいずれ肩代わりするはず」という期待ではなく、**この保証が意味を持つ
ためにはA・Bを1単位として書き込む機能(トランザクション、バッチ書き込み)が前提として必要**という点
にある。VMemKVのAPIはget/put/remove/scanいずれも単一キー操作であり、複数キーを1つの書き込み単位と
して扱う機能を持たず、その予定もない。独立した2回のput()の間にアプリ側が守るべき不変条件はそもそも
表現しようがないため、scanがそれらをどの順序で見せても破られる不変条件が存在しない。この保証を
scan側だけに単独で作り込んでも、対応する書き込み側の協調機構がない限り誰にも観測・活用され得ず、
コスト(前述のbump/drainのいずれの方式でも発生する)に見合う価値がない。

将来的に複数キーにまたがる書き込み機能を追加する場合は、その機構が自分自身のコミット時点で
バージョン/タイムスタンプを刻み、読み取り時にそれをT1へ渡す形で一貫性を実現すべきである
(RocksDB/WiredTiger/InnoDB等のストレージエンジンも、iteratorは呼び出し側が渡すsnapshotハンドルに
従うだけで、ストレージ層自身がグローバルな順序を発明してはいない)。その時点で本セクションの
スコープを再検討する。

上記の2点(構造的完全性+per-keyの鮮度)は、forwarding pointerの仕組みだけで満たされる:
split中の古いシャードSを直接読んでも(補正しなくても)、Sは freeze 時点のデータをすべて保持して
おり、破棄されるまで壊れない。forwarding pointerを辿ってS1/S2側を読めば、freeze以降にS1/S2へ
書き込まれた分もper-keyの鮮度の範囲内で反映される。いずれの読み方でも構造的完全性は崩れない。
**グローバルなepoch/sequenceカウンタは不要であり、追加のコストは発生しない。**

### 背景メンテナンス操作の直列化

各シャードは既存のT1Index自身が持つ`reorg_in_progress_`相当のCASガードを独立に持ち、複数シャードの
`reorganize()`/split/mergeは並行に実行できる。直列化が必要なのは共有ディレクトリオブジェクトの
更新だけであり、これは共有ディレクトリポインタへのCASで解決する(複数シャードが同時にsplit/mergeを
申告した場合は、CAS失敗時に最新のディレクトリを読み直して自分の変更を再適用するリトライループになる)。
並行性を考える必要があるのは「1つのシャードの背景メンテナンス操作 vs そのシャードへの前景read/write」
の組み合わせであり、「シャードA・Bの背景メンテナンス操作同士」は互いに独立で競合しない。

## 背景ワーカーのスレッドプール

### モデル

シャードごとに専用スレッドは割り当てない(Kが大きい場合にOSスレッドが増えすぎるため)。かわりに、
固定サイズの共有ワーカースレッドプールと、「reorganizeが必要なシャードID」を保持するキュー
(集合、重複投入なし)を用いる。あるシャードへの書き込みが既存のsoft threshold相当を超えると、
そのシャードIDがキューに投入される(すでにキュー中/処理中なら何もしない)。プール内の各ワーカーは
キューからシャードIDを取り出し、そのシャードの`reorganize()`(split判定込み)を実行する。

hard threshold(シャードのappend領域が実際に満杯)は既存と同じ発想をシャード単位にスコープを絞って
維持する: そのシャード宛の書き込みを試みたスレッドだけがブロックして待つ。他シャードへの書き込みは
影響を受けない。これは今日の(単一T1Indexの)hard threshold機構をそのままシャード単位に一般化した
ものであり、`docs/benchmark/20260823_defragment_scaling_measurements.md`系で実測されている
「メンテナンス作業を前景スレッドの経路に混ぜ込むとスループットが崩壊する」という既知の問題を
再導入しないよう、**アクセスしたスレッド自身にreorganizeを代行させる方式は採用しない**。

キューは集合なので深さは最大でもK(シャード総数)に収まり、無制限には膨らまない。FIFO+重複排除に
より、処理済みのシャードが直後に再び閾値を超えても列の末尾に再投入されるだけで、特定シャードの
飢餓は起きない。

### Nested parallelismの回避

既存の`reorganize()`は内部で`std::execution::par`によるparallel sortを使っている。これは「同時に
1つのreorganizeしか走らない」という前提の下では正しい選択だったが、プールが複数シャードの
`reorganize()`を同時に走らせるようになると、各タスクが内部でさらに全コアへfan-outしようとして、
プールサイズ×内部並列度の分だけ実効スレッド数が物理コア数を超過し、コンテキストスイッチとキャッシュ
スラッシングで悪化する。シャーディング後は、各シャードのサイズが目標サイズ以下に抑えられているため、
シャードの`reorganize()`は逐次sortで十分に高速なはずであり、`std::execution::par`から
`std::execution::seq`へ切り替える。並列性は「多数の小さなreorganizeを別々のワーカーが同時に処理する」
ことで得る。

### プールサイズ

決め打ちにせず、既存の`T1ReorganizeSoftThresholdPercent`等と同じ流儀で設定可能にする
(例: `T1ReorgWorkerThreads`)。デフォルト値は「物理コア数から前景トラフィック用に確保する分を
引いた数」を出発点とし、実測によってチューニングする。

### キューが追いつかない場合の劣化特性

持続的にプールの処理能力を超える書き込み負荷がかかると、複数シャードが同時にhard thresholdへ
到達し、それぞれのシャード宛の書き込みだけがブロックする。全体のスループットはプールの処理能力で
頭打ちになるが、影響は該当シャードに限定され、他シャードへの書き込みは継続する——今日の単一
T1Indexが持つ「hard thresholdで全体が止まる」状態より優れている。

例外は、単調増加キーのように**常に1シャードだけが更新され続けるワークロード**である。この場合
並列化できるシャードがそもそも1つしかなく、プールを大きくしても意味がない。この限界は
「Splitting」節で述べた、splitのコスト分析における同じ限界と同一の原因による。

### 検討したが不採用: コルーチン/グリーンスレッド

コルーチン/グリーンスレッドは「大量の、待ち時間の多い(I/Oバウンドな)タスクを低オーバーヘッドで
捌く」場面で有効な技術である。`reorganize()`は基本的にCPUバウンド(メモリ上のsort/merge/再構築)で
あり、同時に存在しうるタスク数もK(シャード総数、現実的には数千程度)に収まる。これは軽量な
同時実行数を必要とする問題ではなく、コルーチンを導入しても利点がない。また「シャード数Kに対して
スレッドを1つずつ割り当てると多すぎる」という問題は、上記の固定サイズプール+キューの設計で
既に解決されており、コルーチンに置き換える動機がない。

## Checkpoint/Recovery

T2とWALは既存どおりグローバル(シャード非依存)のまま維持する。`checkpoint()`はK個のシャードそれぞれに
対して強制`reorganize()`(split判定込み)を行い、境界キー+各シャードのソート済みエントリをチェック
ポイントファイルへ直列化する。既存の「T1チェックポイントをWALローテーションより前に行う」順序制約は
維持し、これをK回繰り返す形になる。Recoveryはこのファイルからディレクトリ+K個のシャードを再構築した
後、ディレクトリベースのルーティングでWALの残りを再生する。checkpoint中もforwarding pointerの仕組みで
保護されるため、split固有の新しいハザードは生じない。

## 実装状況

- 済: `APPEND_CAP`のT1Index実行時コンストラクタ引数化(`src/t1_index/t1_index.hpp`)。
- 済: `ThreadReferenceTracker`のstd::deque由来の競合状態を修正(`src/core/reference_tracker.hpp`)し、
  ディレクトリ層のepoch回収の土台をTSanクリーンにした。
- 済: ディレクトリ層本体(`src/t1_index/sharded_t1_index.hpp`の`ShardedT1Index<Config>`) —
  ディレクトリのRCU差し替え、forwarding pointer(`null`/`Closing`/`Split{...}`)、2段階split
  protocol、シャード跨ぎscan。
- 済: サイズ閾値に基づくsplitの自動発火と背景ワーカーのスレッドプール+キュー
  (`request_maintenance_if_needed()`/`worker_loop()`/`run_maintenance()`)。ポーリング間隔方式は
  `vmemkv_impl.hpp`の`reorg_worker_loop()`と同じ流儀(condition variableではなく固定間隔ポーリング)。
  実装過程でTSanが3件の実際のuse-after-freeを検出し修正した(いずれも根は同じ: routing epoch
  guardの外で取得・使用された`ShardSlot*`が並行するsplitの解放と競合する)。(1) 重複キュー投入
  されたエントリが、対象シャードの解放後もキューに残り得た問題 → `null → Closing`遷移時に同じ
  mutexの下でキューをpurgeし、`request_maintenance_if_needed()`側も同じmutexの下で`superseded`
  を再チェックしてから投入する形に修正。(2) `null → Closing`のCASやディレクトリを読むだけの
  アクセサがrouting epoch guardの外で行われ、並行するsplitの解放と競合した問題 → 「`null →
  Closing`のCASは必ずrouting epoch guard配下で行う」節の設計に修正。(3)
  `worker_loop()`が`pop_queue()`をrouting epoch guardの外で呼び、取得した`ShardSlot*`を
  `run_maintenance()`に渡していたため、同じシャードを狙う並行`split_shard_containing()`が
  `continue_split()`まで完了して解放し終えても、そのworkerスレッドは`routing_epochs_`に一切
  登録されておらず`wait_until_epoch()`から見えない問題(実データ損失も再現: 2000件中1件が
  消失)。→ `pop_queue()`自体を`run_maintenance()`の(超過チェック/reorganize/CAS試行と同じ)
  routing epoch guard配下に移し、workerがシャードへ触れる前に必ずguard登録済みになるよう修正。
  `tests/test_sharded_t1_index.cpp`に、背景ワーカー主導の自動splitと明示的な
  `split_shard_containing()`が同じシャード群を同時に取り合う専用のリグレッションテストを追加
  (既存テストはこの2つの経路を同時に踏むケースを持っていなかった)。
- 済: 各シャードのreorganize()を`std::execution::seq`へ切り替え(nested parallelism回避、
  「Nested parallelismの回避」節参照)。`T1Index::reorganize()`に`parallel_sort`引数(既定`true`)を
  追加し、既存の(シャーディングなしの)呼び出し元は変更不要のまま、`ShardedT1Index`側だけが
  `false`を渡す形にした。
- 済: Checkpoint/Recoveryのシャード対応。`ShardedT1Index::checkpoint_all_shards(offset_mapper,
  per_shard_writer)` — 全シャードを昇順に強制reorganize()し、`T1Index::reorganize()`と同じ
  `(OffsetMapper, ChkWriter)`契約をシャードごとに適用、境界キー配列を返す。split/mergeは呼び出し
  期間中一時停止(`splits_paused_`)し、ループ全体を1つの`with_routing_guard()`配下に置くことで、
  停止前にCASを勝ち取っていたsplitの解放とも競合しない。`ShardedT1Index::load_from_checkpoint(
  boundaries, per_shard_entries)` — 構築直後の単一シャード状態を、チェックポイントデータから
  複数シャードのディレクトリへ丸ごと置き換える(他スレッドに公開する前提、同期不要)。
  実装過程で、シャーディングとは無関係な**T1Index本体の既存バグ**を発見・修正した:
  `put()`の既存キー更新パスが、書き込み直後の2回目の`resolve()`が「見つからない」を返した理由
  (真のappend領域ハッシュ衝突による追い出し/reorganize()によるfreeze起因のbypass)を区別せず、
  どちらも自分の書き込みをtombstone化していたため、get()/scan()がfreeze競合時に生きているキーを
  一瞬「存在しない」と誤って返し得た(恒久的なロストではなく自己修復するが、可視性の実バグ)。
  `tests/test_t1_index.cpp`に能動的なreaderスレッドを伴うリグレッションテストを追加。
- テスト: `tests/test_sharded_t1_index.cpp`(ルーティング、split後の整合性、gap-free/dedup-freeな
  scan、put/get/scanが明示splitと競合する並行性ストレステスト、自動split発火、複数スレッドからの
  並行insertが複数回の自動splitを引き起こしてもデータを失わないことの検証、checkpoint単体の
  網羅性、load_from_checkpointからの復元、put/get/scan/splitと並行するcheckpointがデータを
  失わないことの検証)。TSan(`ShardedT1Index*`+`T1Index*`全ケース)クリーン。
- 済: `vmemkv_impl.hpp`への結線。`T1IndexT`を`ShardedT1Index<ConfigT>`に差し替え、`get`/`put`/
  `get_with_hash`/`scan`は同一シグネチャのため呼び出し元は無変更。checkpoint(`checkpoint_internal()`)
  は`ShardedT1Index::checkpoint_all_shards()`をシャード対応の複数シャードチェックポイントフォーマット
  (`src/checkpoint/checkpoint.hpp`の`ShardedT1CheckpointWriter`/`ShardedT1CheckpointFile`。ヘッダを
  ファイル末尾に置くトレイラー形式 — shard_count/boundaryが全シャードのコールバック完了まで
  確定しないため)に接続。`reorganize_internal(T1Only)`は同じ`checkpoint_all_shards()`をno-op
  writerで呼ぶ形にし、`store->reorganize()`の同期完了契約を維持。`ShardedT1Index`はコンストラクタで
  `worker_threads=0`(背景ワーカー未起動)にしてから`load_from_checkpoint()`を行い(単一スレッド前提)、
  直後に`start_workers()`を呼んでから`recover_from_wal()`に入る — WAL再生自体は単一スレッドのままだが、
  `ShardedT1Index::put()`内部の`AppendRegionFull`自己解決が効くようワーカーを先に起動しておく必要が
  あるため(旧実装の「reorg workerはrecovery後に起動」という順序からの意図的な変更)。
  `maybe_reorganize_if_needed()`/`reorg_worker_loop()`からT1のappend容量閾値ロジックを削除し、
  WALサイズ閾値によるcheckpoint起動のみを残した(T1自身のsplit/reorganizeトリガーは
  `ShardedT1Index`の背景ワーカーが担う)。`maybe_reorganize_if_needed_for_delete()`は削除。
  `tests/test_crash_recovery.cpp`に複数シャードでのcheckpoint+再起動ラウンドトリップテストを追加。
  全271テストがTSan(`build-tsan/`、`external_symbolizer_path`にaddr2line指定 — サスペンションの
  シンボルマッチにはシンボル化が必須なため`symbolize=0`は使えない)込みでクリーン。
- 済: この結線のTSan検証中に、シャーディングとは無関係な**`Wal`本体の既存バグ**を発見・修正した
  (`src/wal/wal.cpp`): `Wal::size_bytes()`が`fd_`をロードして`fstat()`する間に、並行する
  `Wal::rotate_segment()`が同じfdを`close()`する競合があった(fstat-after-closeでEBADF例外、
  最悪の場合はfd番号が別のopen()に再利用され無関係なファイルのサイズを返し得た)。
  `rotate_segment()`の`close(old_fd)`と`size_bytes()`の`fstat(fd_)`を専用の`fd_close_mu_`で
  相互排他する形で修正(書き込み/fsyncパスとは無関係、close呼び出しの瞬間だけをガード)。
- 済: `continue_split()`まわりに存在した3件のバグを発見・修正した。(4) 2回目の
  `reorganize()`が`T1Index`内部の`reorg_in_progress_` CASを、同じシャードに対する冗長な
  (重複キュー投入由来の)並行`run_maintenance()`呼び出しと取り合って負けることがあり、その結果
  (空の`merged_entries`)を「分割するほどのデータがない」ケースと区別できず静かに中断していた
  (`superseded`をnullへ戻し、シャードが無制限に肥大化を続ける——シャーディング導入前のO(corpus)
  問題の再発)。持続的な書き込み負荷下では恒久的に発生しうる。→ `checkpoint_all_shards()`と
  同じ「捕捉できるまでリトライする」パターン(`captured`フラグをコールバック内でのみ立てる)を
  適用して修正。(5) 上記(4)のabortパスで`maintenance_pending`を`superseded`より先にfalseへ
  戻していたため、その間隙に`request_maintenance_if_needed()`が滑り込むと
  `maintenance_pending`がtrueのまま永久に取り残される問題があった → リセット順序を
  `superseded`→`maintenance_pending`へ入れ替えて修正。(6) 上記を修正後もなお、
  「Closing可視化直前にシャードを解決したが`T1Index::put()`本体にはまだ到達していない」
  書き込みが、2回目の`reorganize()`のスナップショット確定後に`target`(まもなく削除される)へ
  書き込みを完了させ、`delete target`で消失するケースが残っていた(`T1Index`自身のepoch機構は
  `T1Index::put()`到達済みの呼び出ししか見えない)。既存の(結線前からある)コミット済みコードにも
  存在する既存バグ。→ `continue_split()`終盤の既存epoch drain(`delete old_dir`/`delete target`
  の直前)の**直後**に3回目の`reorganize()`でtarget残留分(straggler)を捕捉し、`boundary`で
  低/高シャードへ再配分するステップを追加(このdrainはbump前に登録された全guardの解放を待つため、
  完了時点で該当スレッドは必ず書き込みを終えている一方、それより前にdrainを置くとClosingで
  スピン待機中の書き込みへの自己デッドロックになる)。`T1Index`に
  `put_with_final_hash(prefix, stored_hash, value)`(`put()`の共通ロジックを
  `put_with_stored_hash()`へ切り出し、生キーバイト列の代わりに確定済みhashを受け取る経路)を
  追加。`tests/test_sharded_t1_index.cpp`に単発一意キー挿入によるリグレッションテストを追加
  (検証は`shard_count()`の安定化+一定時間待機後に行う必要がある——split完了は上記stragglers
  再配分の**前**に起こるため)。

  未解決の別問題: 極端な設定(target_shard_size数百件+16書き込み/8ワーカースレッドで同一の
  小さいキー範囲へ継続的に既存キーを再更新し続けるcyclingパターン)では、上記(4)(5)(6)修正後も
  データ不整合が残存する。単発挿入パターンや本番相当スケール(数十万〜500万件)では再現しない
  ため、cyclingする既存キー更新が極端な小シャード・高並行度と組み合わさった場合に固有の別バグと
  見られる。根本原因未特定(次のステップ参照)。`tests/test_sharded_t1_index.cpp`の該当テストは
  この極端な設定でのデータ整合性チェックを意図的に含めていない。

- 済: シャーディング後の公開API整理。`KVStore`コンセプト(`include/vmemkv/vmemkv.hpp`)に
  `reorganize()`/`checkpoint()`/`get_statistics()`を追加(いずれも`StoreAdapter`が全バックエンド
  向けに無条件で提供済み——`reorganize()`は各rivalが自前no-opを持つため、`checkpoint()`/
  `get_statistics()`は`StoreAdapter`側の`is_rival_store_v`分岐によるため、コンセプトへの追加は
  既存の保証を型レベルで明示するだけで済んだ)。`store_adapter.hpp`の`reorganize()`の
  doc commentに、これがシャーディング後は依然O(全コーパス)であり(`checkpoint_all_shards()`で
  全シャードを強制同期マージするため)、本番の定常的なメンテナンスはシャードごとの自動背景
  ワーカーに任せるべきで、このメソッドはテストの決定的な検証や、意図的に重い強制メンテナンス
  ジョブを計測するベンチマーク向けの用途として残していることを明記。`bench_kv.cpp`が
  カスタムコンテキストとして出力していた`T1ReorganizeSoftThresholdPercent`/
  `T1ReorganizeHardThresholdPercent`(シャーディング後は`t1_index.hpp`/`sharded_t1_index.hpp`
  のどこからも参照されない死んだ設定値)を、実際に発火条件を制御する
  `T1ShardTargetSizeEntries`/`T1ShardSplitThresholdPercent`の出力に置き換え。
  `tests/test_kv_store.cpp`の「scan with integral keys verifies lexicographical ordering」
  から不要な`reorganize()`呼び出しを削除(`scan()`が既にsorted/append両リージョンをライブに
  マージ・ソートするため、順序保証にreorganize()は不要——コメントは削除前の実装を反映した
  記述だった)。

- 済: `VMemKVStatistics`のT1系カウンタをシャード化の実態に合わせて刷新。従来の`t1_reorg_count`
  (`VMemKVImpl::reorganize_internal(T1Only)`が明示的に呼ばれた回数のみを数える)は、YCSB-Eの
  強制reorganize()トリガーを撤去した後は事実上常に0になり、しかも本来観測したかった
  「ShardedT1Indexの背景メンテナンス活動」を最初から一度も捉えていなかった(自動splitは
  `worker_loop()`/`run_maintenance()`/`continue_split()`という別経路で動き、この
  カウンタを一切経由しないため)。`ShardedT1Index`に`total_splits_`(`continue_split()`の成功
  パス——`kMinSplitEntries`未満で中断するケースを除く——でのみ加算、organic/明示的
  `split_shard_containing()`のどちらも同じ`continue_split()`を通るため区別不要)を追加し、
  `total_splits()`として公開。`VMemKVStatistics::t1_reorg_count`を`t1_split_count`に置き換え、
  `VMemKVImpl::get_statistics()`は`t1_.total_splits()`から取得する形に変更(`reorg_t1_count_`
  atomicとその2箇所の加算は削除)。あわせて、`maybe_reorganize_if_needed()`のT1閾値ロジック
  削除以来ずっと常に0だった`hard_stall_count`(`hard_stall_count_`atomic含め)も削除——
  「常に0を返すAPI互換目的の値」として残す判断は本番リリース前の現段階では不要と判断。
  `bench_kv.cpp`のベンチマークカウンタ名も実体に合わせて`Reorgs_T1`→`T1_Splits`、
  `Reorgs_T2`→`Checkpoints`に変更し、常に0だった`Hard_Stalls`カウンタは削除。

  `total_hard_stall_duration_us`(`wait_until_reorg_not_running()`経由)自体は生きた値だが、
  その実装コメントとフィールドコメントは「insert/update/delete hit the hard backpressure
  limit」「summed across all writer threads」など、旧T1のappend領域ハードスレッショルドが
  書き込みを直接ブロックしていた頃の記述のまま残っていた。実際の呼び出し元を洗い直したところ
  `run_reorganize()`内の1箇所のみで、書き込みパスからの直接呼び出しは(上記の閾値ロジック削除に
  伴い)既に存在しない——コメントの「Both call sites below」は嘘になっていた。実態は「明示的な
  `reorganize()`/`checkpoint()`呼び出しが、並行する別サイクル(organicまたは別の明示呼び出し)の
  完了をどれだけ待たされたか」のみを表すため、`total_hard_stall_duration_us`を
  `total_reorganize_wait_duration_us`に改名し、コメントを実態に合わせて修正
  (`wait_until_reorg_not_running()`という関数名自体は変更不要——文字通り「reorgが実行中でなく
  なるまで待つ」という動作を正しく表しているため)。機構自体(この待機)は
  `run_reorganize()`の「呼んだら必ず1サイクル完了してから返る」という契約に必要なため削除
  できない。`bench_kv.cpp`の対応するカウンタ名も`Hard_Stall_Duration_us`→
  `Reorganize_Wait_Duration_us`に変更。

- 済: YCSB-Eベンチマーク(`bench_kv.cpp`の`register_ycsb_e_benchmark()`)の強制トリガー
  スケジュールから`reorganize()`(t=5s)を撤去。従来のコメントは「known-cheap control」
  としていたが、シャーディング後は`reorganize()`も`checkpoint_all_shards()`経由で全シャード
  同期マージするためO(全コーパス)であり、YCSB-Eの母集団規模(シャード数十個程度)では
  数秒〜十数秒かかりうる——もはや「軽量な対照」ではない。また、この強制呼び出しが提供して
  いた「forced vs organic」比較は上記の通りorganic側が最初から測れておらず、実測している
  内容は`run_background_jobs_probe.sh`の専用プローブと重複していた。checkpoint()トリガー
  (t=10s, t=25s)はシャーディングと無関係な実I/O操作として引き続き有効なため維持。

- 実測: 2026-09-07のAWSフルベンチ(`benchmark_results/2026090700/`)で、2026-08-21の同一構成
  計測と比較した結果、`ltm/64KB`(コーパス131,040件——`T1ShardTargetSizeEntries`約104万件を
  大きく下回り、計測中`T1_Splits=0`のまま常に単一シャード)でのみVMemKV自身のInsert/Update
  絶対スループットがスレッド数に比例して悪化することを確認した(Insert: threads=1で-5.9%、
  threads=16で-27.3%、threads=32で-30.7%)。同じ08/21比較で`ltm/1KB`(825万件、複数シャードに
  分割済み)のInsertは-7.6%(実際は逆に+7.6%改善)に留まっており、悪化は「絶対にsplitしない
  小コーパス」かつ「高並行度」の組み合わせでのみ顕著。原因は`ShardedT1Index`が全put/get/scanに
  追加した`with_routing_guard()`(ディレクトリ参照+epoch guard)の固定コストで、shardingの恩恵
  (複数の独立したT1Indexインスタンスへ競合を分散する効果)が単一シャードでは一切発生しない
  ため、コストだけが残る形になっている。RocksDB比のwin ratioはこの区間でも1.59x〜5.00xと
  依然優位だが、シャーディング導入の定量化された副作用として記録しておく。

- 済: ベンチマークプローブを2種に分離。`run_background_jobs_probe.sh`(既存)が測る
  `reorganize()`/`checkpoint()`は全シャードを1回の呼び出しで同期的にマージする手動の
  エスケープハッチであり、本番の定常運用で実際に起きるメンテナンス(シャードごとの自動
  背景ワーカーによる、独立した増分的なsplit)を代表しない——background_jobs_probeの
  Insert QPS劣化(約50%)がジョブ所要時間(in_memoryで1.5〜3.7秒、ltmで4.1〜14.1秒)の
  短い窓でのみ発生するのに対し、この所要時間自体は`harness_new_incremental_scaling.cpp`等が
  測った「1シャードの所要時間」(0.6〜1.0秒、コーパス総量に依存せず一定)とは別物(全シャード分の
  合計、あるいはswap圧の影響を受けたもの)であるため、本当に気にすべき「organicなper-shard
  splitがQPSに与える定常的な影響」を答えていなかった。新設の`run_organic_split_probe.sh`
  (`bench_kv.cpp`の`run_organic_split_probe()`、`--mode=organic_split_probe`)は、空の
  ストアに実際の背景ワーカーを起動した状態で単調増加キーを90秒間挿入し続け、
  `get_statistics().t1_split_count`を100ms間隔でポーリングしてorganicなsplit完了を検出、
  各splitイベント直前(このイベント固有のローカルなベースライン——定常QPSはコーパス成長に
  伴って緩やかに変化するため、グローバル平均を使うと後発のイベントほど不利になる)と
  split中(重なる約1.2秒の窓)のInsert QPSを比較する。ロジックは小さい`target_shard_size`を
  使った独立のスタンドアロン検証で確認済み(8シャードへの成長で7回のsplit検出、期待通り一致)。
  `run_bench_aws_c6id.sh`に`--organic-split-probe`フラグを追加し、`run_5parallel_bench.sh`の
  5番目の専用インスタンスで`--background-jobs-probe`と併走させる形にした。
  `run_background_jobs_probe.sh`側のcheckpoint()測定にも、`VMemKVStatistics`の既存フェーズ
  内訳(`last_checkpoint_t1_reorganize_duration_us`等)を使ってT1マージ/WAL rotate/msyncの
  内訳を追加。`generate_report.py`は"Background Jobs"セクションの説明文を「強制・全シャード」
  である旨を明記する形に更新し、新設の"Organic Per-Shard Splits"セクション(split数・
  ベースライン/split中QPS・劣化率をイベントごとに表示、shard数が増えるほど劣化率が下がる
  傾向が見えるはず)を追加。

- 実測(初回、AWS): `run_organic_split_probe.sh`の初回実測で、in_memory/ltm両シナリオとも
  約960〜980万件挿入してもsplitが1件も発生しない(`final_shard_count:1`)という結果になった。
  原因調査の結果、**organicなcheckpoint(WALサイズ閾値駆動)がorganicなper-shard splitを
  事実上飢餓状態にする**という、シャーディングとは別レイヤーの実在の相互作用を発見した:
  1KB値・32並列挿入では`WalMaxBytesSinceCheckpoint`(64MiB)に約6万〜6万4千件ごとに到達し、
  `reorg_worker_loop()`が非常に高頻度で`checkpoint_all_shards()`を発火させる。この呼び出しは
  全シャードのappend領域を無条件に(占有率に関係なく)コンパクションするため、
  `request_maintenance_if_needed()`のsoft閾値(append領域が容量の50%——約104万件——を
  超えたら background maintenance キューに投入)が一度も成立せず、split判定を行う唯一の経路
  である`run_maintenance()`/`continue_split()`が丸ごと呼ばれなくなる。コーパス総量がいくら
  増えてもこの状態では恒久的にsplitが起きない。修正として、`run_organic_split_probe()`の
  挿入フェーズ全体を`VMEMKV_SUPPRESS_AUTO_REORG=1`(`run_background_job_probe()`のpopulate
  フェーズで既に使われていた抑制フラグ)で囲み、organic checkpointを止めた状態で
  ShardedT1Indexの自前のsplit判定だけを働かせる形にした。

- 実測(2回目、AWS): 上記修正後、実際にsplitを観測できた(in_memory: 7回、shard 2→8。ltm: 5回、
  shard 2→6)。ただし固定幅(前後1.2秒)の窓による劣化率計算が、ほとんどのイベントで**マイナス**
  (during窓の方がbaselineよりQPSが高い)という不可解な結果になった。原因は、単調増加キーの
  挿入では常に1つの「hot」シャードだけが書き込みを受けており、そのシャードはsplit直前に
  最も肥大化(閾値の約210万件に到達)している一方、split直後は新しく分割された小さい方の
  シャードに書き込みが移るため処理が軽くなるという、シャード自身の成長サイクルの中で
  baseline窓が最も重い瞬間を、during窓がその後の身軽な瞬間を捉えてしまう交絡にあった。
  加えて`total_splits()`(観測に使っていたカウンタ)はepoch drain・straggler再配分まで完了した
  後にしか増分されないため、観測時刻とsplit本体の停止区間には実際にはズレがあり、固定窓の
  位置取り自体もそもそも不正確だった。
- 済: 上記の交絡を解消するため、`ShardedT1Index`に書き込みが実際にブロックされていた区間を
  直接計測する仕組みを追加した。`continue_split()`内で、Closing可視化(`run_maintenance()`の
  CAS成功直後、この関数の呼び出しとほぼ同時)から`target->superseded.store(split_info, ...)`
  (spin待機中の書き込みが起床・リダイレクトする瞬間)までを`steady_clock`で直接計測し、
  `last_split_pause_us_`(区間長)と`last_split_pause_end_ns_`(区間終了時刻、プロセス内で
  `steady_clock::now()`と直接比較可能な生タイムスタンプ)という2つのatomicに保存、
  `total_splits()`と同様に`get_statistics()`(`VMemKVStatistics::t1_last_split_pause_us`/
  `t1_last_split_pause_end_ns`)経由で公開した。`epoch drain`/straggler再配分(停止区間より
  *後*に走る、書き込みは既に解放済みの処理)は意図的に含めていない——呼び出し元が実際に
  気にするのは書き込みが止まっていた区間そのものであり、`continue_split()`全体の所要時間
  ではないため。「last」(単一フィールド、蓄積ではない)なので異なるシャードの並行splitが
  競合しうるが、`last_checkpoint_*`系と同じ許容範囲の設計(診断目的であり、単調増加キーのように
  同時に1シャードしかhotにならないワークロードでは実質問題にならない)。`run_organic_split_probe()`
  はこれらの値を使い、observed総split数増分ごとに「baseline窓」を停止区間開始の直前1.2秒、
  「during窓」を停止区間そのもの(固定幅の当て推量ではなく実測区間)に置き換えた。274/274テスト
  変更なし(本番コード側の追加は既存の書き込みパスに新しい分岐を入れておらず、統計値の追記の
  み)。

- 実測(3回目、AWS、確定): 正確な停止区間計測により、筋の通った結果が得られた。全12件
  (in_memory 6件・ltm 6件)を通じて`pause_us`は**約204,000〜223,000マイクロ秒(約0.20〜0.22秒)
  で極めて安定**しており、in_memory/ltm間でほぼ差がなく、shard数(2→8)が増えても伸びる傾向は
  見られなかった——コーパスサイズに依存しないという当初の設計目標をより正確な形で裏付けた。
  これは`harness_new_incremental_scaling.cpp`等が測っていた`continue_split()`全体の所要時間
  (約0.6〜1.0秒)より短い。差分はepoch drain・straggler再配分の分で、これらは書き込みが
  既に解放された*後*に走る処理であり、実際に書き込みがブロックされる時間ではないことが
  今回はっきりした。劣化率は42%〜98%と正常な範囲に収まり(以前見られた負の値や-645%の異常値は
  再現せず)、単調増加キーでは常に1つの「hot」シャードだけが書き込みを受けるため、そのシャード
  がsplitする間はほぼ全ての書き込みスレッドが影響を受けるという理屈と整合する。

  一方、「shard数が増えるほど劣化率が縮む」という当初の予想は、このワークロードでは確認
  できなかった(劣化率とshard数到達順に明確な相関なし)。理由は、単調増加キーの挿入パターンでは
  常にシャードが1つしか"hot"にならず、他に何個シャードが存在しても影響を受ける書き込みスレッド
  の割合(≒ほぼ全数)が変わらないため——この性質(shard数が増えるほど1回のsplitが書き込み全体に
  占める影響が薄まる)は、ランダムキー分散のように複数シャードへ同時に書き込みが分散する
  ワークロードでないと現れない。今回のプローブの守備範囲外であり、別途測定するなら単調増加
  キーではなくランダムキーの挿入で同じプローブを走らせる必要がある。

  結論として、シャーディング導入前に問題視されていた「コーパス増大に伴うコスト増大」は、
  organicなper-shard splitの実測(停止区間約0.2秒、コーパスサイズ非依存)によっても裏付けられた。
  一方で、1回のsplitが書き込みスループットに与える瞬間的な影響(単調増加キーでは60〜98%程度)
  自体は非自明であり、"shard数が増えれば自動的に軽くなる"という単純な話ではなく、実際のワーク
  ロードのキー分布に依存することが分かった——ドキュメント化しておく価値のある知見。

  この停止区間(約0.2秒、コーパスサイズ非依存)は許容範囲と判断する。根拠: (1)
  シャーディング前の問題は「コストが際限なく伸びる」ことであり、有界かつ小さい値である時点で
  性質が根本的に異なる、(2) 今回の実験(挿入QPS約9〜11万/秒)ではsplitの発生間隔は約10〜15秒
  であり、定常的な激しい書き込み下でも「劣化している時間」は全体の1.5〜2%程度、(3)
  劣化率の高さ(60〜98%)は単調増加キー特有の「常に1シャードだけがhot」という条件に起因し、
  実運用のワークロード(読み取り混在・非単調キー)ではより小さくなると見込まれる。

  レポート側は、`benchmark_results/pages/2026090700_charts.html`の"Background Jobs"表に
  この平均値(pause時間・Insert劣化率)を要約行として追加し、個々のsplitイベントの詳細
  (pause時間・劣化率のshard到達数ごとの推移)はページ最下部の"Organic Per-Shard Splits"
  セクションにChart.jsの折れ線グラフとして配置する形に再構成した。

- 試行・撤回: Update/Scanの劣化率も測ろうとして、Insert(コーパス成長を駆動、32スレッド)に
  加えてUpdate用8スレッド・Scan用8スレッド(いずれもランダムな既存キーを対象)を追加した版を
  一度実装・実測したが、2つの問題が出て撤回した。(1) 合計48スレッドが32vCPUマシンを過剰予約
  し、Insert自体のスループットが落ちてsplit発生数が半減した(in_memory: 8→4シャード、
  ltm: 7→3シャード)。(2) Scanの劣化率が-513%〜-1693%という不可解な値になった——split対象
  シャードを狙っていた多数のInsert/Updateスレッドが一時停止した際、システム全体のCPU/ロック
  競合が緩和され、相対的に少数のScanスレッドがむしろ速くなる、という実際の相互作用(splitの
  コストそのものではなく、混在ワークロード特有のノイズ)と見られる。3パス構成(insertのみ/
  insert+updateのみ/insert+scanのみ、`run_background_job_probe()`と同じ「1度に1ワークロード
  だけを測る」設計)にすれば回避できると考えられるが、そもそも停止区間自体が短く(約0.2秒)
  頻度も低いため、Update/Scanの具体的な劣化率を知る価値がこの複雑化に見合わないと判断し、
  Insert単体計測に戻した。Background Jobs表のUpdate/Scan列は測定していないことを示す恒久的な
  "n/a"として残る(データ欠損ではなく意図的な仕様)。

## 未実装/次のステップ

- `run_organic_split_probe`をランダムキー挿入パターンでも実施し、「シャード数が増えるほど
  1回のsplitが与える影響が薄まる」性質が、単調増加キー以外の分布でも実際に現れるか確認する。

- 上記の「単一シャード時ルーティング税」——小コーパス(splitが一度も起きない規模)×高並行度
  でのInsert/Updateスループット悪化——の対策検討。実世界での影響度合いは母集団サイズ次第
  (自然に複数シャードに分割される規模では相殺されて現れない)。対策の方向性としては、
  シャード数が1のままの間は`with_routing_guard()`を経由しないfast path(旧来の非シャード
  `T1Index`呼び出しに近い形)を用意することが考えられるが、現時点では未着手。

- 極端に小さいtarget_shard_size+高並行度+継続的な既存キー更新(cycling)の組み合わせで残る
  データ不整合の根本原因調査。本番相当の設定・単発挿入パターンでは再現しないため優先度は
  上記(4)(5)(6)より低い。

- Shrink(マージバック)の実装(split実装後のfast follow、対象外として最初からスコープ外)。
- delete圧力トリガー(`maybe_reorganize_if_needed_for_delete()`相当)の`ShardedT1Index`版。
  `vmemkv_impl.hpp`結線時に、既存の実装を移植せず落とした既知のフォローアップ。
- scan中の動的閾値低下(`scan_active_`駆動でappend領域をL2キャッシュサイズに保つ最適化)の
  `ShardedT1Index`版。同じく結線時に落とした既知のフォローアップ。

## 関連ドキュメント

- [`t1_index_structure_decision.md`](t1_index_structure_decision.md): T1がflat arrayを維持するという
  決定と、シャーディングを次の検討方向とする経緯。
- [`benchmark/20260905_t1_structure_alternatives_survey.md`](benchmark/20260905_t1_structure_alternatives_survey.md):
  pskiplistの実測、PMA/Bw-tree/ARTの調査、範囲シャーディングの先行研究(PebblesDB、B-link tree等)の詳細。
