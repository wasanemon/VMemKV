# 2026-08-07 ScanをRocksDB/LMDB水準へ: T2 base/tail split + io_uring実データ読み取り

## 背景

`20260806_scan_madvise_tradeoff.md`(結果7・結果8)で、ScanをLTM環境で高速化する試みとして`madvise(MADV_POPULATE_READ)`によるio_uring並列プリフェッチを検証したが、実システムでは最良でもBaseline並みが精一杯で、明確な優位は得られなかった。結果8のmajor fault計測により、原因は「プリフェッチしたページが、実際に使われる前にcgroupの継続的な回収圧力で再度追い出される」("prefetch-then-evict"競合)にあることが判明していた。

本ドキュメントは、この知見を踏まえて再設計した**T2の物理レイアウト変更(base/tail split)とio_uringによる実データ読み取り**(`IoUringScanRealRead`アブレーション)の設計・実装過程・測定結果を記録する。結果7・結果8と異なり、**このアブレーションは採用され、実際にBaselineだけでなくRocksDB/LMDBも上回る性能を達成した**。T2の物理構造そのものに変更を伴うため、`20260806_scan_madvise_tradeoff.md`とは別ファイルとして独立させている。

## 設計

### なぜmmapではなく明示的readなのか

RocksDBは値の読み取りを`mmap`ではなく`pread()`+自前のblock cacheで行っており、iteratorへのreadahead(`ReadOptions::readahead_size`)をアプリケーション側で明示的に制御できる。カーネルのグローバル回収ポリシーに委ねていないため、cgroup圧力下でも「今使うと分かっているページ」を横から奪われない。この設計思想を取り入れる方向で検討した。

結果8で明らかになった通り、`MADV_POPULATE_READ`(ページを温めるだけ)は所有権のないコピーであり、cgroup回収に対して脆弱だった。一方、実際にバイト列を自分のバッファへコピーする読み取り(`pread()`や`io_uring`の`IORING_OP_READ`)は、コピーが完了した時点でそのデータは所有権のあるバッファの中にあり、元のページキャッシュエントリがその後追い出されようと一切影響を受けない。

### 整合性の壁: T2はMAP_PRIVATE

`t2_flat_file.cpp`の該当コメントの通り、T2は明示的に「MAP_PRIVATE: changes are not written back to the underlying file (volatile on restart)」である。同一サイズの上書き更新(`update_value_at()`)はmmap'd領域への`memcpy`だけで完結しており、ファイルディスクリプタへの書き戻しは一切ない。そのため`pread(fd, ...)`で同じオフセットを読めば、in-place更新済みのレコードに対しては**更新前の古い値**が返ってくる——これは以前io_uring経由のfd直接read案を却下した理由と全く同じ壁である。

### 解決策: T2のbase/tail split

T2は元々「reorganizeで書かれたsorted base + 到着順のtail」という構造を持つ(`20260806_scan_madvise_tradeoff.md`結果2)。ここに1つルールを追加する:

- **sorted base領域(直近のfull reorganizeが書き込んだオフセット範囲)へのin-place更新を禁止**し、その更新はtail側へout-of-place(追記+tombstone)で逃がす(`update_impl()`、`vmemkv_impl.hpp`)。
- baseの物理バイトは、それを書いたreorganizeが完了した後、二度と変化しない。したがって**ファイル記述子経由での直接読み取りが常に安全**。
- tailは引き続きin-place更新の対象になり得るため、mmap経由(COW反映済みの最新ビュー)でしか安全に読めない。

新規に追加した`T2Memory::base_boundary`(直近reorganize完了時点の`bytes_used`)が、baseとtailの境界を表す。この境界を境に、`scan_impl()`は候補ごとに「baseならio_uring実read、tailなら既存のmmap+seqlock」を選択する。

**新しいcompactionは増えない**: reorganizeは元々baseを丸ごと書き直す操作で、それは今のままである。変わるのは「base宛の更新を書き込み時にtailへリダイレクトする」という更新パス側の分岐だけ。

### fdの管理

