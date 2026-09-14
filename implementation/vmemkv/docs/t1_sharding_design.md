# T1の範囲シャーディング設計

`reorganize()`/`checkpoint()`のO(corpus)コスト([`t1_index_structure_decision.md`](t1_index_structure_decision.md)参照)を、
単一の`T1Index`を独立したK個のシャードに分割することで解消する設計。各シャードは
`src/t1_index/t1_index.hpp`をシャーディング用フック付きで使う
(freeze/bypass、offset-mapped reorganize、sorted-region dump/load)。

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
3. 対象シャードの`reorganize()`をもう一度呼ぶ。`Closing`セット時点で対象シャードに残っていた書き込み中の操作を
   `merged`へ含める。`Closing`以降に到達した新規書き込みは(2)で止まっているため、この
   2回目の`merged`はそのシャードの完全な最終状態になる。
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

## APPEND_CAPの扱い

各シャードの `APPEND_CAP` は固定であり、シャード目標サイズとの比例関係はない。新シャードは親シャードの `append_capacity()` を継承して生成する。既定値は `Config::T1AppendCapacityEntries`(2M entries)である。

split 判定は `T1ShardTargetSizeEntries` × `T1ShardSplitThresholdPercent` の live entry 数で行い、`APPEND_CAP` とは独立である。チューニングノブはシャード目標サイズ・split 閾値・`APPEND_CAP` のまま個別に存在し、「シャード目標サイズ」の1つには集約されない。

splitはいずれも「背景ワーカーのスレッドプール」節で述べる仕組みにより、シャードごとに独立して
並行に実行できる。ディレクトリの更新だけが共有オブジェクトへのCASを介して直列化される。

## シャード跨ぎの並行アルゴリズム

### Forwarding pointer(B-link tree方式)

各シャードは`superseded`という1ワードのatomicフィールドを持つ(初期値null)。取りうる状態は
`null`(通常)・`Closing`(split中、新規書き込み拒否、S1/S2未確定)・
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
`directory_`を読む。

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

上記の2点(構造的完全性+per-keyの鮮度)は、forwarding pointerの仕組みだけで満たされる:
split中の古いシャードSを直接読んでも(補正しなくても)、Sは freeze 時点のデータをすべて保持して
おり、破棄されるまで壊れない。forwarding pointerを辿ってS1/S2側を読めば、freeze以降にS1/S2へ
書き込まれた分もper-keyの鮮度の範囲内で反映される。いずれの読み方でも構造的完全性は崩れない。
**グローバルなepoch/sequenceカウンタは不要であり、追加のコストは発生しない。**

### 背景メンテナンス操作の直列化

各シャードは既存のT1Index自身が持つ`reorg_in_progress_`相当のCASガードを独立に持ち、複数シャードの
`reorganize()`/splitは並行に実行できる。直列化が必要なのは共有ディレクトリオブジェクトの
更新だけであり、これは共有ディレクトリポインタへのCASで解決する(複数シャードが同時にsplitを
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

hard threshold(シャードのappend領域が実際に満杯)はシャード単位にスコープを絞って
維持する: そのシャード宛の書き込みを試みたスレッドだけがブロックして待つ。他シャードへの書き込みは
影響を受けない。アクセスしたスレッド自身にreorganizeを代行させる方式は採用しない。

キューは集合なので深さは最大でもK(シャード総数)に収まり、無制限には膨らまない。FIFO+重複排除に
より、処理済みのシャードが直後に再び閾値を超えても列の末尾に再投入されるだけで、特定シャードの
飢餓は起きない。

### Nested parallelismの回避

シャードの`reorganize()`は逐次sort (`std::execution::seq`) を使う。並列性は多数の小さな
reorganizeを別々のワーカーが同時に処理することで得る。

### プールサイズ

`T1ReorgWorkerThreads` で設定する。既定値は `max(1, hardware_concurrency / 4)` である。

### キューが追いつかない場合の劣化特性

持続的にプールの処理能力を超える書き込み負荷がかかると、複数シャードが同時にhard thresholdへ
到達し、それぞれのシャード宛の書き込みだけがブロックする。全体のスループットはプールの処理能力で
頭打ちになるが、影響は該当シャードに限定され、他シャードへの書き込みは継続する——今日の単一
T1Indexが持つ「hard thresholdで全体が止まる」状態より優れている。

例外は、単調増加キーのように**常に1シャードだけが更新され続けるワークロード**である。この場合
並列化できるシャードがそもそも1つしかなく、プールを大きくしても意味がない。この限界は
「Splitting」節で述べた、splitのコスト分析における同じ限界と同一の原因による。

## Checkpoint/Recovery

T2とWALはグローバル(シャード非依存)である。`checkpoint()`はK個のシャードそれぞれに
対して強制`reorganize()`(split判定込み)を行い、境界キー+各シャードのソート済みエントリをチェック
ポイントファイルへ直列化する。「T1チェックポイントをWALローテーションより前に行う」順序で
これをK回繰り返す。Recoveryはこのファイルからディレクトリ+K個のシャードを再構築した
後、ディレクトリベースのルーティングでWALの残りを再生する。checkpoint中もforwarding pointerの仕組みで
保護されるため、split固有の新しいハザードは生じない。

## 関連ドキュメント

- [`t1_index_structure_decision.md`](t1_index_structure_decision.md): 各シャードがflat arrayを
  維持するという決定と、範囲シャーディングの定義。
- [`benchmark/20260905_t1_structure_alternatives_survey.md`](benchmark/20260905_t1_structure_alternatives_survey.md):
  pskiplistの実測、PMA/Bw-tree/ARTの調査、範囲シャーディングの先行研究(PebblesDB、B-link tree等)の詳細。
