# 関連研究・ワークロード・ベースライン判断表

Status: 計画メモ。ここでは実験結果を主張しない。

このファイルは `paper/evaluation_plan.md` とは意図的に分けた独立メモである。`evaluation_plan.md` は論文本体の評価構成を定義する。一方で、このファイルは `must_read_papers/` と `paper/draft.md` 内の関連研究をもとに、「どの workload を VMemKV で実施すべきか」「どのシステムを直接比較対象にすべきか」を判断するための表である。

## 先に結論

| 疑問 | 推奨 | VMemKV での具体的対応 |
| --- | --- | --- |
| RocksDB と比較するのに YCSB だけで十分か？ | 不十分。YCSB は main macrobenchmark として必要だが、RocksDB 評価全体を YCSB だけに閉じるべきではない。 | YCSB load + A/B/C/E/F は維持する。その上で、larger-than-memory stress、write amplification / storage metrics、compaction と reorganization の長時間挙動、tail latency / page fault の時系列を追加する。 |
| VMemKV は `vmcache` と直接勝負すべきか？ | main evaluation では不要。ただし、論文が「DBMS buffer manager に勝つ」と主張するなら必要になる。 | vmcache は Related Work と responsibility table の比較対象にする。vmcache は standalone KVS ではなく buffer-management design なので、直接 benchmark は optional。 |
| VMemKV は WiscKey と直接比較すべきか？ | 実用的には RocksDB BlobDB を value-separated RocksDB baseline として優先する。 | WiscKey は最も近い academic ancestor として議論する。large value 実験では RocksDB BlobDB を使う。WiscKey 実装が容易に再現できる場合のみ追加する。 |
| VMemKV に LMDB を入れるべきか？ | optional / appendix baseline。 | LMDB は mmap-based KVS として有用。ただし、VMemKV の主張は「mmap が mmap-B+tree に勝つ」ではないため、本編必須ではない。reviewer から mmap KVS baseline を求められる場合に備える。 |
| Bitcask-like baseline を入れるべきか？ | optional。優先度は低い。 | append-only log + in-memory index の最小比較としては有用。ただし custom baseline は公平性説明が難しく、実装作業が増える。 |
| FASTER / F2 を直接比較対象にすべきか？ | framing 次第。 | 論文を「OS-delegated larger-than-memory value residency」に絞るなら Related Work で十分。一般的な high-performance KVS 勝負に広げるなら optional baseline として検討する。 |

## 直接比較 baseline の候補

| 優先度 | baseline / variant | 理由 | 主に使う workload | 注意 |
| --- | --- | --- | --- | --- |
| 必須 | RocksDB | 成熟した LSM-tree KVS。最も重要な実用 baseline。 | YCSB load, A/B/C/E/F。1 KiB / 16 KiB values。memory budget と durability を揃える。 | YCSB だけでは不足。write amplification、storage usage、compaction behavior、tail latency も見る。 |
| large value では必須 | RocksDB BlobDB | VMemKV は value を T2 に分離するため、通常 RocksDB だけとの比較では不公平になり得る。 | 16 KiB 以上の value。特に load / update-heavy / write-amplification 実験。 | setup が重い場合は large-value subset のみでもよい。 |
| 必須診断用 | mmap-only microbaseline | raw mmap-backed value access のコストと、VMemKV の T1/T2 機構のコストを分離する。 | Get hit/miss、scan、update、larger-than-memory。 | durability-matched competitor ではない。diagnostic baseline と明記する。 |
| 必須 | VMemKV ablations | T1/T2 split と各 optimization が本当に効いているかを見る。 | AppendMap は point/update、Bloom は negative lookup、SIMD は scan、MemoryHints は larger-than-memory / reorganization、Inline64 は small value。 | 全組み合わせを網羅しない。効くはずの workload だけ示す。 |
| optional | LMDB | mmap-based KVS の代表的比較対象。 | Get / scan / larger-than-memory。 | mmap 位置づけを問われた場合に有用。本編必須ではない。 |
| optional | LevelDB | WiscKey の元 baseline であり、歴史的 LSM baseline。 | YCSB / db_bench の小 subset。 | 実用 baseline は RocksDB で足りることが多い。 |
| optional | WiscKey implementation | VMemKV の key/value separation に最も近い academic system。 | YCSB 1 KiB / 16 KiB、range scan、GC / recovery。 | 実装が再現できる場合のみ。難しければ conceptual comparison に留める。 |
| optional / low | Bitcask-like custom baseline | append-only data file + in-memory key directory を切り出す比較。 | point read/write、merge / reorganization。 | custom baseline は公平性説明が必要。 |
| 通常は不要 | vmcache / LeanStore / Predictive Translation | standalone KVS ではなく DBMS buffer-manager 系。 | 本編では直接 workload なし。 | Related Work と responsibility table に入れる。主張を広げるなら direct benchmark を検討する。 |