T2のmmap用fdは、mmap確立後すぐにcloseされる(`mmap_t2_memory()`)。base領域の実readには別途fdが要るが、その都度パスから再オープンするのは危険——`T2FlatFile::path_`はコンストラクタ時点の初期ファイルパスのままで、reorganize後の実際のチェックポイントファイルパス(`derive_t2_chk_path()`、checkpoint LSNベースの世代番号を含む)とは異なり、しかも`T2Memory::generation`(ペアリング用のプロセス内グローバルカウンタ)とcheckpoint LSNは別概念のため、`mem->generation`から正しいオンディスクパスを導出する信頼できる方法がない。

解決策: `mmap_t2_memory()`でmmap用のfdを`dup`(`F_DUPFD_CLOEXEC`)し、`T2Memory::read_fd`として同じT2Memoryオブジェクトに保持させる(デストラクタでclose)。これによりread用fdのライフタイムはmmap自体と完全に同期し、パス導出や世代ミスマッチのリスクが原理的に発生しない。

## 実装バグと修正の経緯

### バグ1: ペイロード埋め込みサイズの粒度不一致による値の切り詰め

T1ペイロードに埋め込まれたサイズ情報(`block_count * kBlockAlignment`、16バイト粒度)を素朴に読み取りサイズとして使ったところ、専用テスト(下記)で末尾数バイトが化けた。原因は、実際のレコードは`align_up()`により8バイト粒度でしかアラインされていないため、16バイト粒度に丸めた`block_count`は実際のアラインド長より最大8バイト小さく出ることがある(`aligned_len mod 16 ∈ {0, 8}`)ため。

最初の修正は、読み取りサイズに固定+8バイトのマージンを足し、使用時に`header->key_len + header->value_len`がバッファ内に収まるかを検証し、収まらなければmmap経路へフォールバックする、というものだった。

### ユーザーからの指摘: フォールバック依存の設計を見直す

この時点でユーザーから「フォールバック経路はあまり好ましくない」との指摘を受けた。実際、この修正では**約半数のレコードでフォールバックが発生していた**(アラインド長が16の倍数でない場合は必ず短小読みになるため)——高速パスが実質的にあまり使われないまま「動いているように見える」状態だった。

再設計: 候補レコードをオフセット順にソートし、各レコードを「次の候補のオフセットまで」読む。base領域内のレコードは直近reorganizeが書いた順(=キー昇順)に隙間なく並んでおり、in-place更新でbase領域が二度と書き換わらないことも保証されているため、この差分は常にレコードの真の物理長以上になる——**推測ではなく構築上の保証**である。最後の候補(このスキャン範囲内での最後、T2全体の最後とは限らない)だけは次の候補が分からないため、埋め込みサイズ+マージンにフォールバックする(こちらは1レコードだけの例外的経路)。

### バグ2: 範囲限定スキャンでの巨大読み取り

再設計直後、100件だけのスキャン(コーパス全体は826万キー)でBaseline比100倍以上遅くなる回帰が発生した。デバッグ出力で原因を特定: 「最後の候補」の読み取りサイズ計算に誤って`base_boundary`(T2全体の終端、約7.1GB)を使っていたため、スキャン範囲内で最後の候補が実際にはT2全体の終端まで届いていないにもかかわらず、7.1GBの単一読み取りが発生していた。

修正: 「次の候補」がある場合はその差分、ない場合(このスキャン範囲内での最後)は埋め込みサイズ+マージン(バグ1の修正を再利用、`base_boundary`でクランプ)を使う、という設計に確定した。これによりEOFの懸念も自然に解消される(常にファイルの論理末尾以内で完結する)。

### 正しさの検証

新規に`test_kv_store.cpp`へ専用テストを追加: reorganize後にbase領域のレコードを同サイズ更新し、その更新がout-of-placeへ正しくリダイレクトされること、Scan(io_uring経路)とGet(mmap経路)が更新後の値で一致することを確認する。既存テストの`VMemKVStores`マクロにも`IoUringScanRealRead`を追加し、CRUD/Scanの全既存テストで本アブレーションが確実に運用される経路を通るようにした。liburing有効ビルドで326件の全テストがパス。

## 測定結果

すべてi4i.2xlarge(8vCPU)またはi4i.8xlarge(32vCPU)、1GiB LTM cgroup(8倍オーバーサブスクリプション)、`drop_caches`を挟んだクリーンな計測。

### なぜ効くのか: 分離実験(chunk数 × readahead)

