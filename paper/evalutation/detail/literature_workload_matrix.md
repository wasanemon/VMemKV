# Literature and Workload Matrix for VMemKV Evaluation

---

thesisに関しては、wasanemonが、nikezonoさんによる設計の変更、未実装のプラン(TODO系)を踏まえて作成していないので注意

---

## 1. Review of Current evaluation_plan.md

### 強い点

- E0 の fairness 項目（cgroup、dirty page 設定、NUMA pin、durability scope の明記）は関連研究水準を超えて具体的で良い。
- §9「Negative results の扱い」を事前コミットしている点は characterization 論文として正しい姿勢。
- mmap-only microbaseline を「durability-matched competitor ではない診断用」と正しく限定している（E5）。
- BlobDB を 16 KiB で必須とする判断（E2）は WiscKey Fig 21 / BlobDB の系譜に照らして正しい。
- engine WA と device WA を分離する定義（E2）は How to Write to SSDs の DB WAF / SSD WAF 区別（Sec 2, Table 1）と整合する。



### 足りない workload


| 欠落                                                          | 動機となる文献                                                                                             | 重要度                                                     |
| ----------------------------------------------------------- | --------------------------------------------------------------------------------------------------- | ------------------------------------------------------- |
| **pread twin**（同一 T1、value access のみ pread(+posix_fadvise)） | Crotty（fio O_DIRECT 対照, Sec 4）、Bitcask（pread+page cache で同じ委譲を実現, p.2–3）、WiscKey（explicit I/O 側の実装） | **最重要**。これが無いと mmap の寄与が全アーキテクチャ差分と交絡する                 |
| 小さい value（64–128 B）の point / scan                           | WiscKey Fig 17（64 B scan で LevelDB に 12x 劣後）、FASTER/F2/vmcache（8 B key / 100–120 B value が標準）       | 高。1 KiB 開始では KV 分離が負ける領域を隠したと見られる                       |
| dataset/memory ≥ 10x の点、または memory-budget sweep（2.5–25%）    | F2 Sec 8.5 Fig 13（2.5–25% sweep）、Bitcask p.5（>10x RAM）、How to Write（5–20x）                          | 高。4x/8x は「易しい側」しか測っていない。T1 フットプリントで不可能ならそれ自体を「破綻点」として報告 |
| eviction-onset の明示的注記と 60 s+ 時系列                            | Crotty Fig 2a（~27 s 後に 5 s 近くゼロ落ち→半減で安定）                                                            | 高。E1 は時系列を持つが、3 相（充填/cliff/定常）を捉える run 長と注記の規定がない       |
| fio O_DIRECT による device bound 線の併記                          | Crotty Sec 4、vmcache Fig 7 脚注                                                                       | 高。「device 帯域の X%」で語れないと F2 世代の読者に通じない                   |
| load→reorganize→drop_caches→測定 という **regime protocol** の明文化 | MAP_PRIVATE の帰結（載荷直後は全量 anonymous dirty）                                                            | **必須**。これが無いと E1 は「file page cache」ではなく「swap」を測る実験になる   |
| in-place update vs always-append の ablation                 | FASTER Fig 11（AOL vs HybridLog）、How to Write（in-place の SSD WAF 2–2.7x, Fig 14）                     | 中。in-place update の価値の唯一の直接証拠になる                        |
| 定常 overwrite での space overhead                              | RocksDB Table 4（Dynamic Leveled で定常 ~13%）                                                           | 中。production の第一制約は space（RocksDB Sec 3.2, Fig 3）       |
| hot-set drift（移動する hot set）                                 | FASTER Sec 7.1/7.5                                                                                  | 中（appendix 可）。OS paging が動く working set に追従するかの最鋭利なテスト  |
| T1 memory footprint 表（bytes/key、T2 page table ~8 B/4 KiB）   | F2 Sec 3.2/6、vmcache Sec 3.6（16 B/4 KiB の会計例）                                                       | 高。ベンチではなく表 1 枚。「T1 は RAM に収まる」仮定の数値化                    |
| kswapd CPU / kernel flame graph                             | vmcache Fig 4、Crotty Sec 4.1（kswapd single-threaded）                                                | 中。TLB カウントだけでは帰属が弱い                                     |




### 足りない baseline

- **pread twin**（上記）— E5 に pread variant を追加するのが最小実装。
- **LMDB の main-text 昇格** — 現在 appendix 扱い（§4 優先度表）だが、mmap KVS を名乗る論文で mmap incumbent が appendix なのは弱い。vmcache/PrediCache の両方で LMDB は標準 baseline であり、PrediCache Fig 6 は LMDB の out-of-memory cliff を実測している。最低 1 図は main text に。
- fio O_DIRECT bound（baseline というより参照線）。



### 過剰または費用対効果が低い実験

- **E4 の checkpoint 半分（fork time / CoW faults / parent・child RSS / stop-the-world）**: TODO.md §2 が fork 廃止（no-fork の in-process serialization 化）を宣言しており、廃止予定機構の測定計画になっている。fork を「残す」なら HyPer/Redis BGSAVE を先行研究として引いた上で世代交代 re-mmap を機構貢献として主張すべきで、廃止するなら E4b の指標を serialization time / publish latency に差し替えるべき。**どちらかの決定が先**。
- **E6 全体**: WAL/checkpoint/reopen が 0% 実装で、現状「測定対象が存在しない」。WAL+checkpoint+reopen が実装された場合のみ sanity check として残す（gate 条件を plan に明記）。
- logical cores のスレッド条件（E1/E2）: physical cores までで十分。本質は kswapd/mmap_lock 律速の露出であり、SMT 差分は本筋でない。
- TLB shootdown の VM 環境での測定: /proc/interrupts ベースは仮想化層で信頼性が落ちる。bare metal が確保できない場合は limitation として明記（先行査読 §4 と同意見）。
- YCSB D は appendix のままで良い（現行判断を支持）。



### 本編に必須の実験（Minimum Viable Evaluation; 先行査読 §4 の MVE と整合）

E0（+ regime protocol + 統計方法）/ E1 縮約版（in-mem, 4x; 1 KiB & 16 KiB; uniform+Zipf; 時系列 + faults + TLB + fio bound）/ E2-lite（YCSB load+A/B/C/E, F は安価なら; RocksDB + BlobDB@16 KiB; **both-volatile を主構成として正直に明記**し、durable-RocksDB 列を併記）/ E3（そのまま、ただし large value に拡張、SimdScan は配線修正後）/ E4a（reorganization のみ）/ E5（mmap 版 + pread 版）/ E7（3 値列の責務表）。

### appendix に回せる実験

YCSB D、full value-size sweep（64 B–256 KB; ただし 64–128 B の 1 点は本編推奨）、hot-set drift、latency CDF（本編は p50–p99.9 + 時系列で足りる場合）、LevelDB、FASTER/F2 直接比較、8x 比（4x を本編、8x/10x を appendix に置く構成も可）、multi-SSD scan（ハード次第）。

### 実装担当者に依頼すべき benchmark / instrumentation

詳細な実装タスク節はこの版では削除済み。

### 査読者に突っ込まれそうな点（攻撃シナリオ順）

