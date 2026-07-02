# VMemKV 参考文献・評価ワークロード整理表

この表は、`paper/` の草稿・評価計画と `must_read_papers/` の PDF をもとに、各文献が VMemKV の評価設計に何を要求するかを整理したものです。

目的は、単に related work を並べることではなく、次の判断を明確にすることです。

- RocksDB と比較するとき、YCSB だけで十分か。
- WiscKey / Bitcask / BlobDB / LMDB / vmcache などを、実装して競合評価すべきか。
- mmap 批判・buffer manager 系の論文に対して、VMemKV の評価で何を測れば反論または位置づけになるか。

## 先に結論

**YCSB は必要だが、それだけでは不足です。**
RocksDB と勝負するための最低ラインとして YCSB load, A, B, C, E は本編に入れるべきです。F は read-modify-write / in-place update の補助として余力があれば本編、D は appendix でよいです。ただし VMemKV の主張は「OS 委譲型 larger-than-memory value residency」なので、YCSB だけでは mmap / page fault / eviction / TLB / write amplification の thesis を測れません。E1 の larger-than-memory 時系列実験、E5 の mmap vs pread isolate、E4 の reorganization 実験が必要です。

**RocksDB は主競合、RocksDB BlobDB は large value では必須です。**
16 KiB 以上の value で vanilla RocksDB だけに勝っても、WiscKey の主張を再確認しただけに見えます。large value では BlobDB など value-separated RocksDB variant を入れるべきです。

**vmcache / LeanStore / Predictive Translation と直接「勝負」する必要は薄いです。**
これらは standalone KVS ではなく buffer manager / storage engine の系譜です。VMemKV が「buffer manager より速い」と主張しない限り、直接ベンチマーク対象にするより、mmap/pread twin と eviction-onset 実験で同じ懸念を測る方が筋がよいです。本文では性能優位を主張せず、「VMemKV は eviction policy と frame table を持たない代わりに責務を減らす」という軸に置くのが安全です。

**Bitcask / WiscKey は実装競合というより、設計上の最重要ライバルです。**
実装を持ってきて公平に測るより、VMemKV と同じ T1 を使った `pread` twin、または Bitcask-like な mmap/pread microbaseline を作る方が査読者の疑問に直接答えられます。WiscKey 実装比較は任意ですが、WiscKey/BlobDB への設計差分は related work と responsibility table で必ず正面から扱うべきです。

**LMDB は少なくとも 1 つの main-text 図に入れる価値があります。**
VMemKV が mmap-based KVS として見られる以上、LMDB を appendix だけに回すと弱いです。ただし LMDB は B+tree / single-writer / mmap DB なので、主競合というより mmap incumbent としての診断 baseline です。

## 評価セットの推奨最小構成

| 評価目的 | Workload | 比較対象 | 必須 metrics |
|---|---|---|---|
| RocksDB に対する外部競争力 | YCSB load, A, B, C, E。F は余力、D は appendix。value は 1 KiB と 16 KiB。 | RocksDB, RocksDB BlobDB at 16 KiB, LMDB diagnostic | throughput, p50/p95/p99/p99.9, storage usage, logical/device bytes written, WA |
| OS 委譲型 larger-than-memory が成立する条件 | dataset/memory ratio: in-memory, 4x。uniform と Zipf。thread: 1, 4, 8, physical cores。cold-reload protocol を明記。 | VMemKV, same-T1 `pread` twin, mmap-only baseline | throughput, tail latency, major/minor faults, TLB shootdowns, SSD BW, eviction-onset time series |
| T1/T2 split の効果 | point lookup, scan, negative lookup, update-heavy。large value でも測る。 | VMemKV variants: AppendMap, Bloom, SimdScan, MemoryHints, Inline64 | throughput, latency, T1 memory footprint, T2 access count |
| Reorganization の効果 | update/delete 後の scan before/during/after、fixed-size update と bounded-growth update。 | VMemKV before/after reorg、RocksDB compaction stats は参考 | unreachable bytes, copied/reclaimed bytes, foreground p99/p99.9, storage usage |
| 実装責務の比較 | benchmark ではなく表 | VMemKV, RocksDB/LSM, WiscKey/BlobDB, Bitcask, LMDB, explicit buffer manager | buffer pool, replacement, compaction, value-log GC, recovery, scan support |

## 文献別マトリクス

凡例:

- Workload 必要度: **必須** / **推奨** / **任意** / **不要**
- 競合評価: **主競合** / **診断 baseline** / **設計ライバル** / **関連研究のみ**