i4i.2xlarge、1KB/Zipf/threads:1:

| 構成 | 時間 | vs Baseline |
|---|---:|---:|
| chunk=8(既定)、通常readahead | 579.8μs | 3.22倍速い |
| chunk=1、通常readahead | 601.2μs | 3.11倍速い |
| chunk=8、`POSIX_FADV_RANDOM`強制 | 1465.9μs | 1.27倍速い |
| chunk=1、`POSIX_FADV_RANDOM`強制 | 1903.7μs | ほぼBaseline並み |
| Baseline | 1867.1μs | — |

chunk数(io_uringの並行発行数)はほぼ無関係。`POSIX_FADV_RANDOM`でreadaheadを殺すと性能がBaseline付近まで落ちる。結論: `read_fd`はT2のmmapとは別のfdであり、mmapに常時かけている`MADV_RANDOM`を一切引き継がないため、カーネルの通常のreadaheadがタダで効いている——これが優位性のほぼ全量を占める。io_uringの並行性そのものは、この特定の測定では副次的な要因に過ぎなかった。

### RocksDB/LMDBとの比較(1KB, Zipf, i4i.8xlarge)

**方法論上の注意**: 複数のストアを1つの`--benchmark_filter`でまとめて1つのcgroupスコープ内で連続測定すると、後から測定されたストアほど不当に有利になるアーティファクトが生じることが判明した(cgroupの回収状態が前のストアの測定で「温まる」ためと見られる)。以下は各ストアを完全に独立したcgroupスコープ・`drop_caches`付きで測定し直した、信頼できる数値である。

| スレッド数 | IoUringScanRealRead | RocksDB | LMDB | Baseline |
|---|---:|---:|---:|---:|
| 1 | **643μs** | 1004μs | 820μs | 2071μs |
| 4 | **150μs** | 230μs | 184μs | 442μs |
| 16 | **51μs** | 67μs | 77μs | 123μs |
| 32 | **37μs** | 38μs(ほぼ同着) | 63μs | 67μs |

全スレッド数でIoUringScanRealReadが最速、または実質同着。32スレッド(i4i.8xlarge本番相当インスタンスでの最大並行数)でもハングは一切発生しなかった——結果7・結果8で見られた「並行発行がcgroup圧力下でハングする」問題は、実データをコピーして所有権を得る本設計では再現しなかった。

chunk数の分離実験も同条件・独立測定でやり直したが(1〜32でスイープ)、差は12%程度に収まり、既定値(`hardware_concurrency()`)のままで問題ないことを確認した。

### 64KB値サイズでの比較(i4i.2xlarge、独立測定)

| Store | Zipf | Uniform |
|---|---:|---:|
| IoUringScanRealRead | 10.70ms | 12.17ms |
| RocksDB | 19.71ms | 24.93ms |
| **LMDB** | **9.45ms** | **9.71ms** |
| Baseline | 108.92ms | 152.04ms |

64KBではLMDBがわずかに(13〜25%)上回る。RocksDBには2倍近く勝り、Baselineには10〜12倍の差をつけている。1KBの場合と異なり、この64KB比較では複数ストア連続測定によるアーティファクトの影響は見られなかった(独立測定前後でほぼ同じ数値)——アーティファクトが顕在化する条件(高並行・小さい値サイズでの短時間スキャン)に依存するものと考えられる。

## 並行性の検証(ThreadSanitizer)

正しさの検証(上記)は単一スレッドでのシナリオに留まっていたため、`update()`・`scan()`・`reorganize(true)`を並行に走らせる専用のストレステストを追加した(`test_kv_store.cpp`、"concurrent update+scan survive repeated reorganize")。50件のキーに対し、更新スレッドが同サイズ値でランダムなキーを更新し続け、スキャンスレッドが範囲スキャンを回し続け、メインスレッドが`reorganize(true)`を60サイクル実行する。書き込む値は常に単一文字の200バイト反復列とし、torn read(新旧バイトの混在)が起きればスキャン側で直接検出できるようにしてある。

liburingをローカルにソースからビルドし(root不要、`./configure --prefix=... && make install`)、ThreadSanitizer(`-fsanitize=thread`)付きでビルドして検証した。WSL2環境ではASLRとTSanのシャドウメモリ配置が衝突し`unexpected memory mapping`で即クラッシュしたため、`setarch $(uname -m) -R`でASLRを無効化して実行している。

