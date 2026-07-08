# VMemKV Evaluation Plan

あんまり具体的な内容は俺独断で決めない方が良さそう、中園さんと相談して決めた方が良さそう

あと実装方針とか実装も考慮しないといけん

このファイルは `evaluation_plan.md` と `additional_experiments.md` を統合した簡潔版である。
粒度は `additional_experiments.md` に合わせ、査読者が疑いそうな点を切り分ける評価計画としてまとめる。

## Summary

中心問いは次である。

> VMemKV は、OS に larger-than-memory 管理を委譲しつつ、RAM-resident T1 index と mmap-backed T2 value layer の責務分離によって、単純な実装と実用的な性能を両立できるか。

VMemKV が全 workload で最速である必要はない。
評価で示すべきことは、OS 委譲型設計が成立する条件、成立しにくい条件、T1/T2 分離が性能と実装責務に与える効果である。

## Scope

対象に含めるもの:

- Get / Insert / Update / Delete / Scan
- larger-than-memory workload
- T1/T2 separation
- reorganization
- checkpoint reload
- WAL replay recovery

対象外:

- transaction / SQL / phantom avoidance
- LineairDB / Kamo 側の concurrency control
- replication

本計画は、WAL / recovery / no-fork checkpoint が反映された最終状態を評価対象とする。

---

## Core Experiments


| Experiment                       | Summary                                                                       | Role                                            |
| -------------------------------- | ----------------------------------------------------------------------------- | ----------------------------------------------- |
| E0. Setup and fairness           | hardware、build、memory budget、durability、I/O policy など比較条件を統一して記録する            | memory budget、durability、I/O policy を揃える        |
| E1. Regime protocol              | Load/write、Clean read、Mixed を分け、T2 が dirty/private か clean file-backed かを明示する | dirty/private T2 と clean file-backed T2 を分ける    |
| E2. LTM behavior and scalability | residency、value size、access distribution、thread count を振って LTM 時の挙動を見る        | OS page cache / page fault / thread scaling を測る |
| E3. YCSB RocksDB / BlobDB        | YCSB A/B/C/E/F で VMemKV、RocksDB、BlobDB を同一条件で比較する                             | 標準 macrobenchmark と large-value baseline        |
| E4. mmap / pread / fio           | mmap-only、pread twin、fio を使い、raw access、mmap 差分、device limit を切り分ける           | mmap、T1/T2 layout、device limit を切り分ける           |
| E5. T1/T2 breakdown              | AppendMap、BloomFilter、SimdScan などを workload ごとに ablation する (今後の実装方針に合わせる)    | 各 optimization の効果を分ける                          |
| E6. Reorganization / checkpoint  | delete/update/scan workload で回収効果、foreground latency、memory spike を測る         | fragmentation repair と foreground cost を測る      |
| E7. Recovery                     | WAL size、checkpoint age、crash point を変えて復旧時間と整合性を検証する                         | checkpoint + WAL replay の成立を示す                  |
| E8. Responsibility table         | VMemKV と代表 engine の buffer pool、compaction、GC、recovery 責務を表で比較する              | 実装責務の少なさを比較する                                   |


---

## E0. Setup and Fairness

### Question

VMemKV と baselines を、公平な memory budget、durability scope、I/O policy で比較できているか。

### Add to Evaluation

最低限、以下を明記する。

- hardware, CPU core count, NUMA topology, CPU pinning
- DRAM size, SSD model, filesystem, mount options, kernel version
- compiler, build options, cgroup memory limit
- warmup, run duration, repetition count, error bars
- VMemKV: `MAP_PRIVATE` T2, WAL/checkpoint sync policy, checkpoint `fsync` policy
- RocksDB: block cache size, compression, WAL/sync policy, direct/buffered I/O
- BlobDB: blob settings when enabled

### Notes

VMemKV の T2 は read-side では OS page cache を使うが、`MAP_PRIVATE` の dirty pages は private COW pages になる。
process RSS だけで比較せず、cgroup memory usage / high-water mark も見る。

RocksDB の compression は基本 off とする。
有効にする場合は別条件として扱う。

---

## E1. **T2 State Evaluation Protocol**

### Question

VMemKV は、どの状態の T2 を測っているのか。

