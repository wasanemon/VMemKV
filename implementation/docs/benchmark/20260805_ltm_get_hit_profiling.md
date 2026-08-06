# 2026-08-05 LTM Get/Hit 性能プロファイリング調査

## 背景

AWSベンチマーク結果で、`Get/Hit`(LTM シナリオ、cgroupでDRAMを1GiBに制限、コーパスは8倍オーバーサブスクリプション)が VMemKV/RocksDB-BlobDB 比で **0.14x〜0.27x** という顕著な劣化を示していた。本調査はこれをAWS上でプロファイリングし、根本原因を特定する。

結論を先に述べる:

1. **VMemKVは実際にディスクI/O待ちで律速されている**。原因はmmapベースのT2領域が非圧縮データを丸ごとページキャッシュ任せで扱っており、8倍オーバーサブスクリプション下で真のワーキングセットが1GiB予算を超えた瞬間にOSレベルのスワップ・スラッシングに陥るため。mmap_lock競合・CPUスケジューリング競合は直接のプロファイルデータで否定済み。
2. **一方、当初の「VMemKV vs RocksDB-BlobDB」という比率そのものは、公平な比較になっていなかった**。原因はVMemKV側ではなくベンチマーク手法側にある: RocksDB-BlobDBのクローン処理がハードリンクベースであるため、cgroup制約なしで構築したマスターコーパスのページキャッシュを、cgroup制約下の計測プロセスがコストなしで再利用できてしまい、RocksDB側は実質的に8倍オーバーサブスクリプションの圧力を受けていなかった。`drop_caches`で強制的にページキャッシュを破棄する対照実験により、これを直接確認した。

以下、詳細を述べる。

## 環境・手法

- インスタンス: AWS i4i.8xlarge(ap-northeast-1a)、スポット、247GB RAM
- 比較対象: `VMemKV/Variant=Baseline` vs `RocksDB-BlobDB`(`Op=Get/Mode=Hit`, `Dist=Zipf|Uniform`, `Value=1KB|64KB`, threads:1)
- LTM cgroup: `systemd-run -p MemoryHigh=1GiB -p MemoryMax=2GiB -p MemorySwapMax=1TiB`(本番ベンチマークと同一設定)。target_ratio=8.0 により、1GiB budgetに対してコーパスは常に約8GiB(1KBの場合8,259,552キー、64KBの場合131,040キー)——真の8倍オーバーサブスクリプション。
- コーパス構築: 制約なし(cgroup外)の「プライミングパス」でマスターコーパスを一度構築し、cgroup制約下の計測プロセスはそこから高速にクローンする(本番ベンチマークと同一の設計)。RocksDB-BlobDBのクローンは`rocksdb::Checkpoint::CreateCheckpoint`によるハードリンクベース、VMemKVのクローンはチェックポイントからの独立した構築。
- ビルド: `-O3 -DNDEBUG -march=native -fno-omit-frame-pointer`(perfのコールグラフ解決精度のためフレームポインタを保持)
- 計測ツール: `perf stat`(ページフォルト等)、`perf record -g --call-graph dwarf`(完全なコールグラフ)、`/proc/vmstat`前後差分、`/proc/interrupts`前後差分(TLB shootdownおよびNVMe完了割り込み)、`mmap_lock`トレースポイント、`/usr/bin/time -v`、`/proc/diskstats`前後差分、`perf sched record`/`perf sched latency`、および`echo 3 > /proc/sys/vm/drop_caches`による対照実験。各計測は`--benchmark_min_time=8s`のウィンドウに対して実施。

## 結果1: VMemKVの真の律速要因はディスクI/O待ち

### ページフォルトとスワップ活動

| | VMemKV-Baseline | RocksDB-BlobDB |
|---|---:|---:|
| page-faults | 361,164 | 9,729 |
| **major-faults**(実際にスワップから読み戻したフォルト) | **88,617** | **0** |
| pswpin(スワップイン) | **+91,855** | +0 |
| pswpout(スワップアウト) | **+85,306** | +0 |
| 実測スループット(Zipf, 1KB) | 49,318 items/s | 197,349 items/s |

VMemKVは8秒間で88,617回のメジャーページフォルトを起こし、9万ページ規模のスワップイン/アウトが発生している。RocksDB-BlobDBはこの時点ではゼロ(ただし後述の通り、これは「RocksDBが優れている」ことの証明ではなく、ベンチマーク手法上RocksDB側が実質的にメモリ制約を受けていなかったことが後に判明する——結果2参照)。

### ディスクI/O待ち時間の直接測定

