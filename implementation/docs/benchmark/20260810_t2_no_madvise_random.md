# 2026-08-10 T2NoMadviseRandom 計測(低並列では大幅改善、実本番並列では逆転悪化 — 最終的にコード削除)

## 背景

`20260809_get_populate_read_prototype.md`で、`MADV_POPULATE_READ`によるフォルト例外の一括化は無効だと判明した。ボトルネックはカーネル内部のI/O発行粒度そのもの(swap-in readaheadが機能していないこと)にあると分かったため、原因の直接候補としてT2の主mmapに適用されている`MADV_RANDOM`(`vmemkv_impl.hpp`のコンストラクタと`mmap_t2_memory()`の2箇所)を調査した。`MADV_RANDOM`は`VM_RAND_READ`としてfile/swap readaheadを`vm.page-cluster`(このカーネルでは3、readahead窓最大8ページ)の値に関わらず無効化する既知の挙動を持つ。

結論を先に述べる: **決定的な効果あり。** `MADV_RANDOM`を外すだけで、LTM/64KB Get/Hitのスループットが**Zipfで約8.0倍、Uniformで約4.2倍**改善し、Zipfワークロードでは**RocksDBを上回る**結果になった(0.36x→1.17x)。Uniformも0.15x→0.66xまで大幅に縮まった。in-memory(スワップ圧なし)の小さい値のGet/Hitに対する退行も観測されなかった。

## 実装

`vmemkv::T2NoMadviseRandom`という新しいConfigタグ(`include/vmemkv/config.hpp`)を追加し、T2の主mmapに`MADV_RANDOM`を適用する2箇所(`VMemKVImpl`コンストラクタの初期mmap、`mmap_t2_memory()`のreorganize/checkpoint時の再mmap)を`if constexpr (!ConfigT::UseT2NoMadviseRandom)`で条件分岐し、このタグが有効な場合は`MADV_RANDOM`を一切呼ばず、カーネルの既定readaheadポリシーのままにした。`VMemKV_ScanBaseSequential`/`VMemKV_GetPopulateRead`と同じ「単独の非累積アブレーションvariant」として`VMemKV_T2NoMadviseRandom`を定義し、`AllPossibleTypes`には含めていない。

## 計測環境

- AWS スポット、i4i.2xlarge、ap-northeast-1a(前回・前々回の調査と同一インスタンスタイプ、同一手法)
- 対象: `VMemKV/Variant=Baseline` vs `VMemKV/Variant=T2NoMadviseRandom` vs `RocksDB`(`Op=Get/Mode=Hit`, `Dist=Zipf|Uniform`, `Value=64KB(20% 8B)`, threads:1/4/8)
- 手法は前回2件と同一(`drop_caches` + store毎の独立cgroupスコープ、`perf stat`、`/proc/diskstats`前後差分)。3ストアとも同一インスタンス上で連続して(独立scope単位で)計測した、同一セッション内のクリーンなA/B/C比較。

## 結果1: スループット

| | VMemKV-Baseline | VMemKV-T2NoMadviseRandom | RocksDB | NoMadviseRandom/Baseline比 | NoMadviseRandom/RocksDB比 |
|---|---:|---:|---:|---:|---:|
| Zipf items/s (threads:1) | 1,062.9 | **8,541.9** | 7,273.7 | **8.04x** | **1.17x(上回る)** |
| Uniform items/s (threads:1) | 764.9 | **3,246.4** | 4,933.5 | **4.24x** | 0.66x |
| Zipf items/s (threads:8) | 22,818.5 | 50,874.1 | 76,793.1 | 2.23x | 0.66x |
| Uniform items/s (threads:8) | 7,908.3 | 12,980.4 | 26,308.4 | 1.64x | 0.49x |

threads:1(スワップ圧が最も顕在化する条件)で最大の改善が出ており、Zipfでは**RocksDBを初めて上回った**。並列度が上がるほど相対的な改善幅は縮むが、それでもBaselineを一貫して上回る。