| 文献 | 概要 / VMemKV との関係 | 論文中の評価 workload | 論文中の比較対象 | VMemKV でその workload をやるか | その手法をライバルとして評価するか | VMemKV 向け判断 |
|---|---|---|---|---|---|---|
| [Bigtable](../must_read_papers/bigtable.pdf) | 分散 LSM 系 storage の古典。RocksDB/LevelDB 系の背景。 | 1000 B values の random/sequential read/write、scan、random reads from memory。tablet server 数を 1, 50, 250, 500 に変えた scalability。 | 自身の構成違いとスケールアウト。外部 KVS との直接比較ではない。 | **推奨**。random/sequential read/write と scan の分類は使う。ただし分散 tablet-server scalability は VMemKV の scope 外。 | **関連研究のみ**。Bigtable 自体とは勝負しない。 | LSM/scan/read amplification の背景に使う。VMemKV 本編では single-node KVS として RocksDB/BlobDB に落とす。 |
| [RocksDB experience](../must_read_papers/rocksdb.pdf) | production LSM KVS の主競合。設定・compaction・resource management の複雑さを示す。 | retrospective が中心。用途別に mixed DB、write-heavy stream/logging/cache、read-heavy index service。DB-bench timestamp API 例あり。 | 基本は RocksDB 内部機能の比較。 | **必須**。YCSB だけでなく、write-heavy / read-heavy / iterator(scan) / cache-like point lookup を含める。 | **主競合**。RocksDB は必ず測る。 | YCSB A/B/C/E/F + load を最低限にし、memory/durability/compression/WAL/direct I/O を公平に設定する。既存の T1-only 8-byte benchmark は VMemKV vs RocksDB 証拠にしない。 |
| LevelDB | WiscKey と Bigtable/RocksDB 系の前身。草稿で LSM baseline として参照。 | WiscKey では `db_bench`: sequential-load, random-load, random lookup, range query。 | WiscKey vs LevelDB。 | **任意**。RocksDB を主比較にすれば LevelDB は補助でよい。 | **関連研究のみ**。 | 本文では LSM の説明に使う。実験対象は RocksDB に集約してよい。 |
| RocksDB BlobDB | value-separated RocksDB。VMemKV/WiscKey に近い large-value baseline。 | paper-only 参照。Blob を LSM 外に置く large-value 設定。 | RocksDB 内 variant。 | **必須** at 16 KiB values。 | **主競合** for large values。 | 16 KiB value で vanilla RocksDB だけと比べるのは不公平。BlobDB を入れないと「WiscKey を再発明しただけ」に見えやすい。 |
| [WiscKey](../must_read_papers/wisckey.pdf) | LSM に key/pointer、別 vLog に value を置く。VMemKV に最も近い key-value separation 系。 | `db_bench`: sequential/random load, random lookup, range query, GC, crash consistency, recovery, space amplification, CPU usage。YCSB load と A-F、1 KiB/16 KiB values。 | LevelDB, RocksDB, WiscKey with/without GC。 | **必須**。YCSB A-F のうち A/B/C/E/F、load、large value、range scan、GC/reorg impact を入れる。 | **設計ライバル**。実装を直接測るのは任意。BlobDB / pread twin で代替可。 | VMemKV の差分は「mmap/pointer dereference」「T1 scan control」「bounded in-place update」「generation/reorg 方針」。WiscKey と同じ workload を避けると弱い。 |
| [Bitcask](../must_read_papers/bitcask.pdf) | in-memory keydir + append-only data file。OS filesystem cache に read performance を委譲。VMemKV の anticipation threat。 | formal benchmark は薄い。random write stream、>10x RAM dataset、merge、startup/recovery、sub-ms median latency という経験的記述。 | API similar local storage systems と述べるが詳細表はない。 | **推奨**。append-heavy insert、point lookup、larger-than-memory、merge/reorg、keydir memory footprint を測る。 | **設計ライバル**。full Bitcask 実装より Bitcask-like pread/mmap baseline が有効。 | 査読者の「Bitcask に sorted T1 と mmap を足しただけでは？」に答える必要がある。T1 memory footprint と scan/reorg 差分を表にする。 |
| [Are You Sure You Want to Use MMAP?](../must_read_papers/andy_mmap.pdf) | mmap 批判の中心文献。VMemKV の最大の反論対象。 | read-only best-case。2 TB range random reads、sequential scan。1 SSD / 10 SSD。`fio O_DIRECT` vs mmap with `MADV_*`。TLB shootdowns と eviction-onset を測定。 | `fio O_DIRECT pread/libaio`、mmap `MADV_NORMAL/RANDOM/SEQUENTIAL`。 | **必須**。E1/E5 で random read、sequential scan、eviction-onset time series、TLB shootdowns を測る。 | **診断 baseline**。論文手法とは勝負しない。 | VMemKV はこの結果を否定せず、value-granularity / T1 hot metadata / current kernel / storage latency の条件でどこまで成立するかを測る。 |
| LMDB | mmap-based KVS の代表。草稿で mmap incumbent として参照。 | `must_read_papers` に単独 PDF はないが、vmcache / Predictive Translation で random lookup、TPC-C、out-of-memory 比較に登場。 | LeanStore, vmcache, WiredTiger, PrediCache など。 | **推奨**。少なくとも VMemKV の YCSB-lite または point/scan 図に入れる。 | **診断 baseline**。 | mmap 系を名乗るなら appendix だけでは弱い。single-writer/B+tree なので主競合ではないが、mmap incumbent として main-text に 1 図あるとよい。 |
| [vmcache](../must_read_papers/vmcache.pdf) | VM を buffer manager の translation に使うが、fault/eviction は DBMS が制御する。VMemKV とは逆の trade-off。 | TPC-C、uniform random point lookup、in-memory/out-of-memory、page-access microbenchmark、allocation benchmark、pread/io_uring random 4 KiB reads。 | LeanStore, WiredTiger, LMDB, vmcache, vmcache+exmap。 | **推奨**。TPC-C は不要だが、random point lookup、out-of-memory transition、pread/io_uring isolate の考え方は採用。 | **関連研究のみ**、または **診断 baseline**。直接勝負は不要。 | vmcache と性能競争しない。VMemKV は eviction decisions/frame table を持たない設計として responsibility table で対比する。 |
| [LeanStore 2024](../must_read_papers/leanstore.pdf) | NVMe SSD 向け high-performance storage engine。buffer pool, page replacement, logging, recovery を高効率に実装。 | この PDF は overview 色が強い。TPC-C NewOrder の instruction breakdown と既存 LeanStore 系結果への参照。 | Shore などとの概念比較。 | **不要**。VMemKV は transaction/storage engine ではない。 | **関連研究のみ**。 | 「buffer manager は遅い」と書かないための重要文献。VMemKV の売りは性能優位ではなく、実装責務削減に置く。 |
| [Predictive Translation](../must_read_papers/predictive-translation.pdf) | hash-table buffer pool を高性能化し、vmcache/LeanStore/LMDB と比較する buffer manager 論文。 | TPC-C、uniform random lookup、skewed YCSB、in-memory/out-of-memory、in-to-out transition、ablation、microarchitectural counters。 | LeanStore, vmcache+exmap, WiredTiger, LMDB, traditional buffer pool。 | **推奨**。skewed YCSB と out-of-memory transition は VMemKV E1 の設計に反映。TPC-C は scope 外。 | **関連研究のみ**。 | user-space buffer manager 系は強い。VMemKV は「より速い」ではなく「value layer だけ OS に委譲し、責務を減らす」と限定する。 |
| [ScaleCache](../must_read_papers/scalecacle.pdf) | production DBMS の scalable buffer management。many-core の buffer pin/translation bottleneck を扱う。 | sysbench multi-random point lookup、TPC-C mix、TPC-H、DiskANN vector search、out-of-memory、customer workload、CPU scaling。 | GaussDB baseline、MySQL/PostgreSQL 一部。 | **任意**。VMemKV の many-core point lookup scaling の参考にはなるが、workload は DBMS 寄り。 | **関連研究のみ**。 | VMemKV が buffer manager 論文ではないことを明確化する。many-core scaling を主張するなら thread scaling と contention metrics は必要。 |
| [FASTER](../must_read_papers/faster.pdf) | concurrent KV with HybridLog and in-place updates。VMemKV の in-place update 主張と衝突しやすい。 | extended YCSB-A variants: read/blind update/RMW、uniform/Zipf/hot-set、in-memory、larger-than-memory by memory budget、append-only vs HybridLog、cache simulation。 | TBB hash map, Masstree, RocksDB, Redis。 | **推奨**。update-heavy/RMW と memory-budget sweep は in-place update を語るなら必要。 | **任意の競合**。主比較ではない。 | VMemKV の in-place update を中心 contribution にしない。FASTER は epoch/recovery 付きで強すぎるので、差分は file-backed OS residency に置く。 |
| [F2](../must_read_papers/f2.pdf) | FASTER v2。large skewed workload 向け two-tier log、read-cache、cold-log index。VMemKV の target workload と近い。 | YCSB A/B/C/F/D/W、MixGraph、Zipf skewness sweep、memory budget 2.5-25%、thread scaling、read/write amplification。 | FASTER, RocksDB, SplinterDB, KVell, LeanStore。 | **推奨**。skewness sweep と memory budget sweep は VMemKV の E1/E2 に有用。MixGraph は任意。 | **任意の競合**。実装成熟後。 | VMemKV が large skewed workload を強く主張するなら F2 を無視できない。ただし今は related work + workload design への反映で十分。 |
| [How to Write to SSDs](../must_read_papers/how_to_write_ssds.pdf) | DBMS write pattern と SSD WAF を扱う。VMemKV の in-place/update/reorg/write amplification 評価に関係。 | YCSB-A Zipf 0.8、TPC-C、dataset size sweep、buffer pool 5-20%、SSD fill factor、CNS/ZNS/FDP、複数 SSD。 | in-place LeanStore, out-of-place/ZLeanStore, MySQL, PostgreSQL。 | **推奨**。YCSB-A update-heavy、dataset size/fill factor、DB/device write bytes は入れる。 | **関連研究のみ**。 | VMemKV でも logical bytes, WAL bytes, device bytes, storage usage を測る。SSD WAF まで出せない場合は limitation にする。 |
| [LLFREE](../must_read_papers/LLFREE.pdf) | Linux page-frame allocator 改善。page fault / allocation path の kernel overhead を理解する材料。 | synthetic bulk/random/repeat alloc/free、fragmentation、NVRAM crash recovery、memtier、mmap populate + `MADV_DONTNEED` write benchmark。 | Linux buddy allocator。 | **任意**。VMemKV の main workload には不要。ただし page fault path 分析の補助に使える。 | **関連研究のみ**。 | 「新しい kernel なら mmap が改善しうる」側の材料。ただし VMemKV で直接比較する必要はない。 |
| [Resource-Adaptive Query Execution with Paged Memory Management / LIPAH](../must_read_papers/lipah_otaki.pdf) | buffer pool pages を query execution memory に使う。LIPAH で page-to-frame mapping overhead を減らす。 | TPC-H SF1 paged vs non-paged、hash index insert/lookup with/without LIPAH。 | 同一 prototype 内の feature on/off。 | **不要**。VMemKV の KVS evaluation とは離れている。 | **関連研究のみ**。 | Related work で一文程度。T1/T2 の「logical ID + physical hint」議論に使えるが、競合評価不要。 |
| YCSB | KVS macrobenchmark。VMemKV/RocksDB/WiscKey/F2/FASTER の共通語彙。 | A: 50/50 read/update、B: read-heavy、C: read-only、D: read-latest + insert、E: short scan、F: read-modify-write。 | benchmark framework。 | **必須**。A/B/C/E は本編、F は推奨、D は appendix。 | 競合ではない。 | 「YCSBだけ」は不足だが、YCSBなしは KVS 論文として弱い。load phase と value size を明記する。 |

