# `T1Index::reorganize()` / `checkpoint()` の O(corpus) コスト削減調査

`TODO.md` item 7(`reorganize()`のO(corpus)マージコスト)を発端に、reorganize/checkpointを
速くする方向性を調査した記録。結論として恒久的な採用には至っていないが、検討した設計・実測結果・
サーベイした文献をここにまとめる。「今の設計に戻す」判断も含め、今後同じ道を再検討する際の参照先。

## 背景: 何が問題か

`T1Index::reorganize()`は、`sorted_region_`(既存のソート済み全件)と`append_region`から集めた
delta(`imm_entries`)をマージし、新しい`sorted_region_`を構築する。この処理は**delta の大きさに
関係なく、既存の全件(`sorted_region_`)を毎回コピーし直す**——`checkpoint_internal()`もT1
checkpointファイルの再構築にこの同じマージへ依存しているため、reorganize/checkpointの両方が
コーパス全体に比例したコストを、書き込みの多寡に関わらず周期的に払い続ける。これがTODO.md item 6/7
が指摘する「持続的な書き込み下でのバックログ複合」の構造的原因。

厳密には**O(N log N)ではなくO(N)**である点に注意: `std::sort`でソートしているのはappend_region
から集めたdelta(サイズk、`T1AppendCapacityEntries`が上限)だけで、既存の`sorted_region_`は
ソート済みのまま。その後は2つのソート済み列を線形マージするだけなので、1回あたりの計算量は
O(N + k log k) ≈ O(N)。問題は対数項ではなく、**この O(N) を差分の大きさに関わらず毎回律儀に
繰り返す**こと。

## 実測: 現状のコスト内訳

スタンドアロンの計測ハーネス(`T1Index`を直接使用、VMemKVImpl/T2/WALは介さない)で、
約300万件のsorted regionに対する最終`reorganize()`呼び出しの内訳を計測(20コアのローカル環境):

| フェーズ | 所要時間(概算) |
| --- | --- |
| マージ(逐次) | ~48ms |
| `SortedRegion`コンストラクタ(EntrySnapshot→SortedSlot変換) | ~50ms |
| その他(freeze/epoch-wait/append領域収集など) | ~35ms |
| **合計** | ~133ms |

マージと`SortedRegion`構築が、ほぼ同じ比重で全体コストの大半(~74%)を占めている。

## 検討した方向性

| 方向性 | 状態 | 要約 |
| --- | --- | --- |
| スレッド並列マージ | プロトタイプ実施・見送り | 個々のワーカーは高速化するが、結合コピー+`SortedRegion`構築が未並列のまま残り、全体では効果なし |
| 構造体サイズ削減(key_prefix 128→64bit) | 検討のみ・非推奨 | 20-25%程度の削減に対し、inline化・scan()の前提を壊す副作用が大きい |
| SIMD化 | 検討のみ・非推奨 | マージのボトルネックはseqlock+データ依存分岐で、SIMD向きの処理ではない |
| 常時ソート済み構造(B+Tree/PMA)への移行 | 検討のみ・保留 | checkpoint一貫性問題を新たに抱えることになり、スコープが跳ね上がる |
| Branchless merge / Merge Path分割 | 次に試す候補 | 低リスク・既存文献で実績あり、未実装 |

## 1. スレッド並列マージ(プロトタイプ実施済み)

ブランチ: `prototype-parallel-reorganize-merge`(commit `d977d62`, `a885a7f`)。

### 設計

`sorted_region_`をインデックスでP分割し、各内部境界に対応する`imm_entries`側の位置を
`lower_bound`で求めて(key, clean_hash)の境界を揃え、P個の独立した(sorted-slice, imm-slice)
ペアを作る。各ワーカーが担当区間を`merge_range()`でローカルバッファへマージし、最後に結合して
`merged`を得る。`SmallThreadPool`(`src/core/small_thread_pool.hpp`)という最小限の永続ワーカープール
を新設し、`reorganize()`呼び出しのたびにOSスレッドを立てるコストを避けた。`ParallelReorganizeMerge`
というopt-inのablationタグ(`config.hpp`)で有効化、デフォルトでは既存の逐次パスに一切影響しない。