## 結果2: major-faultsと実I/O粒度

| | VMemKV-Baseline | VMemKV-T2NoMadviseRandom | RocksDB |
|---|---:|---:|---:|
| major-faults | 176,587 | **38,349**(4.6倍減) | 126 |
| 実ディスク読み込み回数 | 176,550 | **90,833**(48%減) | 49,256 |
| 実ディスク読み込みバイト数 | 0.73 GB | **8.99 GB**(12.3倍増) | 3.35 GB |
| **1回あたり平均読み込みバイト数** | **4,119 B** | **99,022 B** | 68,103 B |
| ディスク読み込みに費やした時間 | 19.95s | 23.70s | 10.59s |

`MADV_RANDOM`を外した結果、1回あたりの平均読み込みサイズが**4,119B(ちょうど1ページ)から99,022B(約24ページ)へ**跳ね上がった——RocksDB自身の68,103Bより大きい。これはswap readaheadが`vm.page-cluster`の設定通りに機能し始めたことを直接示す証拠であり、`GetPopulateRead`プロトタイプで確認された「ボトルネックはディスクI/O発行粒度」という診断と完全に整合する。

読み込みバイト数自体は12倍に増えている(過剰な先読みによる無駄も相応にある)にもかかわらず、読み込み回数が48%減り、個々のI/O待ち・コンテキストスイッチのオーバーヘッドが削減された結果、総所要時間・スループットとも大幅に改善している。

## 結果3: in-memory(スワップ圧なし)小さい値での退行チェック

LTMでの改善が、`MADV_RANDOM`が本来助けるはずだったin-memory・小さい値のGet/Hitを犠牲にしていないか、同一インスタンス上で簡易確認した(cgroup制約なし、Value=8B/1KB、threads:1/4/8)。

| | Baseline | T2NoMadviseRandom |
|---|---:|---:|
| 8B/Zipf/threads:1 | 47.4k/s | 91.8k/s |
| 8B/Uniform/threads:1 | 15.1k/s | 741.2k/s |
| 1KB/Zipf/threads:1 | 16.1k/s | 28.1k/s |
| 1KB/Uniform/threads:1 | 9.0k/s | 15.4k/s |

いずれの条件でも退行は観測されず、むしろ全条件でT2NoMadviseRandomの方が高速だった(特にUniform/8Bの49倍という数字は、両variantを同一プロセス内で連続実行した簡易チェックであるため、CPUキャッシュ/TLBのウォームアップ順序による測定バイアスの影響を排除できておらず、鵜呑みにはできない——本番の判断には、他のablationと同様に独立cgroupスコープでの厳密な再測定が必要)。ただし、方向性として「in-memory小さい値でMADV_RANDOMを外すと明確に悪化する」という兆候は一切なかった。

## 結論と示唆

**`MADV_RANDOM`がT2の主mmap全体に無条件で適用されていたことが、LTM/大きい値Get/Hitの性能を大きく損なっていた。** 元々`MADV_RANDOM`はin-memory・小さい値のGet/Hitで約5%の改善を狙って導入されたものだが、LTM・大きい値(64KB)条件下ではその代償(swap readaheadの完全な無効化)の方がはるかに大きかった。

今回の計測は`Get/Hit`のみを対象としているが、`20260809_ltm_64kb_get_hit_profiling.md`の考察通り、Scan中の個々のレコード読み取りやUpdate/YCSB-Eの大きい値アクセスも同じmmap経路を通るため、同様の改善が期待できる。

## 今後の検討事項(未決定)

- 本ablationを`AllPossibleTypes`の累積スタックへ組み込むか(単独variantのまま追加検証を続けるか)は未決定。in-memory小さい値での退行チェックがまだ簡易的(同一プロセス内連続実行、独立cgroupスコープでの再検証なし)なため、この点をユーザーと相談して決める。
- 読み込みバイト数が12倍に増えている点(過剰な先読み)が、より大きなワーキングセットやメモリ帯域が逼迫する条件下でどう影響するかは未検証。
- `MADV_RANDOM`を完全に外すのではなく、`vm.page-cluster`相当のより控えめな先読みだけを有効化する中間的なポリシー(例えば値サイズに応じた条件付きmadvise)も選択肢としてあり得るが、今回の結果は「まず外してみる」のシンプルな案で十分に大きな効果が出ることを示した。

