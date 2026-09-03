# T1を「flat array + 周期的reorganize」から「mmap'd skip list」へ

`docs/reorganize_optimizations.md`(reorganize()のO(corpus)コスト削減調査)からの派生検討。あちらは
「今のflat array設計を維持したまま、マージ自体を速くする」方向を探ったが、根本的にO(N)/サイクルと
いう計算量は変えられないという結論だった。ここでは逆に、**「T1をそもそも周期的に作り直さない」**
方向、具体的にはT1をmmap'd・offsetベースのskip listにして、reorganize()という概念自体を消す設計を
検討する。設計・文献調査のメモ。まだ実装には着手していない。

## 背景: なぜflat arrayの周期的再構築という発想に至ったか

現在のT1は`sorted_region_`(不変・ソート済み)+`append_region`(可変)の2領域構成で、
`reorganize()`が周期的に両者をO(N)でマージして新しい`sorted_region_`を作る。この設計の利点は
「checkpointがreorganizeの副産物としてタダで手に入る」ことだった(mergeの出力`merged`が、構造上
不変・一貫したスナップショットになるため)。欠点はTODO.md item 7が指摘する通り、この O(N) を
差分の大きさに関係なく毎サイクル払うこと。

`reorganize_optimizations.md`で検討した並列マージ・branchless merge等は、この O(N) 自体を高速化
する方向。今回検討するのは方向転換で、「そもそもO(N)の再構築が要らない構造にする」。

## 検討した方向性の変遷(このドキュメントに至るまでの議論の要約)

1. **leveled merge / mini-LSM(L0をskip listにしてflushで階層化)**: LSM-treeのmemtable発想。
   ソートコストは消えるが、flush先が階層構造なので結局定期的なコンパクションが要る。ユーザーの
   意向で棄却(コンパクションを避けたい)。
2. **T1をリカバリに使わない(T2 + 有界WAL replayだけで再構築)**: T1自体を非durableにし、
   checkpointはT2だけ永続化する。reorganize/checkpointの結合を切り離せるが、2つの穴がある:
   - inline value(T1InlineValueで値がT1スロットに直接格納され、T2に一切現れないキー)は
     再構築できない。
   - delete(`remove_impl()`は`t1_.put(key, STORE_NOT_FOUND)`とWAL記録のみで、T2には一切書かない
     ——`src/vmemkv_impl.hpp`の`remove_impl()`で確認済み)も、checkpoint境界より前のdeleteは
     T2スキャンでは検出できない。
   - 対策(T2にtombstone書き込み、inline専用の縮小checkpoint)は考えられるが、
     「inline対象がテーブル全体になり得る」「deleteの新規T2書き込みで性能上のdelete優位性を失う」
     という指摘があり、次点の案へ。
3. **(本ドキュメントの本題) T1自体をT2と同じ設計(offsetベース・MAP_SHARED・msyncで永続化)の
   mmap'd skip listにする**: T2が安いmsyncを実現できているのは「ポインタでなくoffset」
   「基本的に場所が動かない」「MAP_SHARED」という3点セットのおかげ。同じ3点セットをT1の
   skip listに適用すれば、T1もT2と全く同じ理由で安くcheckpointできる。inlineもdeleteも
   「T1自体が直接durable」なので特別扱い不要になり、②の穴がそもそも発生しない。