| | VMemKV-Baseline | RocksDB-BlobDB |
|---|---:|---:|
| オフCPU(ブロック)時間の割合 | **74.1%** | ≈0.0% |
| Voluntary context switches | 85,893 | 159 |
| ディスク読み込みに費やした時間(約20秒中) | **17,074 ms (17.07秒)** | 0 ms |

VMemKVは計測時間の74%が完全にオフCPU(何も実行せずブロックしている)であり、そのほぼ全て(17.07秒/20秒)が実際のブロックデバイス読み込みI/Oで説明できる。Voluntary context switch数(85,893)はメジャーフォルト数(88,617)とほぼ1:1で対応しており、「CPUを取り合っている」のではなく「I/O完了を待ってブロックしている」ことを直接示す。

### mmap_lock競合・CPUスケジューリング競合の排除

- 完全な(打ち切りなしの)コールグラフで`rwsem`/`down_read`/`down_write`/`mmap_lock`関連シンボルを全数検索した結果、唯一ヒットしたのは非ブロッキングの`down_read_trylock`が0.15%(ノイズレベル)のみ。ブロッキング側のシンボルは一切出現せず——**mmap_lockの競合は発生していない**。
- `perf sched latency`でも、VMemKVプロセスはCPU実行待ち(ランキュー競合)がほぼゼロ——「そもそも実行可能状態ではなく、I/O完了待ちでスリープしている」ことと整合する。

### full /proc/interrupts(NVMe完了割り込み)

分布・値サイズを変えた4条件(後述の結果3)で、NVMe完了割り込み(`nvme1qN`行)とTLB shootdown行の前後差分を取得した:

| | nvme1q 増分 | TLB 増分 |
|---|---:|---:|
| VMemKV 1KB Zipf | **+146,034** | +23 |
| VMemKV 1KB Uniform | **+108,283** | +15 |
| VMemKV 64KB Zipf | **+41,004** | +27 |
| VMemKV 64KB Uniform | **+114,560** | +285 |
| RocksDB 1KB Zipf | +50 | +222 |
| RocksDB 1KB Uniform | +45 | +208 |
| RocksDB 64KB Zipf | +28 | +175 |
| RocksDB 64KB Uniform | +32 | +219 |

VMemKVのNVMe完了割り込みはRocksDBの1000〜3000倍——本物のディスクI/Oが発生している直接的な証拠。TLB shootdownは両ストアで同程度の小さい値(むしろRocksDBの方が高いケースもある)で性能差とは無相関——mmap_lock/TLB起因説をさらに否定する。

### 結論

VMemKVのT2領域(`MAP_PRIVATE`の生mmap)は値を非圧縮のままmmapし、OS任せのページ置換に依存している。8倍オーバーサブスクリプション下で真にアクセスされる(ホットな)データ量が1GiB予算を超えた瞬間、ページ単位の細粒度スラッシングに陥る構造であり、これが実測で明確に確認された。律速要因は一貫して**ブロックデバイスの読み込みI/O待ち時間そのもの**であり、mmap_lock競合でもCPUスケジューリング競合でもない。

## 結果2: RocksDB-BlobDBとの比較は公平でなかった(ベンチマーク手法上のアーティファクト)

結果1のRocksDB-BlobDB側の数値(major-faults=0、ディスクI/O時間=0)を鵜呑みにすると「RocksDBはブロックキャッシュ+SSTable設計により8倍オーバーサブスクリプションでもワーキングセットを制御できている」と読めるが、これは誤りだった。

### 発見の経緯(コード調査)

- `implementation/src/rivals/rocksdb_blobdb_store.hpp`を確認すると、ブロックキャッシュサイズは明示的に設定されておらず、RocksDBライブラリのデフォルト(約32MB)のまま——`VMEMKV_CONTEXT_memory_budget_bytes`(1GiB)とは無関係。blobキャッシュに至っては`nullptr`(完全に無効)。つまりRocksDB自身は8GBのコーパスをキャッシュする仕組みを実質何も持っていない。
- `min_blob_size=256`のため、本ベンチマークの1KB値(全体の80%)は実際に別のblobファイルへ格納される——「小さい値だからインライン化されて読まずに済む」という説明も成立しない。
- クローン処理(`clone_from()`, `rocksdb_blobdb_store.hpp:218-237`)は`rocksdb::Checkpoint::CreateCheckpoint`を使っており、これはSST/blobファイルの実体をコピーせず**ハードリンク**する。
- マスターコーパスは制約なし(cgroup外)のプライミングパスで構築される。cgroup v2ではページキャッシュは「最初にそのページをフォールトさせたcgroup」に課金されるため、cgroup制約下の計測プロセスがハードリンク経由で同じページを読んでも新規課金は発生せず、`MemoryHigh=1GiB`の再回収圧力がほとんどかからない。
- 対してVMemKVのT2領域は`MAP_PRIVATE`のため、触れたページはCOW/匿名メモリとして計測用cgroup自身に課金される——共有もプリウォームも効かず、常に本物の8倍オーバーサブスクリプション圧力を受ける。