### 実測結果

各ワーカー個別の処理は高速化に成功(72.5万〜82.5万件を11〜16msで処理、逐次実行時と同じ処理レート)。
しかし**全体としては速くならなかった**(~0.98〜1.1倍、誤差レベル)。原因は2つの見落としていた
逐次O(N)ボトルネック:

1. **結合コピー**: 4ワーカーの出力を1つの`merged`へ`insert()`で結合する処理自体がO(N)の逐次コピーで、
   並列マージの最も遅いワーカーの時間の約3倍かかっていた。
2. **`SortedRegion`のコンストラクタ**: `EntrySnapshot`→`SortedSlot`変換パスも別のO(N)逐次処理で、
   今回全く手を付けていなかった。

### 得られた設計(未実装)

Hadoop/Sparkのshuffleパターンに近い2フェーズ構成で両ボトルネックを解消できる見込み:

- フェーズ1(並列): 各ワーカーが担当区間を実際にmergeしてローカルバッファへ書く。結果件数は
  この処理の副産物として確定する(tombstoneの数や新旧sorted/imm間の重複数は事前に分からないため、
  実測ベースが素直)。
- フェーズ1.5(逐次・ほぼ無料): P個のローカル件数のprefix-sumを取り、最終`SortedSlot[]`のオフセットを決定。
- フェーズ2(並列): 各ワーカーがローカルバッファを、確定したオフセット位置へ直接書き込む
  (`EntrySnapshot`→`SortedSlot`変換も同時に行う)。これで結合コピーと`SortedRegion`構築の
  両方がこの1回の並列書き込みに吸収される。

試算では2〜2.5倍程度の定数倍改善が見込めるが、スレッド数を増やしても(メモリ帯域律速のため)
4〜8あたりで頭打ちになると予想され、実装コストも`SortedRegion`に新しいビルダー経路が必要になるなど
軽くはない。**根本的にO(N)という計算量は変えない**(コーパスが10倍になれば所要時間もほぼ10倍のまま、
先送りに過ぎない)ため、いったん見送り。

## 2. 構造体サイズ削減・SIMD化(非推奨)

`SortedSlot`(40B: key 16B + hash/payload/version各8B)のkey_prefixを128bit→64bitに削れば
20-25%のサイズ削減になるが、16バイトという値は「16バイト以下のキーならT1のprefixだけで完全な
キーを復元できる(T2アクセス不要)」という閾値そのもの——inline化条件・`scan()`のキー復元
(Round 6で修正した箇所)の前提を作り直すことになり、影響範囲が広い。

SIMDについては、マージループのボトルネックが**SIMD向きの処理ではない**という結論に至った。SIMDが
効くのは「独立した大量データに同じ演算を並列適用する」場面(例: 未実装の`SimdScan`タグが想定する
append領域の線形走査)。一方マージループは (a) `si`/`ii`のどちらを進めるかが直前の比較結果に
依存する逐次処理、(b) `load_slot_consistent()`のseqlock読み取りがversion奇数なら再試行する
データ依存の分岐、という2点でSIMDのレーンが独立に動けない形をしている。ただし後述のbranchless
merge文献は、この前提の一部(分岐そのもの)を回避する技術であり、これとは別の話。

## 3. 常時ソート済み構造(B+Tree/PMA)への移行(保留)

「T1を完全に揮発可能にしてcheckpoint不要にする」ことは技術的には可能(WAL全replayのみに依存)
だが、recovery時間とWAL保持量が無制限に伸びる代償を払うだけで、問題の解決にはならない。

常時ソート済みの構造(B+TreeやPacked Memory Array)に移行すれば挿入コストは下がるが、**今の
設計がcheckpointを「タダで」得られているのは、周期的な全体再構築(reorganize)が副産物として
不変のスナップショットを作るから**であり、これをやめると「並行更新中にどう一貫したスナップショットを
取るか」という別の難問(index checkpoint問題、下記4節)を新たに抱えることになる。PMAに
切り替えても、この一貫性問題は木構造の場合と全く同じ形で再浮上する——木かPMAかは「ライブ性能」の
選択であり、「checkpointをどう取るか」とは独立した決定。