4. **(検討したが却下) T1はflat arrayのまま、checkpointをreorganizeから切り離すだけ**:
   `checkpoint_internal()`は現状`t1_.reorganize()`を内部で呼んでおり(`vmemkv_impl.hpp`の
   `t1_reorganize_duration`計測箇所で確認)、checkpointのたびに必ずO(N)マージを払っている。
   ならば「append_region(未マージの差分)は一切永続化せず、常にWAL replayだけで復元する
   (LSM-treeのmemtableと同じ発想)」ことにして、checkpointは「今のsorted_region_をそのまま
   manifestに指させる+WAL replay開始オフセットを進めるだけ」にし、reorganizeはWALバイト数
   閾値だけで駆動される独立したバックグラウンドタスクにする、という案。
   `recover_from_wal()`まわりのコメント("T1/T2 are wiped and rebuilt by replaying the WAL,
   unless a committed checkpoint (manifest) is found")を読む限り、append_region自体は
   **今も**ディスク上に独立した永続化表現を持たない(常にWAL replayで再構築される)ため、
   実質的にこの「T0」はすでに存在している。
   **却下した理由**: この案はcheckpointのコストは下げられるが、reorganize自体のO(N)コストは
   変わらず、単に頻度が下がるだけ。AWSでの1B件実測(`reorganize_optimizations.md`)で
   reorganizeが70秒台かかり、LTM環境のbackground jobs probeでは60秒の内部タイムアウトに
   達することがすでに分かっており、「低頻度でもいざ走れば止まる」問題は残る。
   → **結論: O(N)マージそのものを消す必要があり、insert時にO(log N)を払って常にソート済みを
   保証するskip list(方向性3)が必須**。

## 3の設計: 何が変わるか

- T1のskip listノードは、生ポインタではなく**ファイル内offset**で相互参照する
  (プロセス再起動でmmap先の仮想アドレスが変わっても壊れないようにするため——T2が既にこの
  発想で設計されている)。
- ノード用の領域は単一のmmap'dファイル内に確保し、フリーリスト(空きoffsetの集合)で再利用する。
- checkpointは、この領域に対する`msync()`だけになる。T2の`checkpoint_internal()`と同じ理由
  ("MAP_SHARED: writes land directly in the page cache... msync() durabilizes exactly what
  readers already see", `t2_flat_file.cpp`のコメントより)で、追加のシリアライズ/コピー工程が
  要らない。
- **reorganize()という概念自体が不要になる**: skip listは挿入時点で常に正しいソート位置に
  入るので、「append_regionに溜めて後でマージする」という2段構成が要らない。insert自体は
  O(log N)のポインタ更新を払う(方向性④の検討により、これは削減ではなく必須コストとして
  受け入れる)。

## checkpoint設計: epoch + msync + zero-fill(議論の到達点)

- 各書き込みは、その時点のグローバルepochに属する。ノードのepoch stampは1ワード
  (8byte、自然アラインメント)で書く——単語単位の読み取りはtearしない、という前提は
  T2の既存設計とも共通(`vmemkv_impl.hpp`の`checkpoint_internal()`コメント参照)。
- checkpoint手順: epochを`E+1`に進める → epoch `E`で書き込み中のwriterの完了を待つ
  (既存の`reorg_epoch_`/`active_epochs_`のEBRをwriter完了検出に転用できるか、あるいは
  同様の機構を別途用意するかは実装時の検討事項) → `msync()` → manifestに
  `epoch=E`をatomic renameで書く。
- リカバリ時: manifestのepoch `E`を読み、node epoch stamp `> E`のノードはすべて
  「durableである保証がない」として破棄(フリーリストに戻す)。
- **フリーリストはそれ自体を永続化しない**。`{begin_offset, end_offset}`のような複数ワードの
  レコードを直接永続化すると、そのレコード自体がtorn writeの対象になってしまう(16byteは
  単一ワードでアトミックに書けない)。リカバリ時にnodeのepoch stamp/tombstoneビットを
  スキャンして毎回導出する方が単純で、recoveryが元々O(corpus)の1回限りのコストを払う
  前提(NV-Skiplist/FPTree/NV-Tree等、方向性③の先行研究と同じ)とも整合する。
- **level 0だけが正しさの根拠**。上位レベルのポインタ更新は複数ステップにまたがってよく、
  crash時に一部だけ完了していても壊れない(探索性能が落ちるだけ)。この点はASCS
  (Xiao et al., 2021、下記参照)がまさに同じ設計を採用し実測でも裏付けている。

## 未解決の課題(正直に: 「枯れた話」ではなく最先端でもまだ解けていない)

### 課題A: 複数ポインタにまたがる挿入がmsync跨ぎでどう見えるか

skip listの挿入は、O(log n)個の**既存**predecessorノードのnextフィールド(offset)をCASする。
複数レベルにまたがるこのCAS列の途中でmsync()が走った場合、どういう状態がディスクに残るか
という問題。これは文献でも認識されている本物の難所で、DRAM向けlock-freeアルゴリズムを
無償でPMEM対応させる標準技術「RECIPE」(Lee et al.)は**明示的にskip listを対象外**にしている
理由がこれ("insertions require the modification of several pointers, in order to maintain the
skip list property")。

対処の選択肢:
- **PMwCAS**(Wang et al.): 複数アドレスをdescriptorオブジェクト経由でまとめてCASし、
  ポインタ自体に「flush済みか」を示す1ビットを埋め込む。実測オーバーヘッド4-6%(現実的な
  ワークロードで)。
- **link-and-persist**(David et al.): 同様にポインタへビットを埋め込み、1リンクずつ永続化。
- **(現時点の第一候補、実測での裏付けあり) level 0だけを正しさの根拠にする方式**:
  level 0(全生存キーを必ず含む最下段)だけをdurable(offsetベース・mmap'd)にし、上位レベル
  (検索高速化のためだけの構造で、正しさには無関係)はDRAMに置いてリカバリ時に再構築する。
  複数レベルの裂けという問題自体を、「上位レベルはそもそも永続化しない」ことで回避する。
  代償はリカバリ時間の増加(上位レベル再構築、O(corpus)だが起動時1回のみ)。

  この方式は、skip list特化の先行研究で**まさにこの設計として実装・実測されている**ことを
  確認した:
  - **AS/ASCS**(Xiao et al., IEEE Access, 2021、**原著PDFを`must_read_papers/ascs_skiplist.pdf`
    で入手・精読済み**): "Atomic and Selective Consistency Skiplist"は、まさに
    「level 0だけが常にconsistentであることを保証し、上位レベルは永続化されるが障害時に
    consistentである保証はなく、リカバリ時に再構築する」という設計そのもの。ログを一切
    使わず、ポインタ更新の順序付けだけでfailure-atomicにする("log-free failure-atomic
    writes")。redo-loggingベースの素朴な実装(RLS)との比較で、キャッシュラインフラッシュ
    回数を67.5%(AS)/75%(ASCS)削減、挿入レイテンシを32.3-40.9%(AS)/36.2-54.2%(ASCS)
    削減、挿入スループットを49.1%(AS)/65.0%(ASCS)向上という具体的な実測結果あり。
    本ドキュメントの設計が「level 0権威+上位レベル非永続化」で正しさを保てるという主張の、
    最も直接的な実装・実測による裏付け。
    **重要な追加確認: 論文本文(III.B Deletion, Figure 5)を読んだところ、deleteは
    tombstoneではなく実際にノードを物理解放している**——上位レベルを上から順にunlink
    (crash-consistency上は無関係、正しさに影響しない)→level 0(essential list)を
    CLFLUSH+MFENCEでunlink・durable化→**"After persisting the essential list, we then
    free the node"**(Figure 5(d): "Free node 25 to be deleted.")。つまり、
    **「level 0のunlinkがdurableになったのを確認してから物理解放する」という順序**を
    守ることで、crash-consistentな物理回収は実現できることの具体的な実装・実測による
    先行事例になる(課題Bの「crash-consistentな回収」の部分については、これで既存研究に
    前例ありと言い切れる)。
    ただし論文全体を通じて"concurrent"/"thread"/"lock"の類の語が一切登場せず、
    recovery節も単一プロセスによる逐次replayの記述のみ——**並行アクセス(複数readerが
    まさにそのノードを読んでいる最中の解放)への言及が見当たらない**。つまりASCSが
    示しているのは「crash-consistentな物理回収」であって、「concurrent-safeな物理回収」
    ではない可能性が高い。UPSkipList(2021)が「recoverable **かつ** concurrent
    (lock-free)」という組み合わせを狙ってなお物理回収を諦めたのは、crash-consistency
    単体ではなく、この2つの両立の方が本当に難しいから、と理解するのが妥当。
  - **NV-Skiplist**(Q. Chen and H. Yeom, "Design of skiplist based key-value store on
    non-volatile memory," IEEE 3rd Int. Workshops on Foundations and Applications of
    Self* Systems (FASW), Sep. 2018, pp. 44–50; 拡張版が*Cluster Computing*
    (Springer, 2019)としても出版されている——**著者はASCS論文自身の参考文献[35]で
    直接確認、以前の版で残していた著者帰属の不確実性は解消**): 最下段だけをNVM上に
    永続化し、上位レベルはDRAM専用にして障害時に再構築する"selective persistence"。加えて
    1ノードに複数エントリをグループ化し(ノード内は未ソート、空きスロットはbitmapで管理)、
    NVM書き込み回数自体を減らす工夫も持つ——T1のノードレイアウト設計時に参考になりそう。
  - **ListDB**(Kim et al., OSDI, 2022): WALと永続skip listを組み合わせ、"Index-Unified
    Logging"でWALを差分的にskip listへ変換していく設計。WALとskip listを完全に別物として
    扱うのではなく、WAL自体をskip listのレイアウトに寄せることでログ→索引変換のコストを
    下げる、という発想は、本ドキュメントの「WALのwindowをどれだけ小さく保てるか」という
    論点に対する参考になる。

  同じ「durable leaf/bottom + volatile-but-rebuildable upper」というパターンはB+Tree系の
  FPTree/NV-Treeでも独立に発見されており(inner nodeをDRAM専用にしてleafだけ永続化、
  リカバリ時にinner node再構築)、比較研究(He et al., "Evaluating Persistent Memory Range
  Indexes: Part Two," PVLDB 15(11), 2022)ではPMem index設計の中で最も高性能という結果が
  出ている——ただし同論文はこれらのリカバリ時間がデータサイズに比例してスケールする
  (瞬時リカバリではない)ことも指摘しており、本設計のリカバリコストの見積もりにもそのまま
  当てはまる。

### 課題B: delete後のノードの物理回収(reclaim)

**重要な訂正**: 「T1自体が直接durableになる」ことで、delete操作そのもの(T1スロットを
STORE_NOT_FOUND等でマーキングすること)はmsync()で自然に永続化される。durability の問題では
もはやない。焦点は、**削除されたノードの領域を、並行読み取りとクラッシュの両方に対して
安全にフリーリストへ返す(物理的に回収する)方法**。

#### crash-consistentな物理回収には先行事例がある(ASCS)

ASCS(Xiao et al., 2021、原著精読済み——上記参照)は、上位レベルを上から順にunlink→
level 0(essential list)をunlink・durable化→**その後にノードを物理解放**、という順序を
守ることで、crash-consistentな物理回収を実装・実測している。この部分は「未解決」ではなく、
既存研究に前例がある。

#### 依然として未解決/未検証なのはconcurrent-safeとの両立

ASCSの論文には並行アクセスへの言及が一切なく、UPSkipList(Chowdhury, 2021,
Univ. of Waterloo修士論文)は「recoverable **かつ** concurrent(lock-free)」という
組み合わせを狙いながら、なおdeleteを「値をtombstoneに置き換えるだけ」として実装し、
ノードの物理削除・回収は一切行っていない(「removeを含むワークロードは意図的に
ベンチマークから除外した」「真のノード削除+安全な回収は今後の課題」と明記)。
UPSkipList自身の文献調査ではepoch-based reclamationを「推奨される技術」として
挙げているにもかかわらず実装していない——理由は論文からは読み取れず(単に研究の
スコープ外だった可能性もある)、本当に技術的な障壁があるのかは不明。

#### 作業仮説→具体設計: 既存EBR(reorg_epoch_/active_epochs_)を単一カウンタとして再利用する

以下は本ドキュメント内での独自の推論であり、上記のどの論文にも直接書かれていない
——**プロトタイプで検証すべき仮説**として記録する。ただし既存コード
(`t1_index/t1_index.hpp`)を読んだところ、必要な部品はすでに存在することを確認した。

物理回収の安全条件は、性質の異なる2つの軸から成る:
1. **concurrency-safety(生きているプロセス内の話)**: 今まさにそのノードを読んでいる
   readerがいないこと。
2. **crash-safety(クラッシュ後の話)**: predecessorのunlink(level 0のCAS)自体がdurable
   になっていること。そうでないと、クラッシュ後のrecoveryで「predecessorはまだそのノードを
   指しているのに、そのオフセットにはすでに別キーの新しいデータが書かれている」という
   破損が起きる(前掲の"checkpoint設計"節のepoch stampと同じ理由)。

**なぜEBRの揮発性は問題にならないのか**: 「EBRの状態(どのスレッドがどのepochにいるか)は
クラッシュで失われる」という点を以前は懸念として書いたが、よく考えるとこれは問題では
ない。**クラッシュはプロセス内の全readerも道連れにする**——recovery後は新しいプロセスが
ゼロからreaderを起動するので、「クラッシュ直前に生きていたreaderが今も古いポインタを
握っている」という状況はそもそも発生しない。EBRの役目は「プロセスが生き続けている間の
安全性」だけであり、crash-safetyとは独立した別軸の問題として、既存のEBRをそのまま
使ってよい。

**既存コードによる裏付け**: `t1_index.hpp`の`reorg_epoch_`(`std::atomic<uint64_t>`)+
`active_epochs_`(`ThreadReferenceTracker<uint64_t>`)は、すでにまさにこのパターンで
動いている——reader専用のEBRで、get/scanは`T1ReadHandle`で現在のepochをスレッドローカルに
登録してから読み(111-115行目のコメント参照)、`reorganize()`は
`reorg_epoch_.fetch_add(1)` → `active_epochs_.wait_until_epoch(新epoch)`という手順で
「古いepochで入ったreaderが全員抜けた」ことを確認してから、古い`append_region_`/
`sorted_region_`を安全に破棄している(480-481行目、588-589行目)。

**統合設計**: このカウンタをcheckpointからも同じ手順で呼べばよい。
- 新規ノードのepoch stamp = ノード作成時点の`reorg_epoch_`の値。
- checkpoint: `reorg_epoch_`を`E+1`に進める → `active_epochs_.wait_until_epoch(E+1)`
  (既存呼び出しをそのまま流用) → この時点で「epoch `E`以前に入ったreaderは全員抜けた」
  ことが確定 → msync → manifestに`epoch=E`を書く。
- **物理回収の条件は単一**: `unlink_epoch <= 最後にpublishされたmanifest epoch`。

これが単一条件に潰れる理由: manifestにepoch=Eをpublishできた時点で、
`active_epochs_.wait_until_epoch(E+1)`はすでに完了している。つまり
concurrency-safety(readerが抜けた)はcrash-safety(checkpointがdurableになった)の
**前提条件としてすでに満たされている**——独立した2条件のANDではなく、「crash-safeが
確認できた瞬間にはconcurrency-safeも自動的に確認済み」という一方向の含意関係になる。
checkpoint側でwriterの完了を別途待つ必要もない——writerの途中経過はepoch stamp
zero-fillが個別に処理するので、checkpointはreaderのquiescenceだけ確認すればよい。

これは新規発明ではなく、**今すでに`reorganize()`が使っているパターンを、checkpointからも
同じカウンタで呼ぶだけ**という意味で、比較的手堅い設計だと考えている。

**留保**: これは本ドキュメント内の推論であり、ASCS/UPSkipListいずれの論文にも
直接裏付けはない。UPSkipListがEBRを「推奨技術」と認識しつつ実装しなかった理由が
単なるスコープの都合なのか、何か見落としている技術的障壁があるのかは、
小さいプロトタイプで実際に検証する必要がある。

回収が要らない(tombstoneのまま残す)という妥協自体は、UPSkipListも採用している現実的な
落とし所ではある。ただしメモリ使用量が単調増加する(GCなし)という代償を伴う。

### 課題C: 読み取りのキャッシュ局所性

flat arrayのbinary searchと比べ、skip listはポインタ(offset)チェイスなのでキャッシュミスが
増える。durabilityとは独立した問題として残る。実装tipとして、1ノードの全レベル分のポインタ
配列を1つの連続領域に確保する(レベルごとにバラバラに確保しない)ことでキャッシュミスを
減らせるという指摘がある(Tickiのブログ、UPSkipList論文が引用)。

### 課題D: フリーリストの永続化・導出方法

課題Bとは別の軸として、フリーリストという媒体自体をどう永続化/導出するかという問題がある。
削除ではなく「置き換え」(同じキーへの再挿入や、ノード分割など)で不要になったノードの回収
タイミングも、課題Bで整理した「EBR ∧ checkpoint epoch」のANDで統一的に扱えるはず。

上記の"checkpoint設計"節で決めた「フリーリストを永続化せず、recovery時に導出する」という
方針について、単純なepoch stampの閾値スキャンだけでは「削除されてunlinkされた古いノード
(epoch自体は`<=`manifestで新しくない)」を検出できないことに注意——これはrecovery時に
どのみち必要な「level 0を辿って上位レベルを再構築する」パス(課題Aの解決方針)の副産物
として導出するのが自然: 辿って到達したoffsetの集合が「生きているノード」、
確保済み範囲のうちそれ以外が「フリー」、という一種のmark-and-sweep。この場合、独立した
epoch-thresholdのスキャンは新規割り当て(まだ誰からもリンクされていない状態)の
torn-write検出だけに使えばよい。

## 実装方針: 独立したプロトタイプとして先に正しさを検証する

VMemKV本体(T2/WAL/checkpoint manager)に最初から埋め込むのではなく、**concurrent・
crash-consistent・物理回収可能なskip listを単体のライブラリとして先に実装・検証する**
方針にする。理由:

- crash-consistency検証の組み合わせ爆発を、狭いAPI(insert/get/delete/scan)に閉じ込め
  られる。VMemKV全体に埋め込んだ状態だと、注入ポイントの組み合わせがVMemKV全体の複雑さに
  引きずられる。
- T1専用ではなく独立した部品として正しさを先に固めれば、将来defragment実装時の変更にも
  強い。
- 今回の文献調査で、(a)完全なlock-free concurrent(複数reader/複数writer)、
  (b)mmap+msyncによるcrash-consistency、(c)本物の物理ノード回収、の3つを同時に満たす
  先行研究は見つからなかった(UPSkipListは(a)+(b)はあるが(c)を諦め、ASCSは(b)+(c)は
  あるが(a)を扱っていない)。3つを揃えたものという意味で、独立した検証・評価に値する
  可能性がある——ただし論文化はプロトタイプが実際に正しく動くことを確認してからの
  判断でよい。

### テスト方法

- **crash-consistency**: `tests/test_crash_recovery.cpp`がすでに採用している手法
  (実プロセスのfork+killではなく、「インスタンスを破棄→ファイルへ直接torn/corruptな
  バイトを注入→別インスタンスで再構築」という決定論的な手法)をそのまま流用する。
  mmap'dファイルの任意オフセットを切り詰め/破壊してから再オープンし、level 0チェイン
  の不変条件(到達可能なノードは全てepoch stampが正しい範囲内にある等)を検証する。
  決定論的なので、書き込み列のprefixを網羅的に列挙してcrash地点を全パターン試す、
  という厳密な検証もやりやすい。
- **concurrency-safety**: `build-tsan/`(既存)でのThreadSanitizer検証に加え、
  novelな並行データ構造という性質上、簡易的なlinearizability checkerの導入も検討する。

## 参考文献

- Chowdhury, S. "A Scalable Recoverable Skip List for Persistent Memory on NUMA Machines."
  Master's thesis, University of Waterloo, 2021.
  <https://uwspace.uwaterloo.ca/items/329222a5-2f42-4b54-92a7-7ee077718632>
  (UPSkipList: 本ドキュメントで検討している設計に最も近い先行研究。delete/reclaimが未解決である
  ことの根拠、level 0のみ永続化する設計への言及、PMwCAS/link-and-persist/RECIPEのまとめが
  含まれる。)
- Wang, T., et al. "Easy Lock-Free Indexing in Non-Volatile Memory." (PMwCAS)
  UPSkipList thesis 3.1節で参照されている、複数アドレスの永続的CASライブラリ。
- David, T., et al. "Log-Free Concurrent Data Structures." (link-and-persist, NV-epochs)
  UPSkipList thesis 3.1節で参照。
- Lee, S.K., et al. "RECIPE: Converting Concurrent DRAM Indexes to Persistent-Memory Indexes."
  SOSP 2019. skip listを対象外とする理由(複数ポインタ更新)の根拠。UPSkipList thesis 3.1節参照。
- Chen, Q., Yeom, H. "Design of skiplist based key-value store on non-volatile memory."
  *Proc. IEEE 3rd Int. Workshops on Foundations and Applications of Self* Systems*
  (FASW), Sep. 2018, pp. 44–50. 拡張版が"Design and implementation of skiplist-based
  key-value store on non-volatile memory," *Cluster Computing* (Springer), 2019,
  DOI: 10.1007/s10586-019-02925-1 としても出版されている。
  <https://www.researchgate.net/publication/331969956_Design_and_implementation_of_skiplist-based_key-value_store_on_non-volatile_memory>
  (NV-Skiplist: 最下段のみNVMで永続化、上位レベルはDRAM専用で障害時に再構築する
  "selective persistence"。ノード内に複数エントリをグループ化しbitmapで空きスロット管理する
  レイアウト最適化も持つ。著者はXiao et al. [ASCS, 下記]の参考文献[35]で確認済み。)
- Xiao, R., Feng, D., Hu, Y., Wang, F., Wei, X., Zou, X., Lei, M. "Write-Optimized and
  Consistent Skiplists for Non-Volatile Memory." *IEEE Access* 9 (2021): 69850–69859.
  <https://ieeexplore.ieee.org/document/9424603/>(原著PDFを
  `must_read_papers/ascs_skiplist.pdf`で入手・精読済み)
  (AS/ASCS: 本ドキュメントの「level 0だけを正しさの根拠にし、上位レベルは永続化するが
  consistency保証はなくrecovery時に再構築する」という設計そのものを、ログを使わない
  ポインタ更新順序だけで実現し実測したもの。課題Aの解決方針の最も直接的な先行研究。
  deleteについても、上位レベルを上から順にunlink→level 0(essential list)をunlink・
  durable化→その後にノードを物理解放、という順序でcrash-consistentな物理回収を実装・
  実測している(Section III.B, Figure 5)。ただし論文全体に並行アクセスへの言及がなく、
  concurrent-safeな回収は範囲外と見られる——課題Bを参照。)
- Kim, W., Park, C., Kim, D., Park, H., Choi, Y., Sussman, A., Nam, B. "ListDB: Union of
  Write-Ahead Logs and Persistent SkipLists for Incremental Checkpointing on Persistent
  Memory." OSDI 2022. <https://www.usenix.org/conference/osdi22/presentation/kim>
  (WALと永続skip listを組み合わせ、WALを差分的にskip listへ変換する"Index-Unified
  Logging"を提案。WALのwindowコストを下げる設計の参考になる。)
- He, Y., Lu, D., Huang, K., Wang, T. "Evaluating Persistent Memory Range Indexes: Part
  Two." PVLDB 15(11), 2022. <https://arxiv.org/pdf/2201.13047>
  (FPTree/NV-Tree等、「durable leaf + DRAM inner nodeの再構築」系設計がPMem index比較の
  中で最も高性能という実測根拠。ただしリカバリ時間がデータサイズに比例してスケールする
  ことも明記——本設計のリカバリコスト見積もりにもそのまま当てはまる注意点。)
- Xing, L., Vadrevu, V.S.P.K., Aref, W.G. "The Ubiquitous Skiplist: A Survey of What
  Cannot be Skipped About the Skiplist and its Applications in Big Data Systems."
  <https://arxiv.org/abs/2403.04582>
  (skip list全般のサーベイ。9.1.2節で"selective persistence"(NV-Skiplist)と
  "relaxed consistency"(ASCS)を「level 0のみ保証、上位レベルはrecovery時に再構築」という
  同一パターンの2つの実装として並べて紹介しており、このパターンが一過性のアイデアでなく
  複数の独立した先行研究に共通する確立した設計であることの裏付けになる。)
- Wang, T. "How LMDB Works." <https://xgwang.me/posts/how-lmdb-works/>
  LMDBのCOW B+Tree設計(offsetベース・mmap・msync・フリーリストによる不要ページ再利用・
  コンパクション不要)の技術的まとめ。今回の設計がLMDBと共有する部分(offset+mmap+msync)と、
  相違する部分(LMDBはCOWで祖先ページを毎回コピーするが、skip listは既存predecessorノードの
  フィールドをCASするだけでコピー不要)の比較の根拠。
- "Snapshot: Fast, Userspace Crash Consistency for CXL and PM Using msync."
  <https://arxiv.org/pdf/2310.16300>
  msync()を使ったクラッシュ一貫性の一般論(msyncは個々の書き込みの順序は保証するが、
  並行する構造的更新を直列化しない、という限界の根拠)。

## 関連コード(現状の該当箇所)

- `src/t1_index/t1_index.hpp`: 現行のflat array + `reorganize()`実装。
- `src/t2_flat_file/t2_flat_file.cpp`: T2の`MAP_SHARED`+offsetベース設計(586f067で導入)。
  本ドキュメントの設計はこれをT1にも適用する話。
- `src/vmemkv_impl.hpp`の`remove_impl()`(約1060行目): 現行deleteの実装、T2に一切触れないことの
  確認箇所(課題Bの前提)。
- `reorg_epoch_`/`active_epochs_`(EBR機構、T1Index内): 課題Dで再利用を見込んでいる既存の
  epoch-based reclamation。

## 次のステップ(未着手)

- 課題A(複数ポインタ更新の永続化)について、ASCS(level 0のみ正しさの根拠、上位レベルは
  recovery時再構築)を軸に小さいプロトタイプで検証する。
- checkpoint設計(epoch + msync + zero-fill、フリーリスト非永続化)を、既存の
  `reorg_epoch_`/`active_epochs_`のEBRとどう統合するか(特にwriter完了検出)を実装レベルで
  詰める。
- **課題B(delete/reclaim)の作業仮説「EBR(concurrency-safety) ∧ checkpoint epoch
  advancement(crash-safety)」を小さいプロトタイプで検証する**。これは文献に直接
  裏付けのない本ドキュメント独自の推論なので、実際に実装して(a)crashを注入しても壊れない、
  (b)並行readerがいても壊れない、の両方を確認する必要がある。うまくいかない場合は
  UPSkipList同様「tombstoneのまま回収しない」妥協に戻す。