## 関連研究から逆算した workload checklist

| workload / metric family | VMemKV で必要か | 動機になる関連研究 | なぜ重要か |
| --- | --- | --- | --- |
| YCSB load + A/B/C/E/F | main | WiscKey、RocksDB-style KVS evaluation、FASTER/F2-style KVS comparison。 | KVS macrobenchmark として認知されており、「RocksDB と競争できるか」に答えやすい。 |
| 1 KiB / 16 KiB values | main | WiscKey、BlobDB-style value separation。 | 通常サイズ value と、T2 分離が効くはずの large value を分けられる。 |
| larger-than-memory ratio、例: dataset / memory = 4x, 8x | main | VMemKV thesis、mmap criticism、vmcache / LeanStore / F2。 | page cache eviction が始まった後も OS-delegated value residency が成立するかを直接見る。 |
| uniform / skewed access | main | WiscKey、F2、buffer-manager papers。 | cold random fault が支配的な条件と、hot set が OS cache に残る条件を分離する。 |
| short scan / range scan | main | Bigtable、WiscKey、Bitcask contrast、RocksDB。 | value separation と T2 layout は scan に不利になり得る。T1/T2 reorganization の価値を示す場面でもある。 |
| write amplification / storage usage | main | WiscKey、RocksDB、How to Write to SSDs。 | VMemKV の value separation と in-place update は throughput だけでなく write traffic で評価すべき。 |
| tail latency / time series | main | mmap criticism、RocksDB operational evaluation。 | OS page fault、dirty writeback、compaction、reorganization は平均値に隠れやすい。 |
| page faults / TLB misses / TLB shootdowns / SSD bandwidth | main | mmap criticism、vmcache、Predictive Translation。 | mmap-backed T2 が bottleneck かどうかを説明するために必要。 |
| reorganization / GC / merge behavior | main | WiscKey GC、Bitcask merge、RocksDB compaction。 | VMemKV が ordering fragmentation / storage fragmentation をどう修復するかを示す必要がある。 |
| recovery sanity check | main だが scoped | WiscKey recovery、RocksDB durability expectations。 | checkpoint + WAL replay path が機能することを示す。ただし full crash-consistency proof ではない。 |
| TPC-C | 本編では不要 | vmcache、LeanStore、Predictive Translation、How to Write to SSDs。 | VMemKV は transaction を持たない standalone KVS なので、TPC-C は scope を混乱させる。論文の主張を変える場合のみ検討する。 |
| production-trace evaluation | 不要 / future work | RocksDB experience paper。 | 有用だが、初稿の必須条件ではない。再現性も難しい。 |
| distributed scaling | 不要 | Bigtable。 | VMemKV は distributed storage system ではない。 |

## 各論文・システムごとの判断表