## 生データ

- `implementation/benchmark/logs/ltm_64kb_t2_no_madvise_random/` — `perf stat`出力・ベンチマークJSON・`/proc/diskstats`前後差分(Baseline/T2NoMadviseRandom/RocksDBの3系統)、および in-memory退行チェックの生JSON(`result_inmem.json`)

## 追記(2026-08-10, 同日): 厳密な再検証 — in-memory退行チェックの再実施、および高並列(threads:16)での効果減衰の確認

上記「結果3」のin-memory退行チェックは同一プロセス内で両variantを連続実行する簡易テストで、CPUキャッシュ/TLBのウォームアップ順序バイアスを排除できていなかった。また、Zipf/threads:1のスループット改善(8.04x)が並列度を上げても保たれるのか(=`MADV_RANDOM`削除を本採用してよいか)も未確認だった。この2点をAWS上で再検証した。

### Part A: 厳密なin-memory退行チェック(variantごとに独立プロセスで実行)

Baseline/T2NoMadviseRandomをそれぞれ**別プロセス**として交互に実行(ウォームアップ順序バイアスを排除)、`--benchmark_repetitions=5`で統計的に安定させ、中央値(median)で比較した(値サイズ8B/1KB、Zipf/Uniform、threads:1/4/8)。

| | Zipf t1 | Zipf t4 | Zipf t8 | Uniform t1 | Uniform t4 | Uniform t8 |
|---|---:|---:|---:|---:|---:|---:|
| Value=8B (倍率) | 1.05x | 1.09x | 1.06x | 1.08x | 1.13x | 1.05x |
| Value=1KB (倍率) | 0.97x | 0.99x | 0.99x | 1.05x | 0.99x | 1.08x |

**すべての条件で±13%以内**——退行なし、改善もほぼノイズレベル。当初の簡易チェックで見えた「Uniform/8B/threads:1で49倍」という数字は、案の定、同一プロセス内の順序バイアス(後に実行した方がCPU/TLBの温まった状態の恩恵を受ける)による測定アーティファクトだったことが確認された。**結論: in-memoryの小さい値のGet/Hitに対する退行は、厳密な手法でも見られない。**

### Part B: LTM/64KB Get/Hitの並列度依存性(threads:16まで拡張)

`VMEMKV_BENCH_MAX_THREADS=16`でthreads:16を追加登録し、同一の独立cgroupスコープ手法で再測定した。

| | Baseline | T2NoMadviseRandom | RocksDB | NoRandom/Baseline | NoRandom/RocksDB |
|---|---:|---:|---:|---:|---:|
| **Zipf** threads:1 | 1,076.2/s | 8,545.0/s | 7,241.7/s | **7.94x** | 1.18x |
| Zipf threads:4 | 5,748.8/s | 29,080.5/s | 33,217.8/s | 5.06x | 0.88x |
| Zipf threads:16 | 29,456.9/s | 63,976.5/s | 100,149.2/s | **2.17x** | 0.64x |
| **Uniform** threads:1 | 758.7/s | 3,807.6/s | 4,894.4/s | **5.02x** | 0.78x |
| Uniform threads:4 | 3,718.3/s | 10,673.7/s | 20,238.5/s | 2.87x | 0.53x |
| Uniform threads:16 | 14,311.4/s | 14,270.7/s | 43,954.8/s | **1.00x(効果消失)** | 0.32x |

ユーザーの仮説通り、**並列度が上がるほど効果は縮小し、Uniform/threads:16では完全に消失した(1.00x)**。Zipfはthreads:16でもまだ2.17xの実利が残っているが、いずれの分布でもthreads:4以降はRocksDBに再逆転される。

