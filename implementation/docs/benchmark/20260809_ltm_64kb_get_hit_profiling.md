# 2026-08-09 LTM/64KB Get/Hit 性能プロファイリング調査

## 背景

`20260805_ltm_get_hit_profiling.md`の調査・対策(drop_caches導入)はLTM 1KBについてのみ再検証されており、64KBは追跡されていなかった。直近のベンチマーク結果(`2026080801`)では、LTM/64KB Get/HitでVMemKVがRocksDB/RocksDB-BlobDB比0.15〜0.29xと大きく劣後している。本調査は、この劣後が(a)1KBと同種の計測アーティファクトの再発なのか、(b)大きい値サイズに特有の本物のアーキテクチャ上の限界なのかを切り分ける。

結論を先に述べる: **アーティファクトではなく、本物のアーキテクチャ上の限界である。** VMemKVもRocksDB系も、`drop_caches`+store毎の独立cgroupスコープという同一の公平な条件下で、両方とも確かに本物のディスクI/Oを経験している(RocksDB側の`major-faults=0`という2026-08-05当時のアーティファクト兆候は再発していない)。それでもVMemKVが遅いのは、**64KBの値をmmap経由でランダムアクセスすると、ページフォルト単位(4KB)でしか読み込まれず、1レコードあたり最大16回の個別ディスクI/Oに分割されてしまう**のに対し、RocksDBは1レコードを1回の`pread`系syscallでまとめて読めるためである。

## 環境・手法

- インスタンス: AWS スポット、i4i.2xlarge、ap-northeast-1a(`20260805`調査のi4i.8xlargeよりコストを抑えたスコープ限定調査用インスタンス)
- 対象: `VMemKV/Variant=Baseline` vs `RocksDB` vs `RocksDB-BlobDB`(`Op=Get/Mode=Hit`, `Dist=Zipf|Uniform`, `Value=64KB(20% 8B)`, threads:1)
- LTM cgroup: `systemd-run --scope -p MemoryHigh=1GiB -p MemoryMax=2GiB -p MemorySwapMax=1TiB`、`VMEMKV_BENCH_TARGET_RATIO=8.0`(コーパスは131,040キー、真の8倍オーバーサブスクリプション)
- 本番の`run_bench_aws_c6id.sh`と同一の対策込み手法: 各ストアを**個別の**`systemd-run`スコープで測定し、直前に`sync && drop_caches`。RocksDB系のクローンはハードリンクベースの`Checkpoint::CreateCheckpoint`だが、cgroup突入直前のdrop_cachesにより、プライミングパスが温めたページキャッシュの持ち越しは遮断されている。
- 計測ツール: `perf stat`(task-clock, context-switches, page-faults, major-faults, minor-faults)、`/proc/diskstats`前後差分(実際の読み込みセクタ数・読み込み時間)
- ビルド: `-O3 -DNDEBUG -march=native -fno-omit-frame-pointer`

## 結果1: 両ストアとも本物のディスクI/Oを経験している(アーティファクトなし)

| | VMemKV-Baseline | RocksDB | RocksDB-BlobDB |
|---|---:|---:|---:|
| major-faults | **323,981** | 137 | 134 |
| CPU使用率 | **0.182** | 0.516 | 0.637 |
| context-switches | 324,502 | 63,995 | 49,727 |
| 実ディスク読み込み回数 | 323,929 | 62,323 | 48,451 |
| 実ディスク読み込みバイト数 | 1.24 GiB | **3.93 GiB** | 3.03 GiB |
| ディスク読み込みに費やした時間 | 31.73s (elapsed比78%) | 11.13s (elapsed比47%) | 8.61s (elapsed比35%) |

2026-08-05のRocksDB-BlobDB調査で見つかった「major-faults=0、ディスクI/O=0」というアーティファクトの兆候は**再発していない**——RocksDB/RocksDB-BlobDBとも`/proc/diskstats`ベースで数GB規模の実ディスク読み込みが確認できる。`drop_caches`+store別独立cgroupスコープという対策は64KBについても有効に機能している。