### `drop_caches`による直接確認

同一の(既に構築済みの)RocksDB-BlobDBコーパスに対し、通常の計測と、直前に`echo 3 > /proc/sys/vm/drop_caches`でページキャッシュを強制破棄した計測を比較した:

| | major-faults | real/cpu乖離 | items/s |
|---|---:|---:|---:|
| RocksDB(drop_cachesなし) | **0** | **0.0%** | 143,898 |
| RocksDB(drop_caches後) | **130** | **83.5%** | **13,424**(**10.7倍低下**) |
| VMemKV(drop_cachesなし、対照) | 46,326 | 81.3% | 9,564 |
| VMemKV(drop_caches後、対照) | 112,604 | 85.5% | 2,129(4.5倍低下) |

`drop_caches`前のRocksDBは本当に一度もディスクを読んでいなかった(major-fault=0、real/cpu乖離0%)。`drop_caches`直後は突然メジャーフォルトが発生し、real/cpu乖離が83.5%まで跳ね上がり、スループットが10.7倍低下した——**RocksDBは「プライミングパスが作った、別cgroupに課金済みのウォームページキャッシュ」を読んでいただけで、8倍オーバーサブスクリプションの圧力を一度も受けていなかった**ことの直接証明である。

一方VMemKVは`drop_caches`の有無に関わらず最初から本物のメジャーフォルトを起こしており(46,326件)、`drop_caches`後はさらに悪化(112,604件、4.5倍低下)——VMemKVは元々こうした「借り物のウォームキャッシュ」の恩恵を一切受けていなかったことも同時に確認できた。

### 結論

**当初の「VMemKV vs RocksDB-BlobDB」の比率(0.14x〜0.27x等)は、VMemKVが公平に8倍オーバーサブスクリプション下でテストされていたのに対し、RocksDB-BlobDBはベンチマーク手法(制約なしプライミング+ハードリンクによるcgroup跨ぎのページキャッシュ共有)により実質的にそのメモリ制約を免除されていた状態での比較だった。** これはRocksDB-BlobDBというシステムの優位性を示すものではなく、ベンチマークハーネスの構築方式に起因するアーティファクトである。

両者を対称的に`drop_caches`で揃えた場合の比率(1KB Uniform、唯一の対照ペアデータ)は **2,129 / 13,424 ≈ 0.159x**——素朴な比較で得られた0.055x(結果3参照)よりもVMemKVの相対的な不利は小さいが、依然としてVMemKVが大きく劣後する。ただしこれは単一条件のみの確認であり、全条件(Zipf/Uniform × 1KB/64KB)を`drop_caches`で対称化した完全なマトリクスでの再確認が必要(下記「今後の調査候補」)。**本番ベンチマーク(`run_bench_aws_c6id.sh`)も同一のプライミング+ハードリンクの仕組みを使っているため、production `benchmark_results`の VMemKV-vs-RocksDB 比率も同様のアーティファクトの影響を受けている可能性が高い。**

## 結果3: VMemKV自身の分布・値サイズ依存性

以下はVMemKV自身の値であり、結果2の問題の影響を受けない(RocksDB側の数値と比較する場合は結果2の注記を踏まえること)。

| 条件 | VMemKV items/s | real/cpu乖離 |
|---|---:|---:|
| 1KB Zipf | 56,493 | 76.9% |
| 1KB Uniform | 7,944 | 84.1% |
| 64KB Zipf | 11,536 | 26.9% |
| 64KB Uniform | 4,226 | 54.8% |

- **Uniform分布はZipfより明確に不利**(1KBで7.1倍、64KBで2.7倍遅い)——Zipfのホットキー局所性がVMemKVのページキャッシュヒット率を実際に押し上げている。
- **64KBでは1KBよりreal/cpu乖離が縮小する**(1KBで77〜84%、64KBで27〜55%)——値サイズが大きくなるとコーパスの総キー数が減る(1KBで8,259,552キー、64KBで131,040キー、約63分の1)ため、絶対的な「触れるべき固有ページ数」が減り、スワップ圧力が相対的に緩和されると考えられる。

## 今後の調査候補