## 4. Index Checkpointing文献サーベイ

「常時ソート済み構造でchekpoint一貫性をどう保つか」というテーマ自体、確立された研究分野がある。

### LMDB / CouchDB系: Copy-on-Write append-only B+Tree

LMDB(VMemKVが既にrivalとしてベンチマーク比較している)とCouchDBは、更新のたびにroot-to-leaf
パス上のノードだけを新規コピーする設計。checkpointは「今のrootポインタを書く」だけで完了。
トレードオフ: 1件の更新でO(depth)個のノードをコピーする書き込み増幅があり、LMDBはこれを
「単一writerに制限する」ことで対処——VMemKVが目指す複数ライタ並行書き込みとは相性が悪い。

### Bw-Tree + LLAMA(Levandoski, Lomet, Sengupta; Microsoft Research, ICDE 2013 / VLDB 2013)

完全latch-free。更新を「delta record」として既存ノードの前に追加し、間接participationテーブル
経由でCASする。ストレージ側はLLAMA(log-structured)が担当し定期的にdeltaをconsolidate。
VMemKVの「appendしてから後でbatch統合する」という発想と親和性が高いが、実装難度は高い。

### Masstree(Mao, Kohler, Morris; EuroSys 2012)

B+Treeのtrieによる連結構造、全データをメモリ保持。楽観的並行制御(RCU的)はVMemKVの
`load_slot_consistent()`のseqlockパターンと発想が近い。durabilityはロギング+定期checkpoint。

### Index Checkpoints for Instant Recovery(Lee, Xie, Ma, Chen; VLDB 2022)

`must_read_papers/index_checkpointing.pdf`にダウンロード済み。in-memoryインデックスの
checkpointを正面から扱う論文で、3手法を実装・比較。**論文の結論はIACoWを強く推奨**:

| 手法 | 仕組み | 弱点 |
| --- | --- | --- |
| ChainIndex | headTree(可変)+frozenTreeのリスト、背景スレッドでマージ | **read amplification**(read-heavyで45%、delete多めのTPCC-alikeで**79%**のスループット低下)。大規模データでcheckpoint時間が30秒超。 |
| MirrorIndex | 常に最新のMirrorツリーを複製維持、read amplificationは解消 | **write amplification**(全更新をMirror+headTree両方に適用)、write-heavyで22%低下、メモリ使用量ほぼ2倍。 |
| **IACoW**(推奨) | indirection array(論理ID→物理アドレス)+epochベースのノードバージョン管理。ノード単位のCOWで、親ノードは論理IDを持つだけなのでpath copyingが不要 | 読み取りのたびにindirection arrayを1段挟むオーバーヘッド(5-11%)。実装難度は最も高い(GCが必要)。 |

IACoWの「indirection arrayで親を触らずに子だけ差し替える」設計は、LMDB的な素朴なpath-copyingの
書き込み増幅を回避する鍵。またepochベースのノードバージョン管理は、VMemKVが既に持つ
`reorg_epoch_`/`active_epochs_`(EBR)の仕組みと発想が近く、ゼロから作るのではなく既存機構の
延長として接続できる可能性がある。

### FASTER(Microsoft; SIGMOD 2018)

ハッシュインデックス+「hybrid log」(メモリ〜ディスクにまたがるlog-structuredレコードストア、
ホットセットはin-place更新)という構成で、VMemKVのT1(index)+T2(append log)という分割と
設計思想が近い。checkpointは性能とcommit latencyのトレードオフを調整可能な独自方式。

## 5. Merge高速化文献サーベイ

「checkpoint機構は現状維持のまま、flat arrayのソート/マージ自体を速くする」方向で調査。

### Branchless merge(条件分岐→conditional move)