**結果**: 8回の完走で、本ブランチの新規コード(`update_impl`のbase_boundary判定、`prime_base_read_cache`/`scan_impl`のio_uring経路)を発生源とするデータ競合はTSanに一度も検出されなかった。torn readの自己検証(値の一様性チェック)も全て通過。

**一方で、本ブランチと無関係な、既存コードの並行性に関する複数の問題を発見した**:
- `reorganize_internal()`には既にコメントで記録されている"KNOWN OPEN ISSUE"(未解決の既知の問題、専用の意図的に失敗するテストが既存)があり、150サイクル(60ではなく)でのストレステストはこれを実際に踏み抜いてプロセスをクラッシュさせた。
- 既存の回帰テスト("reorganize's T2 record read survives a concurrent in-place update"、"scan survives a T2-rebuilding reorganize running concurrently"、"reorganize_internal()'s drain barrier...")を(本ブランチの変更を一切含まない)`VMemKVStore`構成でTSan下に走らせたところ、同種の"データ競合"(`update_value_at()`のmemcpyと、seqlockで保護されているはずの読み取りとの競合)が同様に報告された。これはseqlockのリトライプロトコルが素の(非atomic)読み書きに依存しているためで、C++の厳密なメモリモデル上は技術的にデータ競合だが、バージョン確認による再試行で実害を防ぐ設計だと考えられる——ただし、この前提自体を検証はしていない。
- これらはすべて本ブランチ以前から存在する挙動であり、本ブランチのスコープ外と判断して深追いはしていない。ただし、このプロジェクトが正しさの根拠として重視してきたTSan検証が、これまで実際には(少なくとも本セッションでは)一度も通っていなかったことを示しており、別途対応を検討する価値がある。

## 結論

**採用。** `IoUringScanRealRead`アブレーションとして実装済み(既定では無効、opt-in)。当初目標だった「RocksDBに並ぶ」は全条件で達成し、多くの条件(1KB、全スレッド数)でRocksDB・LMDBの両方を上回った。64KBの大きい値ではLMDBにわずかに届いていないが、Baseline比では引き続き大差の改善である。

**副産物として得られた重要な教訓**: cgroup制約下のベンチマークで複数のストア/構成を同一cgroupスコープ内で連続測定すると、後発の構成が不当に有利になるアーティファクトが生じ得る。この種の比較は、各構成を独立したcgroupスコープ・`drop_caches`付きで測定しない限り信頼できない。本セッションの前半(高並行測定)はこのアーティファクトに気づかず誤った結論(IoUringScanRealReadが高並行でRocksDB/LMDBに負ける)を報告してしまった。今後のLTM系ベンチマーク(このリポジトリの既存の`run_bench_aws_c6id.sh`によるRocksDB/LMDB/VMemKVの一括比較を含む)にも同じ懸念が当てはまる可能性があり、要検証。

**未解決・今後の課題**:
- 64KBなど大きい値でLMDBとの差を詰める方向性(base領域の読み取り単位をより大きくまとめる、複数レコードをマージして1回のio_uring readにする、など)は未着手。
- `run_bench_aws_c6id.sh`本体の複数ストア一括測定が、本ドキュメントで見つけたアーティファクトの影響を受けていないか要確認。
- T1-onlyのreorganize(4.2節)はT2に触れないため`base_boundary`は変化しないが、繰り返しT1-onlyのreorganizeが起きる間にtail領域の断片化が進んだ場合の挙動は未検証。
- 上記のThreadSanitizer検証で見つかった、本ブランチと無関係な既存の並行性の問題(reorganize_internal()のKNOWN OPEN ISSUE、seqlockパターンのTSan上のデータ競合)は未対応のまま。

## 追記 (2026-08-07): io_uring依存の撤去、`ScanBaseSequential`への置き換え

`IoUringScanRealRead`をfull matrixに組み込み、AWSで再計測したところ、in-memory・高並行(16〜32スレッド)・実値サイズ(1KB)条件でScanが以前の最良構成(Var3、io_uringなし)比0.30〜0.37倍まで落ち込む結果が出た。これを追跡調査した結果、当初の結論(「io_uring自体は必要だったのか」)を覆す発見があった。