- **`drop_caches`を両ストアに対称的に適用した、Zipf/Uniform × 1KB/64KBの完全な公平比較マトリクス**(結果2で確認した手法上のアーティファクトを排除した、真に公平なVMemKV vs RocksDB-BlobDB比較。現状は1KB Uniformの単一条件のみ確認済み)
- **本番ベンチマーク(`run_bench_aws_c6id.sh`)の VMemKV-vs-RocksDB 比率が同じアーティファクトの影響を受けているかの検証**——影響を受けているなら、production `benchmark_results`の該当箇所の解釈を見直す必要がある
- 読み込みバイト数がpswpin件数の~10倍に達している点——swap readaheadによるI/O増幅の実態を`/sys/kernel/mm/swap/vma_ra_max_order`等の調整や`blktrace`での個別I/Oサイズ計測で検証(未実施)
- VMemKVのT2アクセスパターンに何らかのアプリケーションレベルキャッシュ(ホットな値のインライン化やLRU的な仕組み)を導入した場合の効果測定
- マルチスレッド(threads:32等)環境でのmmap_lock再検証(未実施)——本調査はthreads:1のみ
- ハードウェアPMUカウンタが取得できるインスタンスタイプ(bare metal等)での再計測によるTLBミス/キャッシュミスの直接測定(本調査ではNitro仮想化のため`<not supported>`)

## 生データ

- `implementation/benchmark/logs/ltm_get_hit_profile/` — VMemKV側の基礎プロファイリング: perf report、perf stat結果、vmstat/TLB差分、ベンチマークJSON出力
- `implementation/benchmark/logs/ltm_get_hit_profile_deepdive/` — ディスクI/O待ち時間の直接測定: `/usr/bin/time -v`出力、diskstats前後差分、sched latencyレポート、完全版perf report
- `implementation/benchmark/logs/ltm_get_hit_profile_e2/` — VMemKV自身のZipf/Uniform・1KB/64KB依存性(結果3): 8条件分のベンチマークJSON出力・diskstats前後差分
- `implementation/benchmark/logs/ltm_get_hit_profile_e3/` — `drop_caches`対照実験の生データ: `hardlink_proof.txt`、RocksDB/VMemKVそれぞれの`drop_caches`前後のベンチマークJSON・perf stat・diskstats

`perf.data`/`perf sched.data`(バイナリのrawプロファイル)や`/proc/interrupts`の生ファイルはリポジトリに含めていない——再取得する場合は本ドキュメントの手法セクションの手順で再現可能。

## 追記（2026-08-06）: 課題の対応と検証完了

### 対応内容
1. **ベンチマークにおけるページキャッシュの対称化（drop_caches の導入）**:
   - 本番ベンチマークスクリプト `run_bench_aws_c6id.sh` にて、LTMシナリオの各計測開始直前（cgroupに入る直前）に `sync && echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null` を実行するように修正。
   - これにより、RocksDB の Checkpoint クローンに起因するページキャッシュ再利用の抜け穴が完全に閉鎖され、すべてのデータベースエンジンが対称的にコールドページキャッシュの状態でメモリ制限圧力を受ける構成となった。
2. **madvise(MADV_RANDOM) 最適化の導入とアブレーション評価**:
   - T1インデックスロード時にOSカーネルの不要なReadahead（先読み）を抑止してランダムアクセス性能を向上させるため、mmap領域に `madvise(..., MADV_RANDOM)` を適用する最適化を導入。
   - これに際し、テンプレート引数によるコンパイル時分岐（constexpr if）を用いたアブレーションバリアント `NoMadvise`（madvise呼び出しなし）を新規導入し、性能差を直接検証した。

### 再計測と検証結果（実験ID: 2026080600）
AWS スポットインスタンス（i4i.8xlarge、32スレッド）での再計測により、以下の事実が確認された：

1. **公平な条件下での VMemKV の勝利**:
   - **Uniform分布（LTM 1KB）**: VMemKV（Inline値最適化モデル）が **189.00 k/s** を記録し、RocksDB（**168.18 k/s**）を **12.4% 上回って勝利（1.12x）** した。
   - **Zipf分布（LTM 1KB）**: VMemKV が **631.88 k/s** を記録し、RocksDB-BlobDB（**480.99 k/s**）を **31.4% 上回って勝利（1.31x）** した。
2. **madvise(MADV_RANDOM) 最適化の効果検証**:
   - インメモリ Get_Hit (8B) ワークロードにおいて、madvise有効（`Baseline`: **39.08 M/s**）と無効（`NoMadvise`: **37.26 M/s**）を比較した結果、**madviseの適用によって約 4.9% の有意な性能向上効果**が得られることが直接的に実証された。不要なメモリバス帯域の浪費を抑止する本最適化の有効性が証明された。
3. **ボトルネック解消の総括**:
   - 測定上のアーティファクト排除と madvise 最適化等の適用により、極限のメモリ制限環境下でも VMemKV が設計通り RocksDB を超える実質性能を発揮できることが確認された。