1. 「CIDR'22 を読んだか。eviction 開始後 2–20x 劣後・TLB storm・kswapd 律速をどう回避するのか」→ E1 の 3 相時系列 + fio bound + Related Work の定量的応答で正面回答。
2. 「これは Bitcask + sorted array + mmap では」→ pread twin と T1 footprint 表（Bitcask は full key を RAM に持つ、T1 は 16 B prefix + 60-bit hash）、in-place update、世代交代 reorganize を差分として明示。
3. 「WiscKey が 2016 年に log reorganization（sorting）を提案済み（TOS'17 p.5:20）。VMemKV の T2 reorganize はその実装では」→ 認めた上で「WiscKey が open question にした実測」を Fig 17 3 状態実験で埋める、と反転させる。
4. 「なぜ FASTER/F2 と比較しない。RocksDB は point ops では ~500K ops/s の弱い baseline（FASTER Sec 7.3）」→ scan/ordered API を持つ engine 同士の比較であることを scope として明示し、F2 の数値を related work で正面から引用。
5. 「T1 の 32 B/key × mlock は F2 が捨てた設計。10 億 key でどうする」→ footprint 表 + 適用範囲（key 数上限）の明示。
6. 「matched durability と言うが VMemKV に durability は無い」→ both-volatile 構成を主とし、RocksDB durable 列を併記、durability は design + future work と正直に書く。
7. 「BlobDB なしで 16 KiB を勝っても WiscKey の再証明」→ BlobDB 必須（既に plan にあり、実装が必要）。
8. 「ScaleCache は mmap 系の page table メモリ肥大を production 棄却理由に挙げる（Sec 2.2）。測ったか」→ footprint 表に page table 項目を含める。



### 既存 evaluation_plan.md に将来反映すべき変更点

1. 中心問いを弱い thesis に差し替える。
2. E0 に「regime protocol」（load→reorganize→drop_caches→measure、pre-reorganize regime は write-path case として別報告）と統計方法（≥5 reps, mean±sd）を追加。
3. E5 に pread twin variant を追加し、名称を「mmap/pread microbaseline pair」に変更。T2 と同じ mapping mode で構築することを明記。
4. E4 を E4a（reorganization、近期実行可能）と E4b（checkpoint、実装 gate 付き）に分割。fork 指標は fork 継続決定時のみ。
5. E6 に gate 条件（WAL + snapshot checkpoint + reopen 実装済み）を明記。未達なら cut して durability は design/future work に移す。
6. E2 に「both-volatile を主構成、durable-RocksDB を参考列」と明記。RocksDB 設定リストに BlobDB threshold, `min_blob_size`, blob GC 設定を追加。
7. E1 に fio O_DIRECT bound、eviction-onset 注記、run 長規定（60 s 以上）、kswapd CPU 計測を追加。1 KiB に加えて 64–128 B の 1 点を追加。
8. 指標に「T1 bytes/key」「T2 page table overhead」「swap in/out（MAP_PRIVATE regime）」「steady-state space overhead %」を追加。
9. 優先度表の LMDB を「中」→「main-text 1 図」に昇格。
10. MVE（最小核）と cut line を明記する節を追加。

---



## 2. Literature-to-Workload Matrix

must_read_papers 全 14 本の評価設計サマリ。詳細な根拠と引用は §5 と CSV を参照。