## 比較対象の優先順位

| 優先度 | 比較対象 | 理由 |
|---|---|---|
| P0 | RocksDB | production LSM の主競合。査読者が期待する。 |
| P0 | RocksDB BlobDB at 16 KiB | large value の公平性。VMemKV/WiscKey 系に近い。 |
| P0 | same-T1 `pread` twin | mmap の効果を T1/T2 構造から切り分ける決定的 ablation。 |
| P1 | mmap-only + pread microbaseline | raw mmap value access の上限/下限を見る診断 baseline。 |
| P1 | LMDB | mmap incumbent。主競合ではないが main-text にあると related work が締まる。 |
| P1 | Bitcask-like simple log/index baseline | Bitcask anticipation threat に答える。full Bitcask でなくてもよい。 |
| P2 | FASTER/F2 | in-place update / large skewed workload を強く主張する場合のみ。 |
| P3 | vmcache/LeanStore/Predictive/ScaleCache | 直接勝負ではなく、mmap/buffer manager 批判の定量的 context。 |

## 実験計画への反映メモ

1. **YCSB vs RocksDB は E2 として必須**  
   A/B/C/E を本編に残し、F は in-place update を語るなら入れる。D は appendix。1 KiB と 16 KiB values を使う。16 KiB では BlobDB を入れる。

2. **E1 は YCSB とは別に必要**  
   Crotty/vmcache 系の批判に答えるには、larger-than-memory の eviction-onset を時系列で測る必要がある。page faults, TLB shootdowns, SSD bandwidth, p99/p99.9 を入れる。

3. **E5 に pread variant を追加する**  
   mmap-only baseline だけでは足りない。同じ T1 で value access だけ `mmap` pointer dereference と `pread` に分ける twin が最重要。

4. **vmcache とは性能勝負しない**  
   vmcache は「DBMS が VM を使いながら eviction を制御する」手法で、VMemKV は「value residency を OS に委譲する」手法。比較軸は性能優位ではなく責務の違い。

5. **Bitcask/WiscKey は related work で弱点を隠さない**  
   VMemKV の新規性は key-value separation そのものではない。残る差分は、T1/T2 control/data split、mmap/pread の違い、bounded in-place update、reorganization/checkpoint 方針、T1 の compact key metadata。

6. **現状実装との gap は表で正直に分ける**  
   WAL/checkpoint/recovery/BlobDB/YCSB/fairness instrumentation は現状未整備。評価計画では「本編必須だが未実装」と「今すぐ測れる T1/T2 ablation」を分ける。
