# Related-work workload and baseline decision matrix

Status: planning memo. No empirical result is claimed here.

This file is intentionally separate from `paper/evaluation_plan.md`. The evaluation plan defines the main paper structure; this file is a decision memo for selecting workloads and competitors from `must_read_papers/` and from the paper draft references.

## Immediate answers

| Question | Recommendation | Concrete action for VMemKV |
| --- | --- | --- |
| Is YCSB alone enough when comparing against RocksDB? | No. YCSB should be the main macrobenchmark, but not the whole RocksDB evaluation. | Keep YCSB load + A/B/C/E/F as the macrobenchmark. Add larger-than-memory stress, write-amplification/storage metrics, long-running compaction-vs-reorganization behavior, and tail-latency/page-fault time series. |
| Should VMemKV directly compete with `vmcache`? | Not in the main evaluation, unless the paper claims to beat DBMS buffer managers. | Treat vmcache as related work and as a responsibility-table comparison point. Direct benchmarking is optional because vmcache is a buffer-management design, not a standalone KVS competitor. |
| Should VMemKV directly compete with WiscKey? | Prefer BlobDB as the practical value-separated RocksDB baseline. | Discuss WiscKey as the closest academic ancestor. Use RocksDB BlobDB for large-value experiments; only add WiscKey if a reproducible implementation is easy to run. |
| Should VMemKV include LMDB? | Optional appendix baseline. | LMDB is useful as an mmap-based KVS contrast, but the main claim is not “mmap beats mmap-B+tree.” Keep it optional unless reviewers ask for an mmap KVS baseline. |
| Should VMemKV include a Bitcask-like baseline? | Optional, low priority. | A custom Bitcask-like baseline would help isolate append-only log + in-memory index, but fairness is hard and it risks becoming a second implementation project. |
| Should VMemKV include FASTER or F2? | Optional, depending on framing. | Include only if the paper expands from “OS-delegated larger-than-memory value residency” to “general high-performance KV competition.” Otherwise discuss in Related Work. |

## Direct baseline shortlist

| Priority | Baseline / variant | Why | Main workload to use | Notes |
| --- | --- | --- | --- | --- |
| Required | RocksDB | Mature LSM-tree KVS; primary practical baseline. | YCSB load, A/B/C/E/F; 1 KiB and 16 KiB values; matched memory and durability. | YCSB alone is insufficient; also report write amplification, storage usage, compaction behavior, and tail latency. |
| Required for large values | RocksDB BlobDB | Value-separated RocksDB avoids unfairly comparing VMemKV only against non-value-separated RocksDB. | 16 KiB values, especially load/update-heavy/write-amplification experiments. | If setup is expensive, include for the large-value subset rather than every workload. |
| Required diagnostic | mmap-only microbaseline | Separates raw mmap-backed value access cost from VMemKV’s T1/T2 machinery. | Get hit/miss, scan, update, larger-than-memory. | Not a durability-matched competitor; label as diagnostic. |
| Required | VMemKV ablations | Shows whether T1/T2 split and optimizations matter. | AppendMap for point/update, Bloom for negative lookup, SIMD for scan, MemoryHints for larger-than-memory/reorg, Inline64 for small values. | Do not exhaustively run every combination. |
| Optional | LMDB | mmap-based KVS contrast. | Get/scan/larger-than-memory if time permits. | Useful if reviewers question mmap positioning; not central. |
| Optional | LevelDB | Historical LSM baseline and WiscKey baseline ancestor. | Small subset of YCSB/db_bench. | RocksDB is usually enough for main paper. |
| Optional | WiscKey implementation | Closest academic key-value separation paper. | YCSB with 1 KiB/16 KiB, range scan, GC/recovery. | Use only if reproducible; otherwise cite and compare conceptually. |
| Optional / low | Bitcask-like custom baseline | Isolates append-only data file + in-memory key directory. | Point read/write and merge/reorganization. | Custom baselines need careful fairness explanation. |
| Usually no | vmcache / LeanStore / Predictive Translation | DBMS buffer-manager competitors, not standalone KVS baselines. | None in main paper. | Keep in Related Work and responsibility table unless claims broaden. |

## Workload checklist derived from related work

