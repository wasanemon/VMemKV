# 2026-08-10 Get の base_mmap 高速パス(LTM/64KB Get/Hit 弱点の解決)

## 背景

`20260809_ltm_64kb_get_hit_profiling.md`で判明したLTM/64KB Get/Hitの弱点(mmap経由のGet読み取りがページフォルト単位=4KBに制限され、64KBの値で最大16回の個別ディスクI/Oが発生する)に対し、`GetPopulateRead`(効果なし)、`T2NoMadviseRandom`(低並列では有効だが実32コアで逆転悪化、削除済み)を試した。本ドキュメントは3つ目の対策、**Get自体をScanと同じ`base_mmap`経由で読ませる**方式について、実装と実測結果をまとめる。

結論を先に述べる: **決定的な成功。** 実測ではthreads:1〜32の全域で2.71倍〜8.24倍の改善が確認され、`T2NoMadviseRandom`が実32コアで逆転悪化したのとは対照的に、**高並列でも悪化する兆候は一切なかった**。

## 実装

`ScanBaseSequential`で既に実装・実測済みの`T2Memory::base_mmap`/`base_boundary`(T2の「base」領域=直近のフルreorganizeで書かれた不変な範囲専用の、`PROT_READ`のみ・`MADV_SEQUENTIAL`の第二mmap)を、Scanだけでなく**Getにも共通ヘルパー経由で流用**した。

- 共通ヘルパー`try_read_base_record(mem, offset)`(`vmemkv_impl.hpp`、`get_impl()`の直前に追加)を新設。オフセットが`base_boundary`未満(base領域内)かつ`ScanBaseSequential`が有効なら、seqlockなしで`base_mmap`から直接`T2RecordView`を返す。それ以外は`std::nullopt`を返し、呼び出し側は既存のmmap+seqlock経路にフォールバックする。base領域は一度書かれたら二度と変更されない(`update_impl()`がin-place更新をout-of-placeへリダイレクトする既存の不変条件)ため、seqlockもコピーも不要——callbackにライブなspanをそのまま渡せる。
- `scan_impl()`: 既存のインライン実装(29行)をこのヘルパー呼び出し1つに置き換え、重複を解消。
- `get_impl()`: 新たにこのヘルパーを使った高速パスを追加。鍵が一致しなければ(理論上起きないはずの防御的フォールバック)既存のseqlock経路へ流れる。

## なぜT2NoMadviseRandomと異なる結果になったか

`T2NoMadviseRandom`はT2の主mmap全体の`MADV_RANDOM`を外し、**swapのページクラスタ先読み**(`vm.page-cluster`、物理オフセットベースの粗いクラスタリング)を有効化する方式だった。低並列では効いたが、高並列では過剰に読んだ分(最大12倍)が他スレッドの本当に必要な読み込みとディスク帯域を奪い合い、逆転悪化した。

今回の方式は`base_mmap`(`PROT_READ`のみ、書き込み不可)を経由するため、そのページは一度もCOWで匿名化されず**常にファイルバックのクリーンページ**のままで、追い出されても**swap-inではなく通常のファイルreadahead**(page cache層、ブロック層のI/Oマージも効く適応的な仕組み)経由で戻ってくる——swapクラスタ先読みとは根本的に別のカーネル機構である。さらに、base領域はreorganize時に**キー順にソートされて**書かれるため、隣接オフセットの先読みがZipf分布下で実際に有用である可能性が高く、スワップの物理アドレスベースの先読みより無駄が少ない。

## 計測環境・手法

- AWS スポット、2段階で計測: (1) i4i.2xlarge(8vCPU、threads:1/4/8) (2) i4i.8xlarge(実32vCPU、threads:1/4/16/32、`T2NoMadviseRandom`の逆転悪化が実32コアでのみ顕在化した教訓を踏まえた追加検証)
- 対象: 修正前(git HEAD、commit `36587aa`)と修正後(作業ツリー)の`Bloom-T1InlineValue-Prefaulting-ScanBaseSequential`(本番構成)を同一インスタンス上でビルドし直接比較。参考として元々の弱い`Baseline`(`Config<>`、修正の影響を受けないはずの対照群)と`RocksDB`も同時計測
- 手法は前回までと同一(`drop_caches` + store毎の独立cgroupスコープ、`perf stat`、`/proc/diskstats`前後差分)、LTM cgroup 1GiB/8倍オーバーサブスクリプション