[Daniel Lemireのブログ記事](https://lemire.me/blog/2021/07/14/faster-sorted-array-unions-by-reducing-branches/)。
今のマージループの`if (s.key < m.key) {...} else {...}`という分岐は、ランダムなデータでbranch
predictorが外れやすくパイプラインストールを起こす。三項演算子で書き換えるとコンパイラ(特に
LLVM/Clang)が`cmov`命令に変換し、分岐予測ミスのペナルティを回避できる:

```cpp
out[pos] = (v1 <= v2) ? v1 : v2;
pos1 += (v1 <= v2) ? 1 : 0;
pos2 += (v1 >= v2) ? 1 : 0;
```

Apple M1・AMD Zen2で**10%以上**の改善が報告されている。**既存の逐次マージループへのローカルな
書き換えだけで試せ、リスクがほぼゼロ**——最初に試すべき候補。

### Merge Path(Odeh, Green, Birk; IPDPS 2012 / journal版 arXiv:1406.2628)

2つのソート済み配列のマージを、格子上のパス探索問題として定式化し、任意の分割点で独立した
部分問題に分けられることを示した基礎技術。**今回実装した並列マージのパーティション分割
(境界のkeyを`lower_bound`で対応させる手法)は、この論文の考え方そのもの**——スレッド分割にも、
SIMDレーン分割にも、命令レベル並列化(下記)にも同じ考え方が使える。

### Median-split双方向マージ(Lemireのブログより)

大きい方の配列の中央値を求め、小さい方の配列でbinary searchして分割点を探し、独立した2つの
部分問題を双方向にマージする。Zen3で1要素あたり**約7サイクル→3.5サイクル(2倍)**に改善したと
報告。Merge Pathの考え方を1スレッド内のILP(命令レベル並列)向上に応用したもの。

### Origami(Arman; VLDB 2022)

`must_read_papers/origami_sort.pdf`にダウンロード済み。mergesort全体のフレームワークだが、
中核の"branchless streaming merger"は素朴なマージに対して**1.5倍**、SIMD対応のin-register
sorterで小さいrunなら最大8倍、cache-residing quad-merge treeでメモリ帯域律速も回避。単体
(single-threaded)でも既存実装比最大2倍、マルチコアではほぼ理想的なスケーリングを報告。

### AA-Sort(Inoue, Moriyama, Komatsu, Nakatani; PACT 2007)

SIMD向けのcombsort拡張+ベクトル化mergesort。アラインメントされたメモリアクセスに拘ることで
SIMD命令の効果を最大化。PowerPC 970MPでIBM最適化ライブラリの1.8倍。

### SIMD- and cache-friendly algorithm for sorting an array of structures(VLDB 2015)

VMemKVの`SortedSlot`/`EntrySnapshot`はArray-of-Structures(AoS)レイアウト。この論文は
Structure-of-Arrays(SoA)への変換を伴うSIMDソート手法を扱っており、もしSIMD化を再検討する
場合はメモリレイアウト自体の見直しとセットで検討する価値がある。

## 結論と次の一歩

- checkpoint機構(index checkpoint文献)側は決定版がなく、どの手法も一長一短(read/write
  amplification、メモリ2倍、実装難度)——今すぐ手を出すにはリスクが高い。
- 現状の「batch reorganizeがcheckpointを無料で提供する」設計を維持したまま、flat arrayの
  マージ自体を高速化する方向がリスク・リターンともに現実的。
- **次に試す最有力候補**: 既存の逐次マージループをbranchless(cmov)スタイルに書き換え、
  スタンドアロン計測ハーネスで効果を測定する。効果が確認できれば、Merge Path流の分割統治
  (=今回作った並列化のパーティション境界計算をそのまま流用可能)と組み合わせ、スレッド並列×
  命令レベル並列を積み上げる。

## 参考文献

- Levandoski, J., Lomet, D., Sengupta, S. "The Bw-Tree: A B-tree for New Hardware Platforms." ICDE 2013.
  <https://www.microsoft.com/en-us/research/publication/the-bw-tree-a-b-tree-for-new-hardware/>
- Levandoski, J., Lomet, D., Sengupta, S. "The Bw-Tree: A Latch-Free B-Tree for Log-Structured Flash Storage." IEEE Data Eng. Bull. 2013.
  <http://sites.computer.org/debull/A13june/bwtree1.pdf>
- Levandoski, J., Lomet, D., Sengupta, S., Stutsman, R., Wang, R. "LLAMA: A Cache/Storage Subsystem for Modern Hardware." VLDB 2013.
  <https://dl.acm.org/doi/10.14778/2536206.2536215>
- Mao, Y., Kohler, E., Morris, R. "Cache Craftiness for Fast Multicore Key-Value Storage." EuroSys 2012.
  <https://pdos.csail.mit.edu/papers/masstree:eurosys12.pdf>
- Lee, L., Xie, S., Ma, Y., Chen, S. "Index Checkpoints for Instant Recovery in In-Memory Database Systems." VLDB 15(8), 2022.
  <https://www.vldb.org/pvldb/vol15/p1671-lee.pdf>(ローカル: `must_read_papers/index_checkpointing.pdf`)
- Chandramouli, B., et al. "FASTER: A Concurrent Key-Value Store with In-Place Updates." SIGMOD 2018.
  <https://www.microsoft.com/en-us/research/wp-content/uploads/2018/03/faster-sigmod18.pdf>
- Bender, M., Hu, H. "An Adaptive Packed-Memory Array." ACM TODS, 2007.
  <https://www3.cs.stonybrook.edu/~bender/newpub/BenderHu07-TODS.pdf>
- Bender, M., Farach-Colton, M., Fineman, J., et al. "Cache-Oblivious Streaming B-trees." SPAA 2007.
  <https://www3.cs.stonybrook.edu/~bender/newpub/BenderFaFi07.pdf>
- Lemire, D. "Faster sorted array unions by reducing branches." 2021.
  <https://lemire.me/blog/2021/07/14/faster-sorted-array-unions-by-reducing-branches/>
- Green, O., Odeh, S., Birk, Y. "Merge Path - A Visually Intuitive Approach to Parallel Merging." 2014.
  <https://arxiv.org/abs/1406.2628>
- Odeh, S., Green, O., Mwassi, Z., Shmueli, O., Birk, Y. "Merge Path - Parallel Merging Made Simple." IPDPSW 2012.
- Arman, A., et al. "Origami: A High-Performance Mergesort Framework." VLDB 15(2), 2022.
  <https://www.vldb.org/pvldb/vol15/p259-arman.pdf>(ローカル: `must_read_papers/origami_sort.pdf`)
- Chhugani, J., et al. "Efficient Implementation of Sorting on Multi-Core SIMD CPU Architecture." VLDB 2008.
  <https://dl.acm.org/doi/10.14778/1454159.1454171>
- Inoue, H., Moriyama, T., Komatsu, H., Nakatani, T. "AA-Sort: A New Parallel Sorting Algorithm for Multi-Core SIMD Processors." PACT 2007.
- Inoue, H., Taura, K. "SIMD- and Cache-Friendly Algorithm for Sorting an Array of Structures." VLDB 2015.
  <https://dl.acm.org/doi/10.14778/2809974.2809988>

## 関連コード

- プロトタイプ実装: `src/core/small_thread_pool.hpp`, `src/t1_index/t1_index.hpp`の
  `ParallelReorganizeMerge`関連コード(ブランチ`prototype-parallel-reorganize-merge`、
  commit `d977d62`, `a885a7f`)。
- 計測に使ったスタンドアロンハーネスの構成(再現用): `T1Index<Config<...>>`を直接構築し、
  `T1AppendCapacityLog2`を大きめに設定した上でput()→reorganize()を繰り返し、
  `VMEMKV_DEBUG_REORG_TIMING=1`環境変数でフェーズ別タイミングをstderrに出力する
  (該当コードは同ブランチのcommit `a885a7f`に残置、`#ifdef`ではなく`std::getenv`ガードなので
  本番ビルドでも実行時オーバーヘッドはほぼゼロ)。
