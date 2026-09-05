# T1のインデックス構造再検討: pskiplist移行の実測とその後の代替案サーベイ

## 発端

`new_t1_design_from_flatfile_to_skiplist.md`にある通り、T1Index(flat array + append region +
`reorganize()`)の`reorganize()`/`checkpoint()`がO(corpus)のコストを持つことが課題だった。この
コストを避けるため、mmap'd・lock-freeなskip list(`implementation/pskiplist/`)を新規開発し、T1を
置き換えるプロトタイプを実装した(コミット`f4beb99`)。

## 1回目のAWS実測: reorganize/checkpointは劇的に改善、Get/Scanが大きく悪化

i4i.2xlarge(ap-northeast-1a)、2000万件・8B値、旧T1Index(`385fadf`)とpskiplist移行直後
(`d98922c`)を比較。

**reorganize()/checkpoint()単体のコスト**(`run_background_jobs_probe.sh`、1000万件・1KB値):

| Job | 旧T1Index | pskiplist | 変化 |
|---|---|---|---|
| reorganize() | 1.214s | 0.00543s | 約224倍高速化(※) |
| checkpoint() | 3.780s | 0.0403s | 約94倍高速化 |

(※ reorganize()は新旧で意味が異なる。旧はappend領域のマージ、新は`reclaim()`単体。この計測では
削除が発生していないため新側はほぼ何もしていない`reclaim()`の下限値に近く、フェアな比較ではない。
checkpoint()は両者とも実際の耐久化処理を行うため、こちらが実質的な成果。)

**CRUDスループット**(items/s、threads:1/4/8、旧→新の変化率):

| Op | threads:1 | threads:4 | threads:8 |
|---|---|---|---|
| Insert | +2.4% | +4.2% | +0.9% |
| Get/Hit/Zipf | −62% | −62% | −60% |
| Get/Hit/Uniform | −69% | −66% | −68% |
| Get/Miss/Zipf | −39% | −56% | −46% |
| Scan/Zipf | −27% | −22% | −3% |
| Scan/Uniform | −46% | −45% | −29% |
| YCSB-E(30秒, 8スレッド) | −55% | | |

reorganize/checkpointの当初の課題は解決したが、Get/Scanの劣化が実運用を阻む水準だった。

## pskiplistのパフォーマンスチューニングと2回目のAWS実測