**メカニズムを`perf stat`/`/proc/diskstats`で確認**(この計測区間は threads:1/4/16 の合算):

| | Baseline | T2NoMadviseRandom | RocksDB |
|---|---:|---:|---:|
| major-faults | 214,889 | 45,042(4.8倍減、変わらず) | 126 |
| CPUs utilized | 0.735 | 1.827 | 3.053 |
| 実ディスク読み込み回数 | 214,856 | 106,204 | 68,263 |
| 実ディスク読み込みバイト数 | 0.88 GB | 10.56 GB | 4.64 GB |
| **ディスク読み込みに費やした時間** | **26.11s** | **40.28s** | 16.99s |

低並列(threads:1単独)では「結果2」の通り、過剰な先読み(12倍のバイト数)は実質タダだった。しかし複数スレッドを束ねたこの計測では、**T2NoMadviseRandomのディスク読み込み時間(40.28s)がBaseline(26.11s)を上回った**——過剰先読みの無駄が、他スレッドの本当に必要な読み込みとディスクキュー/帯域を奪い合うようになり、「タダ」から「コスト」に転じたことを直接裏付ける。major-faults自体は依然4.8倍少ないままなので、フォルト削減効果そのものは失われていない——並列度が上がるにつれて、過剰I/O(waste)のコストがフォルト削減の恩恵を侵食していく、という構図。

### 結論(更新)

- in-memory小さい値での退行は、厳密な手法でも**確認されなかった**(±13%以内、ノイズレベル)。
- ただし**LTM/大きい値での改善効果は並列度に強く依存し、高並列(Uniform/threads:16)では完全に消失する**。Zipfはより並列耐性がある(threads:16でも2.17x)が、いずれにせよthreads:4以降はRocksDBに劣後する。
- したがって「`MADV_RANDOM`を削除してデフォルトにする」という判断は、**低並列(threads:1〜4程度)のLTMワークロードでは明確な純増**だが、**高並列ワークロードでは中立(Uniform)〜劣化はしないが優位性も失う(Zipf)**という、当初考えていたよりも条件付きの効果である。本番のワークロードの並列度プロファイル次第で判断が変わりうる。

## 生データ(追記分)

- `implementation/benchmark/logs/t2_no_madvise_random_verification/` — Part A(`parta_*.json`、variantごと独立プロセス・5反復)、Part B(`resultb_*.json`、`perfb_*.txt`、`diskstatsb_*.txt`、threads:16込み)

## 追記2(2026-08-10, 同日): i4i.8xlarge実32コアでの再検証 — threads:32で逆転(Baselineより悪化)を確認

上記の追記(i4i.2xlarge、8コアへのオーバーサブスクリプションでthreads:16まで確認)では、Uniformの優位性がthreads:16でちょうど1.00xまで縮む(効果消失)ところまでは分かったが、「そこで下げ止まるのか、さらに悪化するのか」は未確認だった。本番相当の実コア数(i4i.8xlarge、32vCPU実コア)で、threads:32まで再検証した。

### Part A: in-memory退行チェック(実32コア、threads:1/4/16/32)

variantごと独立プロセス、5反復、中央値比較。全条件で**0.98x〜1.16x**——引き続き退行は確認されず、むしろわずかに有利な傾向すらある。in-memory小さい値については、並列度によらず安全と言える。

### Part B: LTM/64KB Get/Hit(実32コア、threads:1/4/16/32)

| | threads:1 | threads:4 | threads:16 | threads:32 |
|---|---:|---:|---:|---:|
| Zipf(vs Baseline) | 7.97x | 4.81x | 2.26x | **0.69x(逆転・悪化)** |
| Uniform(vs Baseline) | 4.62x | 3.01x | 1.09x | **0.65x(逆転・悪化)** |