| Workload / metric family | Needed in VMemKV? | Source motivation | Why it matters |
| --- | --- | --- | --- |
| YCSB load + A/B/C/E/F | Main | WiscKey, RocksDB-style KVS evaluation, FASTER/F2-style KVS comparison. | Gives a recognizable KVS macrobenchmark and answers “does it compete with RocksDB?” |
| 1 KiB and 16 KiB values | Main | WiscKey and BlobDB-style value separation. | Distinguishes ordinary values from large values where T2 separation should matter. |
| Larger-than-memory ratios, e.g. 4x/8x dataset-memory | Main | VMemKV thesis, mmap criticism, vmcache/LeanStore/F2. | Tests whether OS-delegated value residency works after page cache eviction begins. |
| Uniform and skewed access | Main | WiscKey, F2, buffer-manager papers. | Separates cold random-fault behavior from hot-set caching behavior. |
| Short scan / range scan | Main | Bigtable, WiscKey, Bitcask contrast, RocksDB. | Scan is where value separation and T2 layout may hurt; T1/T2 reorganization should help. |
| Write amplification and storage usage | Main | WiscKey, RocksDB, How to Write to SSDs. | VMemKV’s value separation and in-place update should be evaluated by write traffic, not only throughput. |
| Tail latency and time series | Main | mmap criticism, RocksDB operational evaluation. | OS page faults, dirty writeback, compaction, and reorganization can hide in averages. |
| Page faults, TLB misses/shootdowns, SSD bandwidth | Main | mmap criticism, vmcache, Predictive Translation. | Required to explain whether mmap/T2 behavior is the bottleneck. |
| Reorganization / GC / merge behavior | Main | WiscKey GC, Bitcask merge, RocksDB compaction. | VMemKV must show how it repairs ordering/storage fragmentation. |
| Recovery sanity check | Main but scoped | WiscKey recovery, RocksDB durability expectations. | Shows checkpoint + WAL replay path works; not a full crash-consistency proof. |
| TPC-C | No for main paper | vmcache, LeanStore, Predictive Translation, How to Write to SSDs. | VMemKV is scoped as standalone KVS without transactions; TPC-C would confuse scope unless the paper changes. |
| Production-trace evaluation | No / future work | RocksDB experience paper. | Useful but not necessary for first paper; harder to reproduce. |
| Distributed scaling | No | Bigtable. | VMemKV is not a distributed storage system. |

## Per-paper matrix