`MAP_PRIVATE` T2 では、load/update 直後の dirty pages は clean file-backed pages ではない。
この状態を larger-than-memory read として測ると、OS page cache ではなく anonymous memory / swap の挙動を測ってしまう。

### Add to Evaluation

少なくとも以下を分ける。


| Regime     | Description                                                | Role                          |
| ---------- | ---------------------------------------------------------- | ----------------------------- |
| Load/write | load/update 直後。dirty private COW pages が多い                 | write-heavy LTM の制約           |
| Clean read | reorganize/checkpoint/reopen 後。clean file-backed pages を読む | 本命の OS-managed read residency |
| Mixed      | clean generation に一部 update を加える                           | 長時間運用時の混在状態                   |


本命の larger-than-memory read は次の protocol で測る。

```text
load
reorganize or checkpoint generation creation
reopen/remap clean generation
drop page cache
measure read workload
```

### Metrics

- throughput, p50/p95/p99/p99.9 latency
- major/minor faults, RSS, cgroup memory, swap in/out
- SSD read/write bandwidth
- dirty/private page ratio, if available

---

## E2. Larger-than-Memory Behavior and Scalability

### Question

OS に委譲した value residency は、DRAM を超える dataset でどこまで機能するか。
thread count を増やしたとき、VMemKV はどこで頭打ちになるか。

### Add to Evaluation

DRAM-resident run と larger-than-memory run を分ける。
DRAM-resident run は T1/T2 実装コストを見るため、larger-than-memory run は page fault / eviction cost を見るために使う。
各 run は E1 の Load/write, Clean read, Mixed のいずれかとして明示的にラベル付けする。

変化させるもの:

- residency: DRAM-resident, dataset/memory ratio 4x, 8x
- value size: 1 KiB, 16 KiB
- access distribution: uniform, Zipf alpha 1.2
- thread count: 1, 4, 8, physical cores, logical cores

### Metrics

- throughput and tail latency
- major/minor faults per operation
- SSD bandwidth time-series
- CPU utilization, cycles/op, instructions/op
- dTLB misses/op, TLB shootdowns

平均値だけでなく時系列も残す。
特に throughput、p99 latency、major faults、SSD bandwidth を並べる。

---

## E3. YCSB RocksDB / BlobDB Comparison ←これ大事〜

### Question

VMemKV は、標準的な KVS macrobenchmark で RocksDB と比べて競争力があるか。
large value では value-separated RocksDB と比べても競争力があるか。

### Add to Evaluation

本編で使う workload:

- YCSB load phase
- Workload A: update-heavy
- Workload B: read-heavy
- Workload C: read-only
- Workload E: short scan
- Workload F: read-modify-write

Workload D は read-latest / insert-heavy の補助 workload として appendix に回す。

比較対象:

- VMemKV
- RocksDB
- RocksDB BlobDB, required for 16 KiB values
- LevelDB, optional

基本条件:

- value size: 1 KiB, 16 KiB
- thread count: 1, 4, 8, physical cores, logical cores
- VMemKV と RocksDB の memory budget を揃える
- durability scope を揃えた条件を最低 1 本作る

### BlobDB Notes

VMemKV は large values を T2 に分離するため、16 KiB value で vanilla RocksDB だけと比較すると不公平になりやすい。
BlobDB を入れることで、「value separation 自体」ではなく「OS-managed value residency + RAM-resident T1 split」の効果を問える。

BlobDB では、`enable_blob_files`、`min_blob_size`、blob file size、blob GC、compression、WAL/sync、block cache、I/O mode、memory budget を明記する。

### Metrics

- throughput, p50/p95/p99/p99.9 latency
- storage usage
- logical bytes written, WAL bytes written, device bytes written
- engine write amplification, device write amplification
- compaction / blob GC / reorganization foreground interference

`engine-written bytes` は、VMemKV では WAL、checkpoint output、reorganization output を含める。
T2 `MAP_PRIVATE` への append / overwrite は、必要なら private dirty bytes として別枠にする。

---

## E4. mmap / pread / fio Baselines

### Question

VMemKV の性能差は mmap によるものか、T1/T2 layout によるものか。
また、device limit に対して VMemKV はどの程度近いのか。

### Add to Evaluation

まず、raw mmap-backed value access の参照点として mmap-only microbaseline を置く。

- mmap-backed value file + in-memory `unordered_map<key, offset/length>`
- T1 sorted/append region なし
- T1/T2 reorganization なし
- checkpoint なし
- WAL recovery なし