**threads:32で、ZipfもUniformも両方ともBaselineを下回った。** i4i.2xlargeでの「threads:16で効果が消失(1.00x)」という結果は、実は「下げ止まり」ではなく「まだ減衰の途中」だった——本番相当の実32コアでは、**Baselineの方が速い**という完全な逆転に至る。RocksDBとの比較では、threads:32でNoRandom/RocksDB=0.26x(Zipf)/0.35x(Uniform)まで開き、低並列時の優位はすっかり失われている。

**メカニズムを`perf stat`/`/proc/diskstats`で確認**(threads:1/4/16/32合算区間):

| | Baseline | T2NoMadviseRandom | RocksDB |
|---|---:|---:|---:|
| major-faults | 621,676 | 67,744(9.2倍減) | 125 |
| CPUs utilized | 1.868 | 2.317 | 4.754 |
| 実ディスク読み込み回数 | 621,606 | 161,184 | 160,406 |
| 実ディスク読み込みバイト数 | 2.55 GB | 15.95 GB | 10.93 GB |
| **ディスク読み込みに費やした時間** | **82.05s** | **104.83s** | 54.14s |

前回(threads:16、8コアオーバーサブスクリプション)で観測した「過剰先読みの無駄が高並列で無料からコストに転じる」構図が、実32コアではさらに顕著に現れている。**T2NoMadviseRandomのディスク読み込み時間(104.83s)がBaseline(82.05s)を明確に上回った**——過剰に読み込んだ15.95GB(Baselineの6.3倍)が、32並列という高い同時要求下でディスクキュー・帯域を奪い合い、フォルト削減の恩恵(9.2倍減)を完全に飲み込んでいる。

### 結論(最終)

**`T2NoMadviseRandom`は、低並列(threads:1〜4程度)のLTM/大きい値ワークロードでは明確な純増(最大8倍)だが、本番相当の高並列(threads:32)では逆にBaselineより悪化する。** 「悪化しないなら削除してよい」という前提は成り立たない——実際に悪化するケースが実測で確認された。

**判断: `MADV_RANDOM`の削除(恒久的なデフォルト変更)は行わない。**

### 追記3(2026-08-10, 同日): アブレーションのコード自体は削除、本ドキュメントのみ記録として残す

当初は`T2NoMadviseRandom`を切り替え可能なアブレーションとして残す方針だったが、「低並列であることが分かっているデプロイは実際には存在しない」という指摘を受け、方針を修正した。有効化する現実的な条件が存在しない以上、Configタグ・2箇所の`if constexpr`分岐・単独variantをコードに残しておく実用的価値はなく、YAGNI原則に照らして削除するのが妥当と判断した。

- `include/vmemkv/config.hpp`: `T2NoMadviseRandom`タグ、`UseT2NoMadviseRandom`を削除。
- `src/vmemkv_impl.hpp`: コンストラクタと`mmap_t2_memory()`の2箇所の`madvise(MADV_RANDOM)`呼び出しを、`if constexpr`分岐なしの無条件呼び出しに戻した(元の実装と同一)。コンストラクタ側には、この投資判断の根拠(なぜトグルを持たないか)を指す短いコメントを残した。
- `include/vmemkv/vmemkv.hpp`: `VMemKV_T2NoMadviseRandom`の単独variant定義を削除。

340件の既存テストは全て通過。**本ドキュメントは削除したアブレーションの実装内容・実測データ・判断根拠を将来再現するための唯一の記録となる**——もし将来、低並列・大きい値・スワップ圧下という条件に合致する実デプロイが現れた場合は、本ドキュメントの実装セクション(2箇所の`madvise`呼び出しを`if constexpr`で条件分岐するだけ)を元に数分で再実装できる。

## 生データ(追記2分)

- `implementation/benchmark/logs/t2_no_madvise_random_verification32/` — i4i.8xlarge実32コアでのPart A(`parta32_*.json`)、Part B(`resultb32_*.json`、`perfb32_*.txt`、`diskstatsb32_*.txt`、threads:32込み)
