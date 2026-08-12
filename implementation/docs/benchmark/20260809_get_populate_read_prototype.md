# 2026-08-09 GetPopulateRead プロトタイプ計測(否定的結果)

## 背景

`20260809_ltm_64kb_get_hit_profiling.md`で、VMemKVのLTM/64KB Get/Hitが遅い原因は「1レコード(64KB、約16ページ)を読むのに、ページフォルト単位(4KB)で最大16回の個別ディスクI/Oが発生する」ことだと判明した。その対策候補として、値の全ページ範囲を`madvise(MADV_POPULATE_READ)`で一括フォルトさせる(mmapは維持したまま、fd経由の`pread`には迂回しない — in-place updateがMAP_PRIVATEで裏のファイルに反映されないため、fd読み取りは安全でない)プロトタイプを実装し、同一条件で計測した。

結論を先に述べる: **効果なし。** `MADV_POPULATE_READ`はユーザ空間から見た「ページフォルト例外の回数」を16.6倍削減したが、実際のディスクI/O回数・1回あたりの読み込みサイズは変化せず、スループットも誤差範囲でしか変わらなかった。ボトルネックはフォルト例外のハンドリングオーバーヘッドではなく、ディスクI/Oの粒度そのものだった。

## 実装

`vmemkv::GetPopulateRead`という新しいConfigタグ(`include/vmemkv/config.hpp`)を追加し、`get_impl()`のseqlock保護されたコピー直前(`vmemkv_impl.hpp`)で、値がpage size(4096B)を超える場合にのみ、page境界に揃えた範囲へ`madvise(ptr, len, MADV_POPULATE_READ)`を1回発行してからmemcpyする経路を`if constexpr`で追加した。`VMemKV_ScanBaseSequential`と同じ「単独の非累積アブレーションvariant」として`VMemKV_GetPopulateRead`を定義し、`AllPossibleTypes`には含めていない。

## 計測環境

- AWS スポット、i4i.2xlarge、ap-northeast-1a
- 対象: `VMemKV/Variant=Baseline` vs `VMemKV/Variant=GetPopulateRead` vs `RocksDB`(`Op=Get/Mode=Hit`, `Dist=Zipf|Uniform`, `Value=64KB(20% 8B)`, threads:1)
- 手法は`20260809_ltm_64kb_get_hit_profiling.md`と同一(`drop_caches` + store毎の独立cgroupスコープ、`perf stat`、`/proc/diskstats`前後差分)

## 結果

| | VMemKV-Baseline | VMemKV-GetPopulateRead | RocksDB |
|---|---:|---:|---:|
| major-faults | 311,864 | **18,819**(16.6倍減) | 130 |
| 実ディスク読み込み回数 | 311,826 | 309,912(ほぼ同じ) | 61,697 |
| 1回あたり平均読み込みバイト数 | 4,109 B | **4,109 B(変化なし)** | 67,683 B |
| ディスク読み込みに費やした時間 | 32.68s | 32.91s | 11.46s |
| Get/Hit Zipf items/s | 3,004.1 | 2,987.0 | 8,882.7 |
| Get/Hit Uniform items/s | 789.4 | 788.9 | 4,529.7 |

`major-faults`は劇的に減った(ユーザ空間へのフォルト例外という意味では確かに16個のフォルトが1回のmadvise呼び出しに集約されている)にもかかわらず、**`/proc/diskstats`ベースの実読み込み回数・1回あたりサイズはBaselineと事実上同一**(4,109B/回のまま)。つまりカーネルは`MADV_POPULATE_READ`呼び出し内部でも、結局ページ単位の個別ディスク読み込み(おそらく`swap_readahead()`のデフォルト挙動)をそのまま16回発行しているだけで、ディスクI/O自体をまとめてはいない。スループットも誤差範囲(Zipf: 3004→2987、Uniform: 789→789)で、実質的に変化なし。

## 結論と示唆

**ボトルネックはページフォルト例外のトラップ・コンテキストスイッチのオーバーヘッドではなく、swap-in I/O自体が4KB単位でしか発行されていないこと**だと確定した。ユーザ空間側でフォルト回数を減らすアプローチ(`MADV_POPULATE_READ`、あるいは同種の一括フォルトAPI)では、カーネル内部のI/O発行粒度そのものを変えない限り効果が出ない。

## 今後の対策候補(未検証)

- ~~`/sys/kernel/mm/swap/vma_ra_max_order`(swap readahead窓)をチューニングする案~~ — 対象カーネル(6.17.0-1019-aws)にはこのsysfsノード自体が存在しない(`vma_ra_enabled=true`のみ存在)ため却下。代わりに、T2の主mmapに既に適用されている`MADV_RANDOM`(`vmemkv_impl.hpp`の`VMemKVImpl`コンストラクタと`mmap_t2_memory()`の2箇所)が`VM_RAND_READ`としてfile/swap readaheadを`vm.page-cluster`の値に関わらず無効化している可能性が高いと判明したため、`MADV_RANDOM`を外す(または既定のreadaheadポリシーへ戻す)実験に切り替えた。結果は`20260810_t2_no_madvise_random.md`参照。
- mmapを完全に迂回せず、かつMAP_PRIVATEのin-place update問題を避ける形で、値の物理レイアウトを「一括で読める」ようにする(例: 大きい値だけ専用の領域に配置し、その領域はin-place更新を許可しない、といった設計変更)。
- 素朴にfd経由の`pread`へ切り替える案は、in-place updateとの整合性が取れないため引き続き却下(`20260809_ltm_64kb_get_hit_profiling.md`参照)。

## 生データ

- `implementation/benchmark/logs/ltm_64kb_get_populate_read_prototype/` — `perf stat`出力・ベンチマークJSON・`/proc/diskstats`前後差分(Baseline/GetPopulateRead/RocksDBの3系統)