| 論文 / システム | 概要 | その論文での workload / 比較対象 | VMemKV でその workload をやるべきか | VMemKV の直接 rival にすべきか | 本稿での扱い |
| --- | --- | --- | --- | --- | --- |
| Bigtable | tablet、memtable、SSTable、commit log、Bloom filter、cache、compaction を備える distributed sorted map。LSM / SSTable 系譜の重要な起点。 | random read、in-memory random read、random write、sequential read/write、scan、tablet server 数による scale-out、production deployment。 | 一部のみ。point read/write と scan の概念は使うが、distributed server scaling は不要。 | No。Bigtable は distributed service であり、standalone embedded KVS の baseline ではない。 | Related Work。LSM / SSTable / compaction の系譜説明に使う。 |
| RocksDB | compaction、block cache、Bloom filter、WAL、compression、I/O tuning を持つ production LSM-tree KVS。 | production workload observation、compaction-mode comparison、db_bench-style microbenchmark、CPU / space / write amplification analysis。 | Yes。YCSB に加えて write amplification、storage usage、tail latency、memory-budget fairness、long-run compaction-vs-reorganization behavior を見る。 | Yes。primary baseline。 | 本編必須 baseline。ただし全 workload で勝つ必要はないという framing にする。 |
| RocksDB BlobDB | RocksDB における large-value separation / blob file 機構。 | large-value read/write、blob / value-separation tuning。 | Yes。16 KiB 以上の value で必要。 | Yes。large-value subset では直接比較対象。 | large value 比較では必須。特に write amplification の公平性に関わる。 |
| WiscKey | LSM-tree に key + value pointer を置き、value を append-only value log に分離する。VMemKV の key/value separation に最も近い academic ancestor。 | LevelDB db_bench、sequential/random load、random lookup、range query with prefetch、value-log GC、crash recovery、YCSB A-F。LevelDB / RocksDB / WiscKey 比較。 | Yes。YCSB A-F subset、1 KiB / 16 KiB values、range scan、GC/reorganization、write amplification、recovery sanity check を借りる。 | 通常は No。実測は BlobDB を優先。WiscKey 実装が容易なら optional。 | closest prior work として議論。実用的な value-separated LSM rival は RocksDB BlobDB にする。 |
| Bitcask | append-only data files と RAM-resident key directory を持つ log-structured hash table。point lookup と write path が単純。merge で obsolete entries を回収する。 | 詳細 benchmark は限定的。point read/write latency、writes/sec、larger-than-memory data volume、merge / hint file の議論。 | 一部のみ。point operations と merge/reorganization-like reclamation は参考になる。 | 本編では No。custom baseline としては optional。 | Related Work と responsibility table。fair な実装がある場合のみ appendix baseline。 |
| LMDB | mmap-based B+tree-style embedded KVS。mmap KVS の代表的比較対象。 | vmcache 系比較などで mmap storage engine として評価されることが多い。 | optional。Get / Scan / larger-than-memory を時間があれば実施。 | optional。 | mmap KVS 競合を求められた場合の appendix baseline。VMemKV は B+tree 全体ではなく value layer を mmap する点が異なる。 |
| Are You Sure You Want to Use MMAP? | DBMS / storage system における mmap の落とし穴を整理する文献。eviction、page fault、dirty writeback、TLB shootdown、page-cache control が主題。 | fio / O_DIRECT vs mmap。random reads、sequential scans、page-cache-limited larger-than-memory、SSD bandwidth scaling、TLB shootdowns。 | Yes。page faults、TLB events、cgroup / page-cache pressure、SSD bandwidth、time-series behavior を測るべき。 | No。KVS ではない。 | motivation と instrumentation guidance として使う。 |
| vmcache | virtual-memory-assisted DBMS buffer manager。VM address translation を活用するが、page fault / eviction / replacement は DBMS が制御する。 | in-memory / out-of-memory DBMS workloads、TPC-C-like、random-read。LeanStore、WiredTiger、LMDB、exmap variants と比較。 | workload を丸ごと採用する必要はない。page faults、TLB shootdowns、scalability metrics は参考にする。TPC-C は中心ではない。 | main では No。 | Related Work + responsibility table。buffer-manager design に勝つ主張へ広げるなら direct comparison を検討する。 |
| LeanStore | explicit buffer management と pointer swizzling を用いる larger-than-memory storage engine。 | LeanStore 系では YCSB / TPC-C-style page-based workloads が使われる。 | main では不要。explicit-buffer-manager trade-off の動機として使う。 | main では No。 | Related Work。responsibility table の explicit buffer-pool / pointer-swizzling 系として含める。 |
| Predictive Translation / PrediCache | preferred-frame prediction により hash-table buffer manager の translation overhead を下げる手法。 | TPC-C、uniform random read、skewed YCSB、in-memory / out-of-memory。LeanStore、vmcache、WiredTiger、LMDB、traditional buffer pool と比較。 | 一部のみ。skewed YCSB と microarchitectural metrics は有用。TPC-C は scope 外。 | No。 | Related Work。CPU/TLB overhead 測定の動機づけ、および explicit buffer management と OS delegation の対比に使う。 |
| How to Write to SSDs | SSD-conscious DBMS write placement。DB write amplification と SSD write amplification を分けて測る。 | YCSB-A、TPC-C、DB WAF、SSD WAF、total WAF、throughput、logical/physical writes、複数 SSD 種別。 | metrics として Yes。logical bytes、engine-written bytes、device bytes、storage usage を測るべき。 | No。 | write amplification 測定のガイドとして使う。KVS rival ではない。 |
| FASTER | hash index + hybrid log による concurrent KVS。in-place update と read-modify-write を扱う。 | point operations、update-heavy / read-modify-write、concurrency scaling、recovery-related measurements。 | optional。YCSB-F / read-modify-write は既に有用。FASTER 直接比較は concurrency を中心主張にする場合のみ。 | optional。 | hybrid log + in-place update の Related Work。実装が容易で、high-concurrency KVS 性能を主張するなら direct benchmark。 |
| F2 | large / skewed / larger-than-memory workload を対象にする KVS。hot/cold record separation と NVMe を意識した設計。 | larger-than-memory skewed point workloads。RocksDB、SplinterDB、KVell、LeanStore、FASTER と比較。memory constraints 下の throughput と write amplification。 | 一部 Yes。skewed larger-than-memory workload は VMemKV の target regime を直接試す。 | optional。 | 現状は Related Work。実装が再現でき、large-skewed KVS competition に framing を広げるなら appendix rival。 |
| YCSB | 標準的な cloud-serving KVS benchmark suite。 | load と A-F workloads。update-heavy、read-heavy、read-only、read-latest、short scan、read-modify-write。 | Yes。 | rival ではない。 | main macrobenchmark。ただし YCSB だけでは不足。 |
| LevelDB | 歴史的 LSM KVS。WiscKey の baseline ancestor。 | 多くの LSM 論文で db_bench / YCSB に使われる。 | optional。 | optional。 | historical / appendix baseline。実用 baseline は RocksDB。 |
| ScaleCache | Notion / GitHub entry はあるが、現状メモが空に近く、正確な役割は要確認。 | TBD。 | 明確になるまで No。 | No。 | TODO: cache hierarchy / KVS / DBMS buffer-management のどれに近いか確認する。 |
| LIPaH (Otaki) | entry はあるが現状メモが空。reading-order note からは index / prediction / buffer-management 関連の可能性がある。 | TBD。 | 明確になるまで No。 | No。 | TODO: 読んだ後に補完。おそらく Related Work のみ。 |
| LLFREE | entry はあるが現状メモが空。reading-order note からは lock-free / concurrency 系の可能性がある。 | TBD。 | VMemKV が lock-free concurrency を contribution にしない限り不要。 | No。 | TODO: 読んだ後に補完。evaluation-critical ではなさそう。 |
| Aether / group commit references | draft references に WAL / group commit 関連として出てくる可能性がある。 | durability / write latency / group commit 系の評価。 | WAL 実装を中心にする場合のみ。 | No。 | WAL optimization の optional citation。workload baseline ではない。 |