**発見1: 「退行」の大部分は測定ノイズだった。** `--benchmark_repetitions=8`で厳密に再測定すると、比較対象だったmmapベースの旧構成自体が同一セルで62k〜258k items/sの間で暴れる(CV 20〜40%)ことが判明した。原因は`Prefaulting`がinsert時のページしかプリフォルトしておらず、reorganizeが作る新しいmmapには一切効いていなかったこと——「reorganize直後の最初のScan」は毎回コールドなページフォルトの嵐になっていた。一方`IoUringScanRealRead`自身の数値はCV 0.3〜6%と終始安定していた(明示的readはページフォルトを経由しないため)。

**発見2 (本質的): io_uring自体が必要だったのか、という再検討。** 当初の設計方針(結果6・上記「なぜ効くか」節)は既に「効いているのはio_uringの並行性ではなく、mmapの`MADV_RANDOM`を経由しないreadahead」であることを示していた。であれば「base領域を、`MADV_RANDOM`ではなく`MADV_SEQUENTIAL`を与えた**別のmmap**として読む」だけで同じ効果が得られ、io_uring/libyuring依存もowned bufferへのコピーも一切不要なのでは、という疑問が生じた。

standaloneのマイクロベンチマーク(mmap+MADV_RANDOM / mmap+MADV_SEQUENTIAL(別VMA) / pread の3方式を、cold(drop_caches後)とwarm(ページキャッシュ常駐)の両方、8GBファイル・100KBウィンドウで比較)で検証したところ:

- **Cold(実ディスク読み取り、LTM相当)**: `mmap+MADV_RANDOM`比で`mmap+MADV_SEQUENTIAL`は12.1倍、`pread`は12.5倍——両者の差はわずか4%。
- **Warm(ページキャッシュ常駐、in-memory相当)**: `mmap`(どちらのmadviseでも同一)が`pread`より6〜26%**速い**(スレッド数依存)。io_uring/preadは1回のsubmit/wait syscallペアという下限コストを必ず払うが、warmなmmapは単なるメモリロード命令でそのコストがゼロだから。

つまり「base領域を別mmap+`MADV_SEQUENTIAL`にする」だけで、coldではio_uring版とほぼ同等、warmではio_uring版より明確に高速という結果になった。これを受けて、`IoUringScanRealRead`(io_uring/liburing依存、`prime_base_read_cache`の2パス構造、owned bufferコピー、chunk_countチューニング)は全面的に撤去し、`ScanBaseSequential`アブレーションに置き換えた: base領域専用の第二のread-only mmap(`MADV_SEQUENTIAL`、`Prefaulting`併用時は`MAP_POPULATE`で事前ウォームアップ)を作り、`scan_impl()`はそこから直接読む(seqlock不要、priming passも不要——candidateごとにその場で`base_mmap + offset`を読むだけ)。

**結果**: io_uring/liburingへの外部依存が完全になくなり、実装は`prime_base_read_cache()`(候補収集・ソート・delta計算・io_uringバッチ submit/wait)がまるごと不要になるぶん大幅に簡素化された。性能はLTMで同等(cold時±4%以内)、in-memory高並行でむしろ改善(warm時+6〜26%)。詳細な設計は`low_level_design.md` 7.9節、タグは`vmemkv::ScanBaseSequential`(`config.hpp`)を参照。

この経緯は、当初「io_uringのおかげ」と早合点していた性能改善の実体が、実は「per-file readahead policyをGet側のMADV_RANDOMから切り離せたこと」だけだった、といういい教訓でもある——分離実験(結果6)が既にそれを示唆していたのに、そこから「ならmmapのままでも同じでは」という一歩をこのタイミングまで踏み出していなかった。

## 参照

- `implementation/docs/benchmark/20260806_scan_madvise_tradeoff.md` — 本ドキュメントの前提となる結果7・結果8(madvise-populate方式の不採用)。
- `implementation/docs/specification/low_level_design.md` 2.2節(T2 base/tail split)、3.3節(Update時のin-place迂回)、7.9節(`ScanBaseSequential`の要約、上記追記を反映済み)。
- ブランチ: `scan-t2-base-tail-io-uring-read`。