| 文献 (venue)                         | カテゴリ                      | 論文中の主要 workload                                                                    | 論文中の baseline                                            | 主要 metrics                                                      | value size                           | dataset/memory             | 分布                               | LTM    |
| ---------------------------------- | ------------------------- | ---------------------------------------------------------------------------------- | -------------------------------------------------------- | --------------------------------------------------------------- | ------------------------------------ | -------------------------- | -------------------------------- | ------ |
| WiscKey (TOS'17/FAST'16)           | KV separation LSM         | db_bench seq/random load・lookup・range、GC、crash、YCSB load+A–F (Sec 4.2–4.3)         | LevelDB、RocksDB (default)                                | throughput、WA、latency CDF、space amp、CPU、recovery time           | 64 B–256 KB sweep; YCSB 1 KiB/16 KiB | 100 GB / 64 GB RAM (~1.6x) | uniform / Zipf / latest          | あり     |
| RocksDB (TOS'21/FAST'20)           | LSM 経験論文                  | rate-limited random overwrite、production 調査、db_bench (Tables 3–6)                  | 自身の compaction variant                                   | WA、I/Os per Get/seek、space overhead %、資源利用率                     | 100 B (Table 3)                      | 500M keys、block cache=10%  | uniform overwrite                | 実運用    |
| Are You Sure MMAP (CIDR'22)        | mmap 批判                   | random read (100 thr)、seq scan、1/10 SSD、60 s 時系列 (Sec 4)                           | fio O_DIRECT pread / libaio                              | reads/s、TLB shootdowns/s、GB/s 時系列                               | 4 KiB page 粒度                        | 2 TB / 100 GB cache (20x)  | uniform / sequential のみ          | 中核     |
| vmcache (SIGMOD'23)                | VM 支援 buffer manager      | random lookup、TPC-C、in/out-of-memory、page-access micro、alloc micro (Sec 5)         | LeanStore、WiredTiger、LMDB、hash table                     | tx/s、lookups/s、ns/access、instr、I/O GB/s vs fio bound、kernel CPU | 8 B key / 120 B value                | ~1 TB / 128 GB (8x)        | uniform、TPC-C                    | 中核     |
| LeanStore'24 (PVLDB 17(12))        | buffer manager 総説         | TPC-C instruction 内訳 (Fig 4) のみ                                                    | Shore                                                    | instructions/tx                                                 | Needs verification (記載なし)            | in-memory のみ               | TPC-C                            | 引用のみ   |
| FASTER (SIGMOD'18)                 | hybrid-log KVS            | 拡張 YCSB-A（R:BU:RMW 比率）、memory budget sweep、hot-set drift、cache simulation (Sec 7)  | Masstree、TBB hash、RocksDB、Redis                          | Mops/s、log growth MB/s、miss ratio                               | 8 B / 100 B                          | 27 GB vs budget sweep      | uniform / Zipf 0.99 / hot-set    | あり     |
| F2 (PVLDB 18(12) 2025)             | hybrid-log KVS (LTM skew) | YCSB A/B/C/D/F/W、MixGraph、skew sweep、memory 2.5–25% sweep、compaction micro (Sec 8) | FASTER、RocksDB、SplinterDB、KVell、LeanStore                | Kreqs/s、RA/WA (/proc/pid/io)、latency、thread scaling             | 8 B key / 100 B value                | 30 GiB / 10%（4–40x sweep）  | Zipf θ0.99、α 3–1000 sweep、latest | 中核     |
| Bitcask (Basho TR 2010)            | log+RAM-index KVS         | 形式的ベンチなし（5–6K writes/s、sub-ms median、>10x RAM の記述のみ, p.5）                          | なし（定性比較のみ）                                               | throughput、median latency                                       | not specified                        | >10x RAM                   | not specified                    | 主張のみ   |
| Bigtable (OSDI'06)                 | 分散 LSM 起源                 | seq/random read/write、scan、1–500 servers (Sec 7)                                   | なし                                                       | values/s/server                                                 | 1000 B                               | ~1 GB/server               | uniform                          | なし（分散） |
| How to Write to SSDs (PVLDB'26)    | SSD WA 方法論                | YCSB-A Zipf 0.8（fill 18–90%）、TPC-C 1.6 TB、fio、ZNS/FDP (Sec 7)                      | in-place LeanStore、MySQL、PostgreSQL、自変種                  | DB WAF、SSD WAF、Total WAF、bytes/op、OPS                           | page 粒度 (4–16 KiB)                   | 5–20x                      | Zipf θ0.8                        | あり     |
| Predictive Translation (SIGMOD'26) | buffer manager            | uniform/Zipf θ0.9 lookup、TPC-C、in/out-of-memory、transition、microarch (Sec 6)       | LeanStore、vmcache+exmap、WiredTiger、LMDB、traditional pool | tx/s、lookups/s、counters                                         | 8 B key / 120 B value                | ~1 TB / 128 GB (8x)        | uniform / Zipf θ0.9              | あり     |
| ScaleCache (PVLDB 18(12) 2025)     | production buffer manager | sysbench point lookup、TPC-C、TPC-H、DiskANN、out-of-memory (Sec 4)                    | GaussDB baseline、MySQL、PostgreSQL                        | QPS、tpmC、scaling to 256 cores                                   | 8 KB page                            | 35x / 29x (Sec 4.7)        | 標準 spec                          | あり     |
| LLFree (venue: Needs verification) | kernel page allocator     | bulk/random/repeat alloc、fragmentation、memtier、mmap+MADV_DONTNEED (Sec 5)          | Linux buddy、list allocators                              | Mops/s、fragmentation、fault path 内訳 (Fig 13)                     | 4 KiB–4 MiB frames                   | in-memory                  | synthetic                        | なし     |
| LIPaH/Otaki (CIDR'25)              | paged query execution     | TPC-H SF1 paged vs non-paged、hash index ±LIPAH (Sec 4)                             | 自プロトタイプ内 on/off                                          | 実行時間、lookup 性能                                                  | 100 B key / 50–100 B value           | in-memory                  | uniform                          | なし     |


---



## 3. Workload Coverage Table

判定凡例 — 必須: 本編に必要 / 望ましい: あると強いが appendix 可 / 不要: 本論文では不要。実行可能性は実装リポジトリ現状に基づく。


| Workload / metric                                                    | 判定                                    | 効く比較・反論                                                        | 組む baseline           | 今すぐ実行可能か                      | 実装に必要な追加                                                                                                      |
| -------------------------------------------------------------------- | ------------------------------------- | -------------------------------------------------------------- | --------------------- | ----------------------------- | ------------------------------------------------------------------------------------------------------------- |
| YCSB load phase                                                      | 必須                                    | WiscKey Fig 9–11、Bitcask の sustained ingest                    | RocksDB, BlobDB       | 不可                            | YCSB ドライバ、value-size knob                                                                                     |
| YCSB A (update-heavy)                                                | 必須                                    | WiscKey Fig 21、F2、How to Write                                 | RocksDB, BlobDB       | 不可                            | 同上 + Zipf request 分布                                                                                          |
| YCSB B (read-heavy)                                                  | 必須                                    | WiscKey、F2                                                     | RocksDB               | 不可                            | 同上                                                                                                            |
| YCSB C (read-only)                                                   | 必須                                    | WiscKey、Crotty（read-only は mmap best case）                     | RocksDB, pread twin   | 不可                            | 同上                                                                                                            |
| YCSB D (read-latest)                                                 | 不要（appendix）                          | WiscKey                                                        | RocksDB               | 不可                            | latest 分布                                                                                                     |
| YCSB E (short scan)                                                  | 必須                                    | WiscKey Fig 17/21（KV 分離の弱点）                                    | RocksDB, BlobDB       | 不可                            | byte-value scan API（現 scan は u64 前提 `vmemkv_impl.hpp:378`）                                                    |
| YCSB F (RMW)                                                         | 必須（安価なら本編）                            | FASTER/F2（RMW が彼らの主戦場）                                         | RocksDB               | 不可                            | RMW 経路                                                                                                        |
| update-heavy / read-heavy / read-only 比率変種 (100:0/50:50/0:100)       | 望ましい                                  | FASTER Fig 8                                                   | RocksDB               | 不可                            | mix knob                                                                                                      |
| large value (16 KiB)                                                 | 必須                                    | WiscKey、BlobDB                                                 | RocksDB, BlobDB       | 不可                            | value-size knob（現ベンチ 8 B 固定 `bench_kv.cpp:171`）、T2 容量拡張                                                       |
| small value (64–128 B)                                               | 望ましい（本編 1 点推奨）                        | WiscKey Fig 17 の 12x 劣後、FASTER/F2/vmcache の標準                  | RocksDB               | 不可                            | 同上                                                                                                            |
| larger-than-memory (4x)                                              | 必須                                    | Crotty、vmcache Fig 7、F2                                        | 全 baseline            | **構造的に不可**                    | reopen 経路（`O_EXCL`, `t2_flat_file.cpp:151`）、容量拡張（wedge bug `t2_flat_file.cpp:46-51`）、mapping regime 決定、cgroup |
| larger-than-memory (8x–10x / memory sweep 2.5–25%)                   | 望ましい                                  | F2 Fig 13、Bitcask >10x、How to Write 5–20x                      | 全 baseline            | 不可                            | 同上 + T1 footprint が許す範囲の明示                                                                                    |
| skewed access (Zipf θ0.99 / α1.2)                                    | 必須                                    | YCSB 標準、F2                                                     | 全 baseline            | 部分（Zipf 生成器あり α=1.0 のみ使用）     | α knob 配線                                                                                                     |
| skew sweep                                                           | 望ましい                                  | F2 Fig 12                                                      | VMemKV 単体 + RocksDB   | 不可                            | 同上                                                                                                            |
| uniform random cold read                                             | 必須                                    | Crotty（最悪ケースを正面から）                                             | pread twin, fio bound | 不可                            | LTM 能力一式                                                                                                      |
| negative lookup                                                      | 必須                                    | RocksDB Table 3（Bloom 有無の I/O 差）、自 Bloom ablation              | VMemKV variants       | 可（8 B・in-mem のみ）              | large value / LTM 版                                                                                           |
| delete-heavy                                                         | 必須                                    | RocksDB Sec 6.3.1（queue pattern）、reorganization 検証             | VMemKV, RocksDB       | 部分                            | garbage 計測カウンタ                                                                                                |
| fixed-size update                                                    | 必須                                    | in-place update の唯一の得意領域（alloc_len 厳密一致 `t2_flat_file.cpp:59`） | RocksDB, BlobDB       | 部分（8 B のみ）                    | value-size knob                                                                                               |
| growing-value update                                                 | 必須                                    | in-place の境界の正直な提示（1 B でも成長すれば append）                         | 同上                    | 不可                            | 同上                                                                                                            |
| in-place vs always-append ablation                                   | 望ましい                                  | FASTER Fig 11                                                  | VMemKV variants       | 不可                            | append-only モード追加                                                                                             |
| scan before reorganization                                           | 必須                                    | WiscKey Fig 17（random-loaded）                                  | RocksDB, BlobDB       | 部分（8 B）                       | byte scan + 大 value                                                                                           |
| scan during reorganization                                           | 必須                                    | WiscKey Fig 18 相当、reorganize 干渉                                | VMemKV                | 不可                            | 並行負荷ハーネス + reorganize の race 修正                                                                               |
| scan after reorganization                                            | 必須                                    | WiscKey p.5:20 の open question を埋める                            | 同上                    | 部分                            | 同上                                                                                                            |
| reorganization vs garbage 比率 (0/25/50/75%)                           | 必須                                    | WiscKey Fig 18                                                 | VMemKV                | 不可                            | unreachable/copied/reclaimed bytes カウンタ                                                                       |
| crash recovery                                                       | 実装 gate 付きで採用                         | WiscKey Sec 4.2.4（ALICE + recovery time）                       | VMemKV 単体             | **不可（基盤ゼロ）**                  | WAL、checkpoint、reopen、crash harness                                                                           |
| checkpoint 実験                                                        | 実装 gate 付きで採用                         | HLD 6.2（ただし TODO は no-fork 化）                                  | VMemKV                | 不可                            | checkpoint 実装 + 方式決定                                                                                          |
| WAL replay                                                           | 実装 gate 付きで採用                         | WiscKey recovery、Aether                                        | VMemKV                | 不可                            | WAL 実装                                                                                                        |
| write amplification (DB WAF / SSD WAF 分離)                            | 必須                                    | How to Write Sec 7.1、WiscKey Fig 12、RocksDB Table 3            | RocksDB, BlobDB       | 不可                            | /proc/pid/io or blkstat 計測、SMART/OCP 読出し                                                                      |
| storage amplification / 定常 space overhead                            | 必須                                    | RocksDB Table 4（~13% 基準）、WiscKey Fig 19                        | RocksDB               | 不可                            | bytes_used 系カウンタ + 定常 overwrite ハーネス                                                                          |
| tail latency (p50–p99.9 + 時系列 + CDF)                                 | 必須                                    | Crotty の cliff、WiscKey Fig 13/16、SILK 系の問題意識                   | 全 baseline            | 部分（microbench のみ p50–p999 対応） | bench_kv への percentile 実装                                                                                     |
| page faults (major/minor, /op)                                       | 必須                                    | Crotty、vmcache                                                 | VMemKV, pread twin    | 不可                            | getrusage / /proc/vmstat 計測                                                                                   |
| TLB shootdowns / dTLB misses                                         | 必須（bare metal 前提、VM なら limitation 明記） | Crotty Fig 2b、vmcache Fig 4                                    | VMemKV                | 不可                            | /proc/interrupts + perf                                                                                       |
| CPU cycles/op・instructions/op                                        | 望ましい                                  | vmcache Table 2、LeanStore Fig 4                                | VMemKV variants       | 不可                            | perf counters                                                                                                 |
| kswapd / kernel CPU share (flame graph)                              | 望ましい                                  | vmcache Fig 4（79% TLB）、Crotty（kswapd 律速）                       | VMemKV                | 不可                            | perf 記録                                                                                                       |
| SSD bandwidth 時系列 + fio bound                                        | 必須                                    | vmcache Fig 7、Crotty Fig 2–4                                   | 全 baseline            | 不可                            | iostat 系記録 + fio 手順                                                                                           |
| cgroup memory pressure                                               | 必須                                    | E0 前提。anonymous vs page cache の会計差を明記                          | 全 baseline            | 不可                            | cgroup ハーネス                                                                                                   |
| dirty page writeback（MAP_SHARED regime）/ swap 挙動（MAP_PRIVATE regime） | 必須                                    | Crotty Sec 2.2、TODO §1 の帰結                                     | VMemKV                | 不可                            | regime protocol + vmstat 記録                                                                                   |
| thread scalability (1→physical cores)                                | 必須                                    | vmcache Fig 6、F2 Fig 11、LLFree Fig 13（mmap_lock）               | RocksDB               | 部分（8 B のみ）                    | 大 value / LTM 版                                                                                               |
| T1 memory footprint 表                                                | 必須（表）                                 | F2 Sec 3.2、vmcache Sec 3.6、Bitcask（full key vs 16 B prefix）    | 各設計の bytes/key        | 可（計算）                         | なし（執筆作業）                                                                                                      |
| TPC-C                                                                | 不要                                    | vmcache/LeanStore/PrediCache は DBMS 用途                         | —                     | —                             | —                                                                                                             |
| production trace (MixGraph)                                          | 不要（future work）                       | F2                                                             | —                     | —                             | —                                                                                                             |
| 分散 scale-out                                                         | 不要                                    | Bigtable                                                       | —                     | —                             | —                                                                                                             |




---



## 4. Baseline / Competitor Priority Table

判定凡例 — P: 実装して性能比較すべき primary / S: 余裕があれば secondary / D: microbaseline・diagnostic / RW: Related Work / responsibility table / design comparison に留める / 無: 今回不要。


| 候補                                  | 分類                                           | 理由                                                                                                                                            | 比較しないときの査読リスク                       |
| ----------------------------------- | -------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------- |
| RocksDB                             | **P（必須）**                                    | production LSM の standard。查読者の期待値。ただし現 wrapper（default options、write 前 Get、8 B 制約 `rocksdb_store.hpp:42-98`）は公平性欠陥があり全面改修が必要                  | 致命的（KVS 論文として成立しない）                 |
| RocksDB BlobDB                      | **P（16 KiB で必須）**                            | VMemKV は value-separated 設計なので、非分離 RocksDB のみとの大 value 比較は「WiscKey の再証明」になる                                                                   | 高（新規性の誤読を招く）                        |
| pread twin（同一 T1 + pread/fadvise）   | **D（必須・最重要 ablation）**                       | mmap の寄与をアーキテクチャ全体から分離する唯一の手段。Bitcask 的設計への実行可能な回答。既存 microbenchmark（`microbenchmark/bench.cpp`）の LRU 版とは別物（buffered pread + page cache 版が必要） | 高（全比較が交絡したまま）                       |
| mmap-only microbaseline             | **D（必須、E5 計画済み）**                            | raw mmap value access の参照点。T2 と同じ mapping mode で構築が条件                                                                                         | 中                                   |
| LMDB                                | **S（main-text 1 図に昇格推奨）**                    | mmap incumbent。vmcache/PrediCache 両方の標準 baseline。導入容易（成熟 C ライブラリ）。B+tree/single-writer なので主競合ではなく診断用                                          | 中〜高（mmap KVS を名乗る以上）                |
| LevelDB                             | RW（appendix 任意）                              | 歴史的 baseline。実用比較は RocksDB で足りる                                                                                                               | 低                                   |
| WiscKey                             | **RW + design comparison（必須引用）**             | 最重要 ancestor。research code（LevelDB 1.18 ベース）の公平な再現は困難で、BlobDB が実行可能な後継。workload・実験形式は必ず借りる                                                    | 実装比較の省略は低リスク、**引用・設計対比の省略は致命的**     |
| Bitcask-like KVS                    | RW + design comparison（必須引用）                 | anticipation threat。pread twin が実行可能な代理。custom 実装 baseline は公平性説明コストが高い（既存 plan §6 の判断を支持）                                                    | 中（pread twin があれば低）                 |
| vmcache                             | **RW / responsibility table（必須引用）**          | standalone KVS ではなく buffer manager。性能勝負は不要かつ不利（挑めば「DBMS 制御はほぼゼロコスト」の実証と正面衝突）。責務比較（eviction 判断・frame table を持たない）が正しい軸                         | 引用省略は致命的、比較省略は低                     |
| LeanStore                           | RW（必須引用）                                     | 同上。TPC-C 中心で KVS API でない。F2 が KVS 比較文脈での LeanStore 数値（WA 30–65x 等, F2 Sec 2.1/Table 2）を既に提供                                                   | 引用省略は高                              |
| Predictive Translation / PrediCache | RW                                           | SIGMOD'26。user-space pool の最新反論。LMDB cliff の実測（Fig 6）を引用して E1 設計の動機に                                                                          | 中                                   |
| ScaleCache                          | RW                                           | production 棄却理由（page table メモリ、Sec 2.2）に footprint 表で回答                                                                                       | 中                                   |
| FASTER                              | RW + design comparison（必須引用）; 任意で appendix S | point-ops 専用（scan なし, Sec 1.1/2.2）なので主比較は不自然。ただし C++ 実装が公開されており、point-op 直接比較を求める査読者は実在。in-place update の差別化は禁物（epoch+CPR vs 無保護 overwrite）   | 引用省略は高（in-place update が title の論文） |
| F2                                  | RW（必須引用）; 任意で appendix S                     | VMemKV の標的レジームでの最強実測（10x LTM で 2.4–3.5 Mreqs/s）。方法論（memory sweep, RA/WA, % device IOPS）を借りる                                                   | 高（知らないと「未調査」に見える）                   |
| fio O_DIRECT bound                  | D（参照線、必須）                                    | device 上界。Crotty/vmcache の標準                                                                                                                  | 中                                   |
| Bigtable                            | RW                                           | 分散システム。LSM 系譜の出自として引用のみ                                                                                                                       | 引用省略は中（系譜の常識）                       |
| How to Write to SSDs                | 方法論ソース（比較対象ではない）                             | WA 会計・fill factor・drilldown 図式を借りる                                                                                                            | 中（WA 主張の信頼性）                        |
| LLFree                              | RW（任意引用）                                     | 「カーネルは改善しうる」側の材料 + mmap_lock/bookkeeping の fault path 内訳（Fig 13）。比較不要                                                                         | 低                                   |
| LIPaH / Otaki                       | RW（一文）                                       | buffer pool 系。KVS 評価に非関係                                                                                                                      | 低                                   |


---



## 5. Paper-by-Paper Notes



### 5.1 WiscKey — Lu, Pillai, Gopalakrishnan, Arpaci-Dusseau×2. ACM TOS 13(1), 2017（FAST'16 拡張版）

- **中心アイデア**: LSM には key と 12 B の value 位置だけを置き、value は unsorted vLog に append。WA を >12 → ~1 に削減（≥1 KB value, Fig 12）。100 GB DB の key LSM は ~2 GB で RAM に載る（Sec 3.2, p.5:10）。unsorted value の range query は 32-thread 並列 prefetch で緩和（Sec 3.3.1）。GC は tail から live record を head に copy（Sec 3.3.2）。crash consistency は append の prefix 性で担保、ALICE で >3,000 crash states 検証（Sec 4.2.4）。
- **VMemKV との同**: RAM 常駐 metadata + offset 間接参照、value record に key+length header（Fig 7 ≒ T2 record）、到達可能 record の copy による回収 ≒ T2 reorganization。**value log の key 順 reorganization（sorting）を将来課題として明示提案（p.5:20）** — VMemKV の T2 reorganize はこの実装に相当する。
- **VMemKV との差**: key 構造が永続 LSM（compaction・recovery あり）vs 揮発 RAM T1。value I/O が explicit syscalls + prefetch vs mmap fault。pure append vs bounded in-place update。incremental GC vs 全量世代交代。実装済み crash consistency vs 未実装。
- **thesis を弱める点**: 新規性が大きく痩せる（上記）。小 value scan で 12x 劣後（Fig 17, 64 B）— VMemKV は並列 prefetch すら持たず、synchronous fault per 4 KiB はさらに悪い可能性。16 KiB でしか勝てないなら「WiscKey/BlobDB が既に占有する領域」と読まれる。
- **採用すべき workload**: value-size sweep（64 B–256 KB）、**Fig 17 の scan 3 状態実験（random-load / after-reorg / seq-load）**、Fig 18 の GC 干渉実験の reorganization 版、latency CDF（Fig 13/16）、space amp（Fig 19）、CPU 表（Fig 20）、ALICE 風 recovery 検証（実装 gate 付き）。
- **判定**: workload=must / baseline=関連研究＋設計対比（実装比較は不要、BlobDB で代替）/ 査読リスク=最大級。



### 5.2 RocksDB experience — Dong, Kryczka, Jin, Stumm. ACM TOS 17(4), 2021（FAST'20 拡張版）

- **中心アイデア**: production LSM の優先順位変遷（WA→space→CPU）。Leveled WA 16.07 / Tiered 4.8（Table 3）。Dynamic Leveled で定常 space overhead 11.8–12.7%（Table 4）。Bloom 有で I/Os per Get 0.99（Table 3）。42 デプロイの資源調査（Fig 2–3）は **space が第一制約** であることを示す（Sec 3.2）。
- **thesis を弱める点**: VMemKV は space に不利な設計（T2 garbage の全量放置→一括回収、alloc_len padding、非圧縮）。「単純さ」の対価として RocksDB が挙げる責務（4 層 checksum、error handling、互換性, Sec 5）を VMemKV は未実装なだけ、という批判が可能。
- **採用すべき workload**: 定常 rate-limited overwrite → space overhead % と WA（Tables 3–4 形式）、Bloom 有無の per-op I/O、read_while_writing、queue 型 delete-heavy（Sec 6.3.1）。
- **公平性ガイド（E0 へ）**: block cache = matched budget、compression off を明記（RocksDB 側は通常 on が production 既定であることも注記）、WAL/sync 方針、direct vs buffered I/O、background jobs 数、rate limiter。**現 wrapper の write 前 Get（**`rocksdb_store.hpp:76-98`**）は標準的 YCSB 使用と乖離しており排除が必要**。
- **判定**: workload=must / baseline=primary must。



### 5.3 Are You Sure You Want to Use MMAP…? — Crotty, Leis, Pavlo. CIDR'22

- **中心アイデア/数値**: read-only best case ですら、page cache 充填後は mmap が fio O_DIRECT の ~50%（random read, Fig 2a: ~27 s まで同等 → ~5 s 近くゼロ → 半分で安定）。TLB shootdown ~1.5–2.0M/s（Fig 2b, /proc/interrupts）。kswapd single-threaded で CPU 律速（Sec 4.1）。10 SSD RAID0 scan で ~20x 差、mmap は 1 SSD から改善せず（Fig 4）。MADV_NORMAL は 1 fault あたり 128 KB 先読み（Sec 2.2）。`mlock` でも dirty page writeback は防げない（Sec 2.2）— **mmap 上の書き込みは WAL-before-data を破る**。MAP_PRIVATE は file に永続化されない（Sec 2.2）。結論は「LTM + 更新 + durability なら使うな」（Sec 6）。
- **VMemKV への含意**: 本論文は「反論対象」ではなく「実験プロトコルの供給源」として使うのが正しい。VMemKV の回答可能な差分は (i) 4 KiB page 粒度 B-tree ではなく value 粒度 point access、(ii) MAP_PRIVATE ゆえ writeback 問題が構造的に不発（代償は swap-bound）、(iii) T1 が RAM で faults は value にのみ発生、(iv) CIDR'22 以後のカーネル（MGLRU 世代）。**どれも測って初めて主張できる**。
- **採用すべき workload**: fio O_DIRECT bound、60 s+ 時系列で 3 相を明示、TLB shootdowns/s の同時系列、madvise モード ablation（MemoryHints は scan 前 MADV_SEQUENTIAL 固定 `memory_hints.hpp:61` — mismatched hint のコストも測る）、高並列 + kswapd CPU。
- **判定**: workload=must（方法論）/ baseline=diagnostic（fio）/ 引用・正面回答は必須中の必須。



### 5.4 vmcache — Leis, Alhomssi, Ziegler, Loeck, Dietrich. SIGMOD'23

- **中心アイデア/数値**: 仮想メモリを translation に使いつつ eviction は DBMS が制御。madvise ベースの page alloc/free は 128 threads 合計で 1.51M pages/s が上限、CPU の 79% が `flush_tlb_mm_range`（Sec 4.1, Fig 3–4）— **1 台の PCIe4 SSD (1.5M IOPS) すら賄えない**。out-of-memory（1 TB / 128 GB, 8x）では素の vmcache は kernel 律速で、exmap カーネルモジュールで初めて I/O bound（Sec 5.3, Fig 7）。in-memory の VM page access は raw DRAM read 比 <8% overhead（Table 2）。LMDB は全実験で劣後。DRAM overhead 会計 ~16 B/4 KiB（Sec 3.6）。
- **thesis を弱める点**: 「OS 委譲は LTM で遅い」の最直接の実証。ARIES 型 WAL は mmap 上では不可能と明言（Sec 2）。「簡単に実装できる」ことも vmcache 自身が主張しており単純性の独占は不可。
- **thesis を強める点**: Table 2 は「hit path の VM translation はほぼ無料」を示し、T1/T2 hit path の低コスト仮説を支持。この論文の限界（Zipf なし、KV value サイズなし、scan なし）が VMemKV の空き地でもある。
- **採用すべき workload**: fio bound 付き out-of-memory 時系列（Fig 7 形式）、kernel flame graph（Fig 4 形式）、Table 2 形式の hit-path ablation（T1 sorted/append/AppendMap/Inline64 の ns・instructions）、DRAM overhead 会計。
- **判定**: workload=一部 must（方法論）/ baseline=**しない**（responsibility table で対比）。



### 5.5 LeanStore 2024 — Leis. PVLDB 17(12)

- **要点**: NVMe 向け storage engine 総説。「あらゆる現代 DBMS は buffer manager を要する」（Sec 2.1）、buffer manager overhead はほぼ解消済み（Fig 4: Shore 比 10x、有効仕事支配）。mmap は「semantical/performance 問題」と一蹴（Sec 2.1）。本 PDF 自体の評価は TPC-C instruction 内訳のみで薄い。
- **注意**: 草稿の [LeanStore] 参照は 2018 ICDE pointer-swizzling 論文の記述になっており、**corpus の PDF は 2024 PVLDB 版**。両方引くか選ぶ（先行査読 P3 と同指摘）。
- **判定**: workload=不要 / baseline=Related Work のみ。「buffer manager は重い」という動機付けは本論文で反証済みのため、simplicity は**責務・工数**の軸でのみ主張すること。



### 5.6 FASTER — Chandramouli et al. SIGMOD'18

- **要点**: hash index + HybridLog（in-place 可変域 + read-only 域）。160M ops/s 級（Abstract）。larger-than-memory は memory budget sweep（27 GB dataset, Fig 10）。RocksDB は同条件で ~500K ops/s（Sec 7.3）。scan 非対応（Sec 1.1）。epoch 保護 + CPR 系 recovery。
- **thesis を弱める点**: 「LTM KVS の in-place update」は本論文のタイトル。explicit 管理 + async I/O が性能側の existence proof であり、VMemKV の残る通貨は simplicity + scan + OS 委譲の特徴づけのみ。synchronous fault で FASTER 級の数値は原理的に不可能。
- **採用すべき workload**: memory-budget 連続 sweep（knee の形状）、R:U 比率変種、hot-set drift、**in-place vs always-append ablation（Fig 11 形式）**、garbage 生成レートの副軸（Fig 12a 形式）。
- **判定**: workload=should / baseline=optional（appendix）。in-place update を FASTER との差別化点にしないこと（差分は file-backed OS residency のみ）。



### 5.7 F2 — Kanellis, Chandramouli, Hart, Venkataraman. PVLDB 18(12), 2025

- **要点**: large skewed LTM 向けの FASTER 後継（hot/cold 2 層 log + read cache + <1 B/key の cold index）。memory=10% で RocksDB の 11.8x、SSD 利用率 85–90%（Sec 8.3, Fig 10–11）。skew sweep（α3–1000, Fig 12）、memory sweep 2.5–25%（Fig 13）、RA/WA を /proc/pid/io で全実験報告（Table 2）。KVell は memory 圧では RA/WA 25–95x（Sec 2.1, Table 2）— **RAM 常駐 index 構造が page out される設計の末路**として、T1 への直接警告。
- **thesis を弱める点**: 最危険。実測バーが高すぎる（同レジームで桁差の可能性）。8 B/key ですら「法外」とする T1 footprint 攻撃（Sec 3.2）。scan 非対応が唯一の明確な空き地。
- **採用すべき workload**: memory-budget sweep（% 表記, log 軸）、skew sweep、RA/WA 常時報告、% of device IOPS 表記、warm-up→計測の方法論（Sec 8.1: WAL/compression/checksum off, pinning）。
- **判定**: workload=should〜must（方法論）/ baseline=optional（appendix）。scope 文（ordered scan を持つ engine の比較）で正面回避 + 数値引用で誠実に。



### 5.8 Bitcask — Sheehy, Smith. Basho whitepaper, 2010（査読なし）

- **要点**: RAM keydir（full key + 位置）+ append-only files + merge。read residency は「OS の filesystem cache に委譲、自前 cache なし」と明言（p.3–4）。>10x RAM で挙動変化なし、5–6K writes/s、sub-ms median（p.5、いずれも条件未記載の逸話的数値）。range scan なし（keydir は hash）。
- **thesis を弱める点**: 「OS 委譲 + RAM index + 単純さ」は 2010 年に主張済み。VMemKV の delta は sorted T1（scan）/ mmap（pread ではなく）/ in-place update / 世代交代 reorganize に限られる。**mmap は自傷的複雑化では？（Bitcask は pread で同じ委譲を達成）** という問いに pread twin で答える必要。
- **thesis を強める点**: T1 は 16 B prefix + 60-bit hash で full key を持たない — keydir 比のメモリ論法は VMemKV 側が有利（aliasing 確率の脚注付きで使う）。
- **採用すべき workload**: ≥10x 比の 1 点（または T1 制約でなぜ不可能かの明示）、sustained ingest、startup/recovery time、T1 bytes/key 表。
- **判定**: workload=should / baseline=design comparison（pread twin が代理）。



### 5.9 Bigtable — Chang et al. OSDI'06

- **要点**: memtable+SSTable+compaction の起源。評価は 1000 B value、1–500 tablet servers の scale-out（Sec 7）。**SSTable の memory-mapped 運用に言及（Sec 4）**、かつ「メモリ常駐は動的判断ではなくユーザ指定」（Sec 10）— 系譜の創始者が動的 OS 委譲を選ばなかった記録。
- **判定**: workload=不要 / baseline=Related Work のみ（LSM 系譜・compaction 用語の出自として引用。VMemKV の reorganization と major compaction の対応も一文で認める）。



### 5.10 How to Write to SSDs — Lee, Ziegler, Leis. PVLDB'26（拡張版, arXiv:2603.09927v3）

- **要点**: Total WAF = DB WAF × SSD WAF。in-place B-tree は 4 KiB page write あたり flash 18.85 KiB（4.7x, Fig 1）。fill factor が支配的（naive out-of-place は in-place より悪化, Fig 13b）。GC の DB WAF = 1/(1−valid_ratio)（Sec 4.1）。PM9A3 の 1 DWPD ≒ 平均 11 MB/s — endurance は実制約（Sec 1）。方法論: blkdiscard 前処理、装置容量 4x 書き込み後の最終 1 時間平均、OCP/SMART で物理書き込み取得（Sec 7.1）。
- **VMemKV への含意**: (i) E2 の WA 会計はこの方法論をそのまま使う。(ii) VMemKV の in-place T2 update は本論文が列挙する in-place の病理（write placement 喪失、torn write、sector 粒度原子性, FAQ 10.6）を継承。(iii) MAP_SHARED regime での OS writeback は「random 4 KiB 書き戻し」であり SSD WAF 2–2.7x 級（Fig 14）を疑うべき。(iv) T2 reorganization は sequential 大書き込みで SSD 親和的 — 測れば強み側にもなりうる。
- **採用すべき workload**: fill-factor sweep、write drilldown 図（user 書き込み / writeback / reorganize copy / WAL の分解, Fig 13b/c 形式）、Zipf θ0.8 update-heavy。
- **判定**: workload=must（metrics）/ baseline=不要（方法論ソース）。



### 5.11 Predictive Translation — Zinsmeister, Nguyen, Leis, Neumann. SIGMOD'26

- **要点**: hash-table buffer pool に「予測 frame」を付けて translation を投機実行 → vmcache/LeanStore と同等以上、kernel module 不要。LMDB は out-of-memory 移行で崩落（Fig 6, Crotty の追認）。8 B key / 120 B value、uniform + Zipf θ0.9、~1 TB/128 GB。
- **判定**: workload=一部参考（in/out-of-memory transition、θ0.9 skew）/ baseline=Related Work のみ。「user-space pool は複雑・低速」という前提を最終的に破壊した論文として、simplicity 主張の書き方を制約する。



### 5.12 ScaleCache — Liu et al.（Huawei）. PVLDB 18(12), 2025

- **要点**: GaussDB の production buffer manager を 256 core までスケール。**mmap 系設計（Kreon/Tucana/FastMap/vmcache）を「page table メモリ肥大」で production 棄却（Sec 2.2）**。out-of-memory 35x/29x 構成もあり（Sec 4.7）。
- **判定**: workload=不要 / baseline=Related Work のみ。page-table overhead を footprint 表で定量回答すること（~8 B/4 KiB → T2 1 TB あたり ~2 GB）。



### 5.13 LLFree — Wrenger et al.（venue: PDF に記載なし、**Needs verification**）

- **要点**: Linux buddy allocator の置換。**Fig 13 の fault path 内訳: mmap rw-lock 17.3% + struct-page/LRU/cgroup/rmap 帳簿 28.7%、allocator 自体は 5.3%** — mmap fault path のスケール限界は allocator ではなく mmap_lock と帳簿にある。end-to-end（memtier）では allocator 10–100x 改善が不可視（Fig 11–12）。
- **判定**: workload=不要 / baseline=不要 / Related Work 任意。多スレッド LTM 実験の設計根拠（mmap_lock 律速）と、THP 主張時の huge-frame fragmentation 注意（Fig 10）として使う。



### 5.14 LIPaH / Otaki — Otaki, Chang, Benello, Elmore, Graefe. CIDR'25

- **要点**: buffer pool page を query 中間状態にも使う resource-adaptive 実行。LIPAH（logical ID + frame hint の fat pointer）で mapping 競合を回避。評価は自プロトタイプ内 on/off のみ（TPC-H SF1、10M pairs hash index, Sec 4）。
- **判定**: workload=不要 / baseline=Related Work 一文。「OS 委譲 eviction は consumer に通知されず hot data を壊す」（Sec 2.2）という批判は cgroup 圧力実験の動機として引用可。T1 payload（offset/inline）と fat-pointer の概念的類似も一文で使える。

---



## 6. Additional References to Read

別ファイル `additional_references_to_read.md` に完全な表を置く。最優先のみ抜粋:

1. **Stoica & Ailamaki (DaMoN'13)** — 「hot は RAM に固定、cold は OS paging へ」の直接の祖先。未引用だと新規性表明が事実誤認になる。
2. **KVell (SOSP'19)** — RAM full-index + unordered SSD slabs + direct I/O。mmap を使わない最近傍。F2 が KVell の index page-out 時 25–95x amplification を実測しており、T1 が RAM から溢れた時の VMemKV の未来図。
3. **LMDB（正式引用 + baseline 導入）**。
4. **Tucana (ATC'16) / Kreon (SoCC'18) / FastMap (ATC'20)** — mmap KVS 工学系譜。「mmap KVS は新しくない」への防御線。
5. **ALICE (OSDI'14)** — E6 を実施する場合の crash-state 方法論。
6. **HyPer fork snapshot (ICDE'11) + Redis BGSAVE** — fork を残す場合の必須先行研究。
7. **LeanStore 2018 (ICDE)** — 参照の 2018/2024 取り違え解消。
8. **BlobDB / Titan / HashKV / BadgerDB** — value separation の実用系譜（BlobDB は baseline、他は Related Work）。
9. **MGLRU / DAMON（カーネル文書）** — 「CIDR'22 以後カーネルは改善した」側の根拠。
10. **SILK (ATC'19)** — LSM tail latency 制御。reorganization 干渉実験の対置として。

---

---



## 7. Final Decision Table


| Item                                     | Decision                                                | Priority                      | Reason                                                       | Risk if omitted               |
| ---------------------------------------- | ------------------------------------------------------- | ----------------------------- | ------------------------------------------------------------ | ----------------------------- |
| YCSB A/B/C/E/F                           | include（D は appendix）                                   | must                          | KVS 論文の共通語彙。WiscKey/F2 と同型で比較可能に                             | 標準ベンチ不在で門前払い                  |
| RocksDB                                  | primary baseline                                        | must                          | production LSM の standard。ただし公平設定を全面再構築                      | 論文として成立しない                    |
| RocksDB BlobDB                           | primary baseline (16 KiB)                               | must                          | 非分離 RocksDB のみとの大 value 比較は WiscKey の再証明                     | 新規性・公平性の двойной 批判           |
| WiscKey                                  | design comparison / related work（実装比較なし）                | must（引用・対比）                   | 最重要 ancestor。BlobDB が実行可能な代理。Fig 17/18 の実験形式は借用              | 引用欠落は致命的。実装比較欠落は低リスク          |
| Bitcask-like KVS                         | design comparison（pread twin が代理）                       | must（対比）/ optional（custom 実装） | anticipation threat。custom baseline は公平性説明コスト過大              | pread twin があれば低              |
| LMDB                                     | secondary/diagnostic baseline（main-text 1 図）            | should                        | mmap incumbent。導入容易。vmcache/PrediCache の標準 baseline          | 中〜高（mmap KVS を名乗る以上）          |
| vmcache                                  | design comparison / related work                        | must（引用）                      | buffer manager であり KVS API でない。性能勝負は thesis に不利で不要。責務対比が正しい軸 | 引用欠落で「無知」認定                   |
| LeanStore                                | related work                                            | must（引用）                      | 同上。F2 経由の数値で文脈は足りる                                           | 高（motivation が stale 認定）      |
| FASTER / FasterKV                        | related work + design comparison（appendix baseline は任意） | should                        | point-ops 専用で主比較は不自然だが、in-place update の先行者として正面対比必須         | 高（title 級の重複を無視）              |
| F2                                       | related work（appendix baseline は任意）                     | should                        | 標的レジームの最強実測。方法論借用 + 数値引用で誠実に                                 | 高                             |
| mmap-only microbaseline                  | diagnostic baseline                                     | must                          | raw mmap 参照点（E5）。T2 と同一 mode で                               | 中（T1/T2 の寄与が不可分に）             |
| pread-based microbaseline / pread twin   | diagnostic baseline                                     | must                          | mmap の寄与を分離する決定的 ablation。Bitcask/Crotty への回答                | 高（全差分が交絡）                     |
| page fault metrics                       | include                                                 | must                          | thesis の機構語彙そのもの（Crotty/vmcache）                             | 高                             |
| tail latency metrics (p50–p99.9 + 時系列)   | include                                                 | must                          | eviction cliff は平均に隠れる（Crotty Fig 2a）                        | 高                             |
| write amplification (DB/SSD WAF 分離)      | include                                                 | must                          | How to Write の方法論。endurance は実制約                             | 高                             |
| storage amplification（定常 space overhead） | include                                                 | must                          | production 第一制約（RocksDB Sec 3.2、Table 4 の ~13% 基準）           | 中〜高                           |
| recovery / WAL replay                    | include with gate（未実装なら future work）                    | should（gate 付き）               | 実装 0%（TODO §1）。実装され次第 sanity check として                       | 実装済みと書けば致命的、future work 明記なら低 |
| checkpoint experiment                    | include with gate（fork/no-fork 決定後）                     | should（gate 付き）               | TODO は fork 廃止方針で plan E4 と矛盾中                               | 矛盾放置は高                        |
| responsibility comparison table          | include（3 値列: OS委譲/設計回避/未実装）                            | must                          | simplicity を監査可能に。未実装を「回避」に数えない                              | simplicity 主張が主観認定            |
| weaker thesis                            | needed                                                  | must                          | thesis/risk 分析により現 thesis は防御不能                              | 現 thesis のままなら reject 級リスク    |


---



## 8. Appendix: Notes and Unresolved Questions



### 8.1 最終 13 問への短答

1. **RocksDB 比較は YCSB だけで十分か** — 不十分。YCSB load+A/B/C/E/F は必要条件。加えて eviction-onset 時系列、WA/space 会計、value-size 感度（小 value 含む）、reorganization 干渉、tail latency 時系列が thesis の検証に必須。
2. **BlobDB は必須 baseline か** — 16 KiB 以上では必須。1 KiB では任意。
3. **WiscKey-like は実装比較すべきか** — 不要。design comparison + BlobDB + pread twin で査読者の問いには全て答えられる。workload 形式（Fig 17/18/CDF/space amp）は必ず借りる。
4. **Bitcask-like を実性能 baseline にすべきか** — custom 実装としては不要（公平性説明コスト過大）。pread twin が実行可能な代理として must。
5. **LMDB を実性能 baseline にすべきか** — すべき（secondary/diagnostic）。main text に最低 1 図。
6. **vmcache / LeanStore は** — design comparison / related work で十分。性能勝負は thesis に必要なく、挑めば不利。引用と定量的応答は必須。
7. **FASTER / FasterKV は** — Related Work + 設計対比が必須、実測比較は appendix 任意。point-ops 専用で主比較は不自然。
8. **mmap-only microbaseline は必要か** — 必要（must, E5）。T2 と同じ mapping mode で。
9. **pread-based microbaseline は必要か** — 必要（must）。本レビューの最重要追加項目。
10. **evaluation_plan.md に足りない最重要項目** — (1) pread twin、(2) regime protocol（load→reorganize→drop_caches）、(3) MVE と cut line、(4) T1 footprint 表、(5) E4/E6 の実装 gate と fork 問題の決定。
11. **実装に足りない最重要 benchmark/instrumentation** — reopen 経路 + 容量修正 + mapping 統一（LTM 実験の前提）、YCSB 相当ドライバ（value size/percentile）、OS 計測一式、RocksDB 公平化 + BlobDB。
12. **現在の thesis は強すぎるか** — 強すぎる。
13. **防御可能な thesis** — 「read 側 OS 委譲 + WAL 分離 durability の design point を、成立・不成立の両側の境界込みで定量特徴づけする」characterization thesis。



### 8.2 既存文書との関係

- `paper/others/reference_evaluation_matrix.md` / `related_work_workload_matrix.md` の結論（YCSB 不十分、BlobDB 必須、vmcache と勝負しない、pread twin 最重要、LMDB main-text 1 図）を PDF 本文精読で**追認**した。本文書はそれらに節・図・表番号の根拠と、実装監査由来の新規制約（reopen 不能、容量 wedge、SimdScan dead code、reorganize race）を加える。
- `paper/draft/vmemkv_paper_review.md`（先行査読）の P0 3 点（mapping mode、fork 宙吊り、E1 ill-defined）を独立に再検証し**一致**。同レビューの引用数値も PDF 照合で概ね確認。1 点補正: 「~1.5M page-ops/s/CPU」は正しくは **128 threads 合計で 1.51M pages/s**（vmcache Sec 4.1, Fig 3）。
- 既存 microbenchmark 結果（`microbenchmark/REPORT.md`）は「mmap vs pread+LRU (O_DIRECT)」であり、E5 の「pread + OS page cache」対照が欠けている非対称は各 REPORT 自身が注記済み。