| Paper / system | Summary | Workloads and competitors used in that paper | Should VMemKV run that workload? | Should VMemKV use it as a direct rival? | Decision for this paper |
| --- | --- | --- | --- | --- | --- |
| Bigtable | Distributed sorted map using tablets, memtables, SSTables, commit logs, Bloom filters, caches, and compaction. Important as the LSM/SSTable lineage. | Random read, in-memory random read, random write, sequential read/write, scan; scaling by tablet server count; production deployment discussion. | Partly. Keep point read/write and scan concepts, but not distributed server scaling. | No. Bigtable is a distributed service, not a standalone embedded KVS baseline. | Related Work only. Use it to explain LSM/SSTable/compaction ancestry. |
| RocksDB | Production LSM-tree KVS with rich compaction, cache, Bloom filter, WAL, compression, and operational tuning. | Production workload observations, compaction-mode comparison, db_bench-style microbenchmarks, CPU/space/write amplification analysis. | Yes. Use YCSB plus write amplification, storage usage, tail latency, memory-budget fairness, and long-run compaction/reorganization behavior. | Yes. Primary baseline. | Required main baseline. Avoid claiming VMemKV must win every workload. |
| RocksDB BlobDB | RocksDB large-value separation using blob files. | Large-value write/read workloads and blob/value-separation tuning in RocksDB context. | Yes for 16 KiB or larger values. | Yes for large-value subset. | Required for fair large-value comparison, especially write amplification. |
| WiscKey | LSM stores key + value pointer; values go to append-only value log. Closest academic ancestor to VMemKV’s key/value separation. | LevelDB db_bench, sequential/random load, random lookup, range query with prefetch, value-log GC, crash recovery, YCSB A-F; compares LevelDB/RocksDB/WiscKey. | Yes. Borrow YCSB A-F subset, 1 KiB/16 KiB values, range scan, GC/reorganization, write amplification, and recovery sanity checks. | Usually no. Prefer BlobDB unless WiscKey implementation is reproducible. | Discuss as closest prior work; use BlobDB as practical value-separated LSM rival. |
| Bitcask | Append-only data files plus RAM-resident key directory. Simple point lookup and write path; merge reclaims obsolete entries. | Mostly design and preliminary observations: point read/write latency, writes/sec, larger-than-memory data volume, merge/hint-file discussion. | Partly. VMemKV should evaluate point operations and reorganization/merge-like reclamation. | No for main; optional custom baseline. | Related Work and responsibility table. Optional appendix if a fair Bitcask-like implementation is available. |
| LMDB | mmap-based B+tree-style embedded KVS. Useful mmap contrast. | Usually evaluated as an mmap storage engine in external comparisons such as vmcache-style studies. | Optional. Run Get/Scan/larger-than-memory only if time permits. | Optional. | Appendix baseline if reviewers need an mmap KVS competitor. Not central because VMemKV maps the value layer, not the whole B+tree. |
| Are You Sure You Want to Use MMAP? | Critique of mmap for DBMS/storage systems: eviction, page faults, dirty writeback, TLB shootdowns, page-cache control. | fio/O_DIRECT vs mmap; random reads, sequential scans, page-cache-limited larger-than-memory settings, SSD bandwidth scaling, TLB shootdowns. | Yes. VMemKV must measure page faults, TLB events, cgroup/page-cache pressure, SSD bandwidth, and time-series behavior. | No. It is not a KVS. | Use as motivation and as required instrumentation guidance. |
| vmcache | Virtual-memory-assisted DBMS buffer manager. Uses VM address translation while retaining DBMS control over page fault, eviction, and replacement. | In-memory and out-of-memory DBMS workloads; TPC-C-like and random-read workloads; compares LeanStore, WiredTiger, LMDB, and variants with exmap. | Use metrics, not workload wholesale. Page faults, TLB shootdowns, and scalability are relevant; TPC-C is not central. | No for main. | Related Work + responsibility table. Direct comparison only if the claim expands to buffer-manager designs. |
| LeanStore | Larger-than-memory storage engine using explicit buffer management and pointer swizzling. | DBMS/page-based workloads such as YCSB/TPC-C-style evaluations in the LeanStore line of work. | No for main, except as motivation for explicit-buffer-manager trade-offs. | No for main. | Related Work. Include in responsibility table under explicit buffer-pool/pointer-swizzling systems. |
| Predictive Translation / PrediCache | High-performance hash-table buffer manager using preferred-frame prediction to reduce translation overhead. | TPC-C, uniform random read, skewed YCSB, in-memory and out-of-memory conditions; compares LeanStore, vmcache, WiredTiger, LMDB, traditional buffer pool. | Partly. Skewed YCSB and microarchitectural metrics are useful; TPC-C is out of scope. | No. | Related Work. Use to justify measuring CPU/TLB overhead and to contrast explicit buffer management with OS delegation. |
| How to Write to SSDs | SSD-conscious DBMS write placement; separates DB write amplification and SSD write amplification. | YCSB-A and TPC-C; DB WAF, SSD WAF, total WAF, throughput, logical/physical writes across SSD types. | Yes for metrics. VMemKV should report logical bytes, engine-written bytes, device bytes, and storage usage. | No. | Use as measurement guidance for write amplification; not a KVS rival. |
| FASTER | Concurrent key-value store with hash index and hybrid log; supports in-place updates and read-modify-write. | Point operations, update-heavy/read-modify-write workloads, concurrency scaling, recovery-related measurements in the FASTER paper. | Optional. YCSB-F/read-modify-write is already useful; full FASTER comparison is not necessary unless concurrency becomes central. | Optional. | Related Work for hybrid log + in-place update. Direct benchmark only if easy to run and if paper claims high-concurrency KVS performance. |
| F2 | KVS for large, skewed, larger-than-memory workloads; separates hot and cold records and targets NVMe. | Larger-than-memory skewed point workloads; compares RocksDB, SplinterDB, KVell, LeanStore, FASTER; reports throughput under memory constraints and write amplification. | Partly. VMemKV should include skewed larger-than-memory workload because it directly tests the target regime. | Optional. | Related Work now; possible appendix rival if implementation is reproducible and framing shifts toward large-skewed KVS competition. |
| YCSB | Standard cloud-serving KVS benchmark suite. | Load plus A-F workloads: update-heavy, read-heavy, read-only, read-latest, short scan, read-modify-write. | Yes. | Not a rival. | Main macrobenchmark, but not sufficient alone. |
| LevelDB | Historical LSM KVS and WiscKey baseline ancestor. | db_bench/YCSB in many LSM papers. | Optional. | Optional. | Use only as historical/appendix baseline. RocksDB is the main practical LSM rival. |
| ScaleCache | Notion/GitHub entry exists, but current notes are empty and the exact paper role still needs verification. | TBD. | No until clarified. | No. | TODO: identify whether it is cache hierarchy, KVS, or DBMS buffer-management related. |
| LIPaH (Otaki) | Entry exists, but current notes are empty; likely index/prediction/buffer-management related from the reading-order note. | TBD. | No until clarified. | No. | TODO: fill after reading; probably Related Work only. |
| LLFREE | Entry exists, but current notes are empty; likely concurrency/lock-free related from the reading-order note. | TBD. | No unless VMemKV claims lock-free concurrency contribution. | No. | TODO: fill after reading; likely not evaluation-critical. |
| Aether / group commit references | Mentioned in the draft references as possible WAL/group-commit related work. | Durability/write latency/group commit style evaluation. | Only if WAL implementation becomes central. | No. | Optional citation for WAL optimization, not a workload baseline. |

## Main-paper recommendation

Use this main evaluation package:

1. **RocksDB comparison:** YCSB load + A/B/C/E/F, with 1 KiB and 16 KiB values, matched memory and durability, plus write amplification and storage usage. Include RocksDB BlobDB for large values.
2. **Not-just-YCSB system behavior:** larger-than-memory thread scaling, page-fault and bandwidth time series, TLB counters, and uniform vs skewed access.
3. **T1/T2 value of the design:** mmap-only microbaseline and VMemKV ablations.
4. **Maintenance behavior:** reorganization/checkpoint/recovery sanity checks, including foreground tail latency and bytes copied/reclaimed.
5. **Responsibility table:** compare VMemKV, RocksDB/LSM, WiscKey-like value log, Bitcask-like KVS, LMDB, and explicit buffer-pool engines.

This package answers the two reviewer-style concerns directly: YCSB is necessary but not sufficient for RocksDB; vmcache is important related work but not a required main rival.