## 本編評価への推奨パッケージ

本編では、次の組み合わせを基本にするのがよい。

1. **RocksDB 比較**: YCSB load + A/B/C/E/F、1 KiB / 16 KiB values、matched memory / durability。加えて write amplification と storage usage を測る。large values では RocksDB BlobDB を含める。
2. **YCSB だけでは見えない system behavior**: larger-than-memory thread scaling、page-fault / bandwidth time series、TLB counters、uniform vs skewed access。
3. **T1/T2 design の価値**: mmap-only microbaseline と VMemKV ablations。
4. **maintenance behavior**: reorganization / checkpoint / recovery sanity check。foreground tail latency、bytes copied / reclaimed を含める。
5. **responsibility table**: VMemKV、RocksDB/LSM、WiscKey-like value log、Bitcask-like KVS、LMDB、explicit buffer-pool engine を比較する。

この構成なら、査読者が持ちそうな次の疑問に直接答えられる。

- RocksDB と比較するのに YCSB だけでいいのか？  
  → YCSB は必要だが不十分。larger-than-memory behavior、write amplification、page faults、tail latency、reorganization まで見る。

- vmcache と勝負する必要があるのか？  
  → main rival ではない。vmcache は重要な Related Work だが、standalone KVS ではなく buffer-management design なので、直接比較は主張を広げる場合の optional とする。