むしろ興味深いのは、**RocksDB系の方がVMemKVより多くのバイト数を実際にディスクから読んでいる**(3.93GB vs 1.24GB)にもかかわらず、**所要時間はVMemKVの方が長い**という点である。

## 結果2: 決定的な違いは「1回のI/Oで読むバイト数」

| | VMemKV-Baseline | RocksDB | RocksDB-BlobDB |
|---|---:|---:|---:|
| 総読み込み回数 | 323,929 | 62,323 | 48,451 |
| 総読み込みバイト数 | 1.24 GiB | 3.93 GiB | 3.03 GiB |
| **1回あたりの平均読み込みバイト数** | **4,109 B** | **67,720 B** | **67,042 B** |

VMemKVの1回あたり読み込みサイズは**ちょうど4KB(1ページ)**——ページフォルト1回につき、まさに1ページしか読み込まれておらず、readaheadによるバッチ化が一切効いていないことを示す。対してRocksDB系は1回あたり約66〜68KB——64KBの値をヘッダ込みでほぼ1回のI/Oにまとめて読んでいる。

**64KBの値は約16個の4KBページにまたがる。** VMemKVはこの16ページを、スワップアウトされていれば16回の個別ページフォルト(それぞれ独立した同期I/O待ち+スケジューリングオーバーヘッドを伴う)で読み込むのに対し、RocksDBは1回の`pread`系syscallでまとめて読む。読み込む総バイト数がVMemKVの方が少ない(全体のワーキングセットがRocksDBより小さい、あるいはRocksDB側のLSM/blob形式のオーバーヘッドが上乗せされている可能性がある)にもかかわらず、**I/O回数の多さ(に伴う個別待ち時間・コンテキストスイッチの累積)がVMemKVを遅くしている**。

## 実測スループットとの整合性

| | VMemKV | RocksDB | RocksDB-BlobDB | VMemKV/RocksDB比 |
|---|---:|---:|---:|---:|
| Zipf items/s | 3,227 | 8,912 | 7,039 | 0.36x |
| Uniform items/s | 835 | 4,569 | 4,464 | 0.18x |

`2026080801`(0.15〜0.29x)とほぼ同レンジであり、本調査の1条件のみの計測でも本番結果の劣後傾向を再現できている。

## 結論

LTM/64KB Get/Hitの劣後は、1KBの時のような**計測アーティファクトではない**。VMemKVのT2領域が`mmap`+OS任せのページフォルト駆動アクセスに依存しており、**複数ページにまたがる大きな値1件を読むために、そのページ数だけ個別のディスクI/Oが発生する**という設計上の特性が、値サイズが大きくなるほど不利に働く、本物のアーキテクチャ上の限界である。1KBの値(1〜2ページで収まる)では顕在化しなかったが、64KB(約16ページ)では1レコードあたりのI/O回数の差がそのままスループット差に直結している。

## 今後の対策候補(未検証)

- 大きい値のT2読み取り時に`readv`/`preadv`または`madvise(MADV_WILLNEED)`で、値の全ページを1回のI/O発行でまとめて先読みする経路を、通常のGetパスに追加する(現状Prefaultingはinsert時のみでGet時には適用されない)。
- LTM/1KBのように、値サイズが小さい(1〜2ページ)場合はこの問題が顕在化しないため、対策の要否は値サイズに応じて条件分岐する必要がある。
- 本調査はGet/Hit(単一レコード読み取り)のみを対象とした。YCSB-Eの大きい値・LTMでの劣勢(前回報告)も、レンジScan中の個々のレコード読み取りが同じ経路を通る以上、同根の可能性が高く、この対策が効けば連動して改善すると予想される。

## 生データ

- `implementation/benchmark/logs/ltm_64kb_get_hit_profile/` — `perf stat`出力(3ストア分)、ベンチマークJSON出力(3ストア分)、`/proc/diskstats`前後差分(3ストア分)