次に、同じ T1、同じ T2 record layout、同じ offset を使い、value access だけを差し替える。


| Variant           | T1   | T2 layout | Value access             |
| ----------------- | ---- | --------- | ------------------------ |
| VMemKV mmap       | same | same      | mmap pointer dereference |
| VMemKV pread twin | same | same      | `pread` / `preadv`       |


必要なら `posix_fadvise` の有無も小さな ablation として測る。

さらに、同じ device に対して `fio` O_DIRECT の上限を測る。

- random 4 KiB read
- random value-size read
- sequential scan bandwidth
- queue depth sweep
- thread / job count sweep

### Interpretation


| Observation                                       | Possible interpretation                                                 |
| ------------------------------------------------- | ----------------------------------------------------------------------- |
| fio も遅い                                           | device / platform limit                                                 |
| fio は速いが mmap-only microbaseline が遅い              | mmap fault path / kernel bottleneck                                     |
| fio と VMemKV pread twin は速いが VMemKV mmap twin が遅い | mmap-specific bottleneck                                                |
| mmap-only microbaseline は速いが full VMemKV が遅い      | T1/T2 organization, reorganization, checkpoint, WAL recovery の overhead |


### Metrics

- throughput, latency, page faults
- syscall count
- IOPS, bandwidth, queue depth
- CPU utilization, cycles/op, instructions/op
- dTLB misses, TLB shootdowns

---

## E5. T1/T2 Design Breakdown

これ検討余地あり、オプションのオンオフはそんな難しくないはずなので色々やってみる

### Question

T1/T2 分離と各 optimization は、どの workload で効いているか。

### Add to Evaluation

すべての組み合わせは網羅しない。
各 optimization が効く workload だけを示す。


| Variant     | Main workload                       |
| ----------- | ----------------------------------- |
| baseline    | reference                           |
| all-on      | best VMemKV configuration           |
| AppendMap   | point lookup / update               |
| BloomFilter | negative lookup                     |
| SimdScan    | scan                                |
| MemoryHints | larger-than-memory / reorganization |
| Inline64    | small fixed-size value workload     |


### Metrics

- throughput and latency
- scan throughput, where relevant
- page faults, where relevant
- cycles/op, instructions/op, dTLB misses/op

---

## E6. Reorganization and Checkpoint Behavior

### Question

Reorganization / checkpoint reload は、ordering fragmentation と storage fragmentation を修復できるか。
その foreground cost と memory high-water mark はどの程度か。

### Add to Evaluation

workload:

- delete-heavy
- value-growth update-heavy
- scan before / during / after reorganization
- unreachable T2 record ratio: 0%, 25%, 50%, 75%
- long run where VMemKV checkpoint reload and RocksDB compaction each occur at least once

### Metrics

- scan throughput before / during / after
- T2 bytes before / after, unreachable bytes, copied bytes, reclaimed bytes
- reclaimed bytes / copied bytes
- reorganization duration
- foreground p99/p99.9 latency during reorganization
- checkpoint serialization time, generation switch time, WAL replay time
- private dirty page count, process RSS, checkpoint buffer bytes
- cgroup memory high-water mark, OOM event count
- major faults and TLB shootdowns

Reorganization は scan 高速化だけでなく、不要データの回収処理として評価する。
Checkpoint cost と recovery time の trade-off は E7 と合わせて解釈する。

---

## E7. Recovery and Crash-Consistency

WiscKey参考？ALICE←古いらしい  

主: RocksDB-style の独自 crash/recovery harness

補助: dm-log-writes または CrashMonkey

発展: mmap/MMIO 経路を強く主張するなら Pathfinder

比較文脈: WiscKey は ALICE を使っていた、と説明

### Question

Checkpoint + WAL replay で復旧できるか。
Checkpoint frequency は foreground cost と recovery time の間にどのような trade-off を作るか。

### Add to Evaluation

変化させるもの:

- WAL size since last checkpoint
- checkpoint age
- dataset size

crash point:

- after load
- during update-heavy workload
- during checkpoint reload
- during reorganization

本文では、checkpoint completion、commit marker or atomic rename、incomplete checkpoint handling、WAL replay LSN range を説明する。

### Metrics