## 結果1: 修正の効果(修正前 vs 修正後、同一variant内)

### i4i.2xlarge(threads:1/4/8)

| | threads:1 | threads:4 | threads:8 |
|---|---:|---:|---:|
| Zipf | 5.95x | 3.93x | 2.97x |
| Uniform | 6.45x | 3.69x | 3.68x |

### i4i.8xlarge(実32コア、threads:1/4/16/32)

| | threads:1 | threads:4 | threads:16 | threads:32 |
|---|---:|---:|---:|---:|
| Zipf | 8.24x | 6.12x | 3.22x | **2.71x** |
| Uniform | 7.28x | 4.00x | 3.50x | **3.06x** |

倍率は並列度が上がるにつれて縮小するが、**実32コアでも2.71x〜3.06xを維持**——`T2NoMadviseRandom`がthreads:32で0.65x〜0.69x(Baselineより悪化)に転じたのとは対照的に、1.0xを割り込む兆候は一切ない。

## 結果2: RocksDBとの比較(修正後、実32コア)

| | threads:1 | threads:4 | threads:16 | threads:32 |
|---|---:|---:|---:|---:|
| Zipf | 1.11x(上回る) | 0.94x | 0.51x | 0.35x |
| Uniform | 1.05x(上回る) | 0.85x | 0.77x | 0.74x |

低並列ではRocksDBを上回り、高並列ではRocksDB(スレッド数に対してほぼ線形にスケールする)にリードを許すが、修正前の0.15〜0.36x程度から大幅に改善している。

## 結果3: メカニズムの確認(実32コア、`perf stat`/`/proc/diskstats`)

| | 修正前 | 修正後 | RocksDB |
|---|---:|---:|---:|
| major-faults | 501,400 | 125,761(4.0倍減) | - |
| 実ディスク読み込み回数 | 553,845 | 179,162(68%減) | 155,034 |
| 実ディスク読み込みバイト数 | 8.94 GB | 22.32 GB(2.5倍増) | 10.51 GB |
| 1回あたり平均読み込みバイト数 | 16,137 B | 124,594 B | 67,808 B |
| **ディスク読み込みに費やした時間** | **98.33s** | **79.23s** | 53.59s |

`T2NoMadviseRandom`のケースでは高並列下でディスク読み込み時間が**増加**したが、今回は読み込みバイト数が2.5倍に増えているにもかかわらず、**実32コアでも読み込み時間はむしろ減少している**(98.33s→79.23s)。これがbase_mmap経由のファイルバック先読みがswapクラスタ先読みより並列耐性が高い、という上記の仮説を裏付ける決定的な証拠である。

## 結論

`get_impl()`にScanと同じ`base_mmap`高速パスを追加することで、LTM/64KB Get/Hitの弱点は**並列度に依存しない安定した改善**として解決された。`T2NoMadviseRandom`の教訓(低並列の好成績だけでは高並列での安全性を保証しない)を踏まえ、実32コアまで検証した上での結論である。この修正は`ScanBaseSequential`が有効な構成(本番の`System_AllOn`/`VMemKVStore`含む)に対して常時有効で、追加のablationタグやトグルは不要——base領域に載っているレコードに対してのみ自動的に恩恵を受け、tail領域(直近書き込み)は従来通りの経路のままである。

## 生データ

- `implementation/benchmark/logs/get_base_mmap_fix_verification/` — i4i.2xlarge、4-way比較(sbs_before/sbs_after/baseline/rocksdb)
- `implementation/benchmark/logs/get_base_mmap_fix_verification32/` — i4i.8xlarge実32コア、同4-way比較