読み取りパスの根本原因を診断: upper-level探索の各ホップが、durable(mmap'd)なLevel 0配列へ
key比較・生存確認のために毎回アクセスしていた。以下を実装(コミット`1331996`):

1. `is_upper_node_dead()`を廃止。upper-levelの生存確認をforward wordのmark bitのみに一本化し、
   `link_upper_levels()`側に自己修復ロジックを追加(nodes_[]配列への参照を削除)。
2. Upper-levelノードに`Key`を直接埋め込み(nodes_[]へのポインタ参照を排除)。
3. Upper-levelノードの確保をRocksDBのmemtable Arenaに倣ったbump-pointerアレナに変更
   (`UpperArena`)。個別`::operator new`の代わりにブロック単位で確保し、ブロックの解放は
   `reclaim()`のepoch-based-reclamationドレインまで遅延。
4. Upper-levelノードを「1ノード1キー」から「1チャンクに最大32件」に変更(`UpperChunk`)。
   既存チャンクへの追加はロックフリーなスロット確保(fast path、実測92%がこちらに乗る)、
   満杯時のみ新規チャンクを分割なしで隣接挿入。

ローカル計測(500万件、シングルスレッド、ランダム挿入順、Get):
6245ns → (アレナ化後)5571ns(−11%) → (チャンク化後)5228ns(−16%)。
挿入順がkey順と一致する場合(バルクロード等)は1967ns(−68%、約3.2倍)まで改善。

AWS再実測(同条件、`1331996`):

| Op | threads:1 | threads:4 | threads:8 |
|---|---|---|---|
| Get/Hit/Zipf | −59.5% | −58.3% | −58.4% |
| Get/Hit/Uniform | −62.2% | −60.6% | −61.3% |
| Get/Miss/Zipf | **−72.0%** | **−74.2%** | **−71.1%** |
| Scan/Zipf | −30.5% | −29.7% | −4.0% |
| Scan/Uniform | −41.3% | −41.4% | −24.8% |
| YCSB-E | −51.7% | | |

Get/Hit・Scan・YCSB-Eは小幅改善したが、Get/Miss/Zipfは逆に悪化した。

## Get/Missの悪化原因: チャンク満杯ではなく、境界での構造密度不足

`Get/Miss/Zipf`は`corpus_size + zipf(rng)`(常にコーパス最大キーの直後)を問い合わせる。

- 仮説「着地するチャンクが満杯でスキャンが重い」はAWS実機での直接計測で否定された:
  該当チャンクの占有率は3/32で、ランダムなGetが着地するチャンクの平均14.37/32よりむしろ疎。
- perf(`perf record -g -e cpu-clock`、AWS実機、`Get/Mode=Miss/Dist=Zipf`)では
  `search_upper_levels`26.7%、`marked_list_find`16.2%、libc(memcmp相当)約39.7%。
  Get/Hitと同種の内訳だが、key比較(libc)の比率がGet/Hitの約2.3倍高い。
- ローカル再現(実際のベンチマークと同じ昇順投入+実Zipf分布)で、チャンク化後は末端クエリの
  ホップ数が元の設計より増える傾向を複数試行で確認したが、試行ごとのばらつきが大きく、
  AWSで観測された悪化幅(2倍超)を安定再現するには至っていない。
- 現時点の説明: チャンク化はオブジェクト総数を約12〜32分の1に減らす。skip listの各段の高さは
  独立乱数で決まるため、「近傍にたまたま高い段まで昇格したオブジェクトがある」確率は周囲の
  オブジェクト数に依存する。Get/Missはキー空間の絶対的な境界という構造上唯一の点を機械的に
  繰り返し問い合わせるため、この「運のショートカット」に恵まれない場合の影響を平均化する
  機会がない。Get/Hitは同じくzipf skewでも問い合わせ先が分散するため、影響が平均化されて
  表面化しにくい。

対応はbloom filter等での回避を優先し、この投資判断からは保留とした。

## Get/Hit・Scan・Updateのperf内訳(AWS実機、`1331996`、RelWithDebInfo、`perf record -g -e cpu-clock`)

- **Get/Hit/Zipf**: `marked_list_find` 36.18%、`search_upper_levels` 18.91%、
  `find_at_or_after` 6.83%、libc(memcmp相当) 約18%。
- **Get/Hit/Uniform**: `marked_list_find` 51.73%(ホットな領域がなくキャッシュ再利用が効かない
  ため、Zipfより高い)。
- **Scan/Zipf**: 初回シーク(`search_upper_levels`等)が全体の約33%、Level 0の逐次歩行
  (`try_read_base_record`等、T2への値読み出しを含む)が残り約67%。
- **Scan/Uniform**: シーク約43%、逐次歩行約57%。
- **Update/Zipf**: 壁時計108〜110µsに対しCPU時間は約19.5µs(5.5倍の乖離、2回の独立実行で再現)。
  `write_and_fsync_batch`/`fdatasync`/ext4ジャーナルコミット(`jbd2_complete_transaction`等)が
  CPUサンプルの25〜33%を占め、さらに壁時計の乖離分はディスク同期待ちで`cpu-clock`サンプリング
  では見えない。pskiplist関連コスト(`marked_list_find`31.27%等)は実在するが、WAL fsyncコストに
  比べると副次的。UpdateがGet系ほど悪化しなかった(−6〜9%程度)理由はこれで説明できる:
  T1実装によらないfsyncコストがUpdate全体の大半を占め、T1側の劣化が希釈されている。

Get/Hit・Get/Missは「O(1)ハッシュ→O(log N)ポインタチェイシング」という設計変更の必然的な
コストであり、特定のバグではない。Scanは今回のチューニングが対象にしていないLevel 0自体の
物理配置(挿入順オフセット、key順ではない)が主要因であることが実測で確定した。

## なぜ元のT1(flat array)が速く、なぜskip listが遅いか

- **T1(flat array)**: 探索・Scanのいずれも「配列インデックスの算術演算」で完結する。各ステップは
  `arr[idx]`という独立に計算可能なアクセスで、依存関係がない。Scanは物理的に連続した領域を
  読むだけでハードウェアのプリフェッチが完全に効く。1エントリのメモリ占有も小さく、
  キャッシュ・TLB効率が良い。
- **skip list**: 「配列の添字計算」を「独立に確保されたオブジェクトへの、依存関係のある
  ポインタチェイシング」に置き換える。ホップNの次の読み先はホップNのデータが実際に返る
  まで計算できないため、直列化されたランダムメモリアクセスになる。チャンク化でホップ数は
  約12分の1のオブジェクト数に削減できたが、直列アクセスという性質自体は変わらない。

## 検討した代替案

### Packed Memory Array (PMA)

隙間(gap)を伴う配列で、局所的なリバランスのみでO(log²N)償却の挿入を実現し、ソート順を
維持したまま連続領域に近い局所性を保つ。理論的には「flat arrayの局所性」と
「reorganize不要」を両立し得る。

サーベイの結果、汎用のKVS・RDBMSでの採用例は見当たらなかった。実際の採用例は動的グラフ処理の
研究システムに限られる:

- **Teseo**(VLDB 2021、CWI): 頂点・辺をPMAに格納し、ARTでインデックスする"FAT(fat tree)"構造。
  **フルlock-freeではなく**、辺のパーティション単位の排他/共有ロック+楽観的並行性制御(OCC)。
- **PCSR (Packed Compressed Sparse Row)**: CSR形式の辺配列をPMAに置き換えた設計。
- **DistPCSR**: 複数マシンのPCSRを束ねる分散ルーティング層。

PMA自体の局所性への懸念は薄いが、局所リバランスを並行更新下で安全に行うには対象window単位の
ロックが必要になり、pskiplistが持つ「フルlock-free」という性質を手放すことになる。この
妥協は上記の実システムでも共通して選択されており、理論だけでなく実例からも裏付けられる。

参考文献:
- Bender, M.A., Hu, H. "An Adaptive Packed-Memory Array." ACM TODS.
  <https://dl.acm.org/doi/10.1145/1292609.1292616>
- De Leo, D. "Teseo and the Analysis of Structural Dynamic Graphs." VLDB 2021.
  <http://vldb.org/pvldb/vol14/p1053-leo.pdf>
- Wheatman, B. et al. "A Parallel Packed Memory Array to Store Dynamic Graphs."
  <https://people.csail.mit.edu/hjxu/papers/ppcsr-alenex.pdf>

### Bw-tree / Adaptive Radix Tree (ART)

- **Bw-tree**: delta chain(更新を差分レコードとしてページ先頭に追加)+ 論理ページID→物理
  アドレスの間接テーブルによって、CASのみでフルlock-freeな更新を実現するB+tree系構造。
  Microsoft SQL Server Hekaton、Azure DocumentDB、Bingで本番稼働している実績がある。
- **ART**: 適応型基数木。Optimistic Lock Coupling(OLC)/ROWEXという、フルlock-freeより
  実装が容易な同期方式で並行アクセスに対応。DuckDB、HyPer/Tableauで採用。

両者ともT1Indexのflat arrayより確実に間接参照が増える: Bw-treeは論理ページIDのマッピング
テーブル参照+delta chainの遡り+複数段のページ階層、ARTは基数木のノード階層を辿る必要がある。
fanoutが広い分ホップ数はskip listより少なくなり得るが、1ホップあたりのコストは
flat arrayの単純な配列アクセスより重い。「グローバルなO(N)再編成を無くす」ことと
「読み取りを間接参照ゼロのフラット配列のままにする」は原理的に両立しない
(差分をどこかに逃がす以上、読み取り側が必ず代償を払う)。

参考文献:
- Leis, V. et al. "The ART of Practical Synchronization." DaMoN 2016.
  <https://db.in.tum.de/~leis/papers/artsync.pdf>
- Levandoski, J. et al. "The Bw-Tree: A Latch-Free B-Tree for Log-Structured Flash Storage."
  IEEE Data Eng. Bull. <http://sites.computer.org/debull/A13june/bwtree1.pdf>

### 範囲シャーディング(今後の有力候補)

T1IndexをK個の独立したシャード(それぞれが元のT1Indexと同じflat array + append region +
`reorganize()`)に分割し、境界キーの小さな配列でルーティングする案。`reorganize()`は
該当シャードのみをO(N/K)で処理するため、最悪停止時間を直接コントロールできる。読み取り側は
「元のT1Indexの探索 + シャード選択の数回の比較」のみで、Bw-tree/ARTのような常時発生する
間接参照コストを持たない。

先行研究:
- **PebblesDB**(SOSP 2017): "guards"という境界キーでLSM-treeの各レベルを独立した
  フラグメントに分割し、compactionをそのフラグメントだけに閉じ込めるFragmented LSM-Tree
  (FLSM)を提案。"skip listに着想を得た"と明言。RocksDB比で書き込み増幅2.4〜3倍削減、
  スループット6.7倍向上を実測。「グローバルな一括処理をkey範囲で局所化する」という
  発想の直接の先行事例。
  <https://www.cs.utexas.edu/~vijay/papers/sosp17-pebblesdb.pdf>
- **Bentley, J.L., Saxe, J.B. "Decomposable Searching Problems I."** Journal of Algorithms,
  1980. 静的構造を「バッファ+定期再構築」で動的化する一般理論(logarithmic method)。
  LSM-tree系設計全体の理論的ルーツ。
  <https://www.ime.usp.br/~cris/aulas/19_1_6957/BentleyS1980-DSP.pdf>

未解決の論点: Scanがシャード境界をまたぐ場合の処理。標準的な解はk-way mergeイテレータ
(シャードごとに1つのイテレータを持ち、min-heapで束ねて最小キーから順に返す)で、RocksDBの
`MergingIterator`が同じ仕組みをmemtable/SSTableをまたぐ範囲クエリに使っている
(<https://github.com/facebook/rocksdb/blob/main/table/merging_iterator.cc>)。ただし
複数の物理的に離れた領域にまたがるマージは、単一領域の連続読み出しより確実にオーバーヘッドが
大きい。シャードサイズは「reorganizeの最悪コスト(O(N/K)、小さいほど良い)」と
「Scanがシャードをまたぐ頻度(大きいほど良い)」の間のトレードオフを直接制御するダイヤルになる。

## 決定

- pskiplistはvmemkvのT1としては採用しない。Get/Hit・Get/Miss・Scanの regression が
  実運用を阻む水準で残り、根本原因(ポインタチェイシング vs flat arrayの局所性)は
  skip list系の設計である限り解消できない。
- pskiplist自体は独立したライブラリとして価値があるため、単一リポジトリとして切り出す。
- vmemkvのT1実装は元のT1Index(flat array + append region + `reorganize()`)に戻す。
- 将来的にreorganize()/checkpoint()のコストが再び問題になった場合、次に検討すべき方向は
  PMA/Bw-tree/ARTのようなポインタ構造への転換ではなく、範囲シャーディング
  (T1Indexを独立したK個のシャードに分割し、`reorganize()`をシャード単位に局所化する)。