- recovery time
- checkpoint load time
- WAL replay time
- replayed WAL bytes
- recovered key count
- checkpoint size, WAL size
- checksum / full scan result

この実験は大規模 crash testing ではなく、recovery path の sanity check として扱う。

---

## E8. Implementation Responsibility Table

### Question

VMemKV は、buffer pool、page replacement policy、multi-level compaction を自前で持たない単純な storage engine だと言えるか。

### Add to Evaluation

1 枚の比較表でよい。
benchmark section として大きく扱わない。

比較対象:

- VMemKV
- RocksDB / LSM
- RocksDB BlobDB
- WiscKey-like value-log
- Bitcask-like KVS
- LMDB / mmap B+tree
- vmcache-like system
- explicit buffer-pool engine

比較軸:

- user-space buffer pool
- page replacement policy
- multi-level compaction
- value-log GC
- mmap-backed value storage
- OS-managed value residency
- ordered scan support
- background repair / reorganization
- in-place update support
- recovery mechanism

---

## Appendix / Optional


| Experiment                        | Priority | Reason                                  |
| --------------------------------- | -------- | --------------------------------------- |
| YCSB Workload D                   | Medium   | read-latest / insert-heavy の補助 workload |
| full YCSB A-F summary             | Medium   | 標準 benchmark として有用だが本編には多い場合がある         |
| LMDB                              | Medium   | mmap-based KVS 代表                       |
| LevelDB                           | Medium   | 補助的な LSM baseline                       |
| full value-size sweep             | Medium   | 傾向確認用。主張には 1 KiB / 16 KiB で足りる          |
| latency CDF                       | Low      | p99 / p99.9 と時系列で代替可能                   |
| Bitcask-like baseline             | Low      | 自作 baseline の公平性説明が必要                   |
| append-update-only VMemKV variant | Low      | in-place update は中心主張ではない               |
| multiple storage tiers            | Low      | 実験環境の制約が大きい                             |


---

## Paper Outputs


| Output   | Content                                                 |
| -------- | ------------------------------------------------------- |
| Figure 1 | DRAM-resident vs larger-than-memory thread scalability  |
| Figure 2 | LTM time-series: throughput, p99, faults, SSD bandwidth |
| Figure 3 | YCSB VMemKV vs RocksDB / BlobDB                         |
| Figure 4 | mmap / pread / fio comparison                           |
| Figure 5 | CPU and TLB overhead                                    |
| Figure 6 | T1/T2 ablation                                          |
| Figure 7 | reorganization and checkpoint behavior                  |
| Figure 8 | recovery time vs WAL size or checkpoint age             |
| Table 1  | implementation responsibility comparison                |


---

## Negative Results

RocksDB が一部 workload で速い場合:
VMemKV は全 workload で勝つ必要はない。
target workload で競争力があり、得意条件と不得意条件を説明できればよい。

High thread count で頭打ちになる場合:
DRAM-resident run と larger-than-memory run の差分から、T1/T2 実装側の競合と OS page fault / eviction cost を分ける。

Very fast NVMe で mmap-backed T2 が不利な場合:
OS-managed value residency の有利不利は、storage latency と workload locality に依存する適用条件として扱う。

Uniform cold workload で page fault が支配的な場合:
想定内の negative result として扱い、平均値だけでなく時系列で初期状態と steady state を分ける。

mmap / pread microbaseline が VMemKV より速い場合:
microbaseline は durability を揃えた競合ではなく、raw value access の性能参照点である。
VMemKV が追加の overhead と引き換えに、T1/T2 organization、reorganization、checkpoint、WAL recovery を提供していることを説明する。

Checkpoint 中に memory spike が出る場合:
in-process checkpoint / reload の適用条件として扱い、cgroup memory high-water mark と OOM risk を明示する。

In-place update の効果が限定的な場合:
中心主張は崩れない。
in-place update は補助的な強みであり、中心は OS-managed larger-than-memory と T1/T2 separation である。

---

## Bottom Line

この評価で必ず押さえる点は次の 4 つである。

- Regime protocol で、dirty/private T2 と clean file-backed T2 を分ける。
- pread twin で、mmap の効果を T1/T2 layout から切り分ける。
- fio O_DIRECT bound で、device limit と kernel / mmap / VMemKV bottleneck を切り分ける。
- RocksDB BlobDB で、large-value comparison を公平にする。

