# VMemKV 評価計画

このメモは、VMemKV 論文の evaluation section を設計するための最終的な評価計画です。
対象は `wasanemon/VMemKV` の `paper-writing` ブランチ、設計方針は `pr-14` を基準にします。

---

## 1. 評価の中心問い

VMemKV の評価で答えるべき中心問いは、次です。

> **VMemKV は、OS に larger-than-memory 管理を委譲しつつ、T1/T2 の責務分離によって、単純な実装と実用的な性能を両立できるか。**

VMemKV は `mmap`, `fork`, `mincore`, `madvise` などの OS 仮想メモリ機構に、value 領域の I/O と page residency 管理をできるだけ委譲する larger-than-memory KVS です。

論文では、VMemKV を以下のように位置づけます。

- RocksDB / LevelDB のような LSM-tree 型 KVS を主な性能比較対象にする。
- Bitcask のような単純な log + in-memory index 系 KVS を設計上の比較対象にする。
- LMDB のように mmap を使う既存 KVS とは異なり、RAM 常駐の T1 index と mmap-backed な T2 value 領域を分離する。
- buffer pool、page replacement policy、multi-level compaction を自前で実装しない。

重要なのは、VMemKV がすべての workload で最速であることを示すことではありません。評価では、OS 委譲型の設計が成立する条件、成立しにくい条件、T1/T2 分離が性能と単純さに与える効果を明確にします。

in-place update は重要な特徴ですが、評価の中心ではありません。中心は **OS に委譲した larger-than-memory 管理** と **T1/T2 の責務分離** です。in-place update は update-heavy workload における補助的な強みとして扱います。

---

## 2. Scope

本稿の評価対象は standalone KVS としての VMemKV です。

対象に含めるもの:

- Get / Insert / Update / Delete / Scan
- larger-than-memory workload
- T1/T2 分離
- reorganization
- checkpoint reload
- WAL replay による recovery

対象に含めないもの:

- 複数操作をまとめた transaction
- SQL layer
- phantom 回避
- LineairDB / Kamo 側の concurrency control
- replication

---

## 3. 評価軸

### Q1. OS 委譲型 larger-than-memory 管理は機能するか？

T2 value region が DRAM を超える条件で、OS page cache / page fault / storage latency が性能にどう影響するかを測ります。

見るもの:

- dataset size / memory ratio
- page faults
- throughput
- tail latency
- SSD read/write bandwidth
- access skew
- page cache が満杯になり、OS がページを追い出し始める前後の性能変化
- thread count を増やしたときの scalability

### Q2. RocksDB に対して競争力があるか？

VMemKV の主 baseline は RocksDB です。可能なら RocksDB BlobDB と LevelDB も追加します。

見る workload:

- load
- read-heavy
- update-heavy
- scan-heavy
- get hit / miss
- YCSB系

VMemKV が全 workload で勝つ必要はありません。重要なのは、target workload で競争力を示し、得意条件と不得意条件を明確にすることです。

### Q3. T1/T2 の責務分離は効いているか？

VMemKV の中核は、RAM 常駐 T1 index と mmap-backed T2 value region の分離です。

評価すること:

- T1 が頻繁に参照される metadata と検索・scan の制御を担うか。
- T2 が OS の page cache で管理される大容量 value 領域として機能するか。
- T1 optimizations が point lookup / scan にどう効くか。
- T2 access が page fault / SSD I/O とどう結びつくか。

### Q4. Reorganization / checkpoint reload は有効か？

VMemKV は複雑な LSM compaction の代わりに、T1/T2 reorganization と checkpoint reload で ordering fragmentation / storage fragmentation を修復します。

見るもの:

- scan before / during / after reorganization
- T2 bytes_used before / after reorganization
- T1 から参照されない T2 bytes
- bytes copied
- bytes reclaimed
- foreground p99 latency during reorganization
- fork time
- stop-the-world time
- WAL replay time

### Q5. Durability / recovery は成立するか？

VMemKV は WAL と checkpoint による durability を持つ設計です。評価では、crash 後に checkpoint + WAL replay で復旧できることを確認します。

見るもの:

- recovery time
- replayed WAL bytes
- recovered key count
- checksum / full scan による整合性確認

### Q6. 実装責務は本当に少ないか？

「実装が簡単」という主張は主観ではなく、設計責務の比較表で示します。

比較対象:

- VMemKV
- RocksDB / LSM
- WiscKey-like value-log
- Bitcask-like KVS
- LMDB / mmap B+tree
- explicit buffer-pool engine

---

## 4. 本編に残す必須実験

### E0. Experimental setup and fairness

目的:

- すべての比較の前提を明確にする。
- VMemKV と baselines の memory budget / durability / I/O policy をできるだけ揃える。

記録するもの:

- hardware
- CPU core count
- DRAM size
- SSD model
- filesystem and mount options
- OS kernel version
- compiler and build options
- cgroup memory limit
- RocksDB options
- RocksDB block cache size
- RocksDB I/O mode
- RocksDB compression setting
- RocksDB WAL/sync policy
- VMemKV durability scope
- warmup duration
- run duration
- repetition count
- error bars or standard deviation

注意:

- VMemKV は OS page cache を使うため、プロセス RSS だけでは memory budget を比較できない。
- cgroup などで総物理メモリを制限し、VMemKV と RocksDB を同じ budget で比較する。
- RocksDB の buffered I/O / direct I/O はどちらかに固定し、設定を明記する。
- compression は無効を基本とする。有効にする場合は別条件として扱う。

---

### E1. Larger-than-memory behavior and thread scalability

目的:

- OS に委譲した value 常駐管理の有効性と限界を示す。
- page cache が満杯になり、OS がページを追い出し始める前後で性能がどう変化するかを示す。
- thread count を増やしたときに throughput と tail latency がどう変化するかを示す。

本編で変化させるもの:

- dataset / memory ratio: 1x, 4x, 8x
- value size: 1 KiB, 16 KiB
- access distribution: uniform, Zipf alpha 1.2
- run condition: warm, steady-state
- thread count: 1, 8, physical cores, logical cores

指標:

- throughput
- p50 / p95 / p99 latency
- major/minor page faults
- page faults per operation
- SSD read/write bandwidth
- CPU utilization
- dTLB misses, if available
- throughput, p99 latency, major faults, SSD bandwidth の時系列

この実験では、run 全体の平均値だけでなく時系列も記録します。平均値だけでは、page cache が空いている初期状態と、ページの追い出しが続く状態を区別できないためです。

---

### E2. RocksDB comparison

目的:

- LSM-tree baseline に対する競争力を示す。
- value size と書き込み増幅が性能差にどう影響するかを示す。
- RocksDB と VMemKV の thread scaling を比較する。

本編で使う workload:

- load
- read-heavy
- update-heavy
- scan-heavy
- get hit / miss

value size:

- 1 KiB
- 16 KiB

thread count:

- 1
- 8
- それ以上
- physical cores
- logical cores

指標:

- throughput
- p50 / p95 / p99 latency
- logical bytes written
- WAL bytes written
- device bytes written
- write amplification: device bytes written / logical bytes written
- storage usage

注意:

- RocksDB options, compression, WAL/sync policy を必ず明記する。
- VMemKV と RocksDB の durability scope を揃えた条件を最低 1 本作る。
- durability scope が揃わない追加条件は、主結果ではなく補助結果として扱う。
- 可能なら large-value workload で RocksDB BlobDB も測る。

---

### E3. T1/T2 design breakdown

目的:

- VMemKV の設計要素ごとの効果を示す。
- T1/T2 分離が、単なる mmap file access では説明できない性能差を作るかを示す。

使う variant:

- baseline
- all-on
- AppendMap
- BloomFilter
- SimdScan
- MemoryHints
- Inline64

見るもの:

- AppendMap: point lookup / update
- BloomFilter: negative lookup
- SimdScan: scan
- MemoryHints: larger-than-memory / reorganization
- Inline64: small fixed-size value workload

注意:

- すべての opt-in の組み合わせを網羅しない。
- 本編では、各 optimization が効く workload だけを示す。
- 既に効果が小さいと分かっている option は Appendix か省略に回す。

---

### E4. Reorganization and checkpoint behavior

目的:

- reorganization が ordering fragmentation / storage fragmentation を修復することを示す。
- update/delete によって発生した不要な T2 record をどれだけ回収できるかを示す。
- checkpoint reload の停止時間と replay cost を測る。

workload:

- delete-heavy
- value-growth update-heavy
- scan before / during / after reorganization
- T1 から参照されない T2 record の割合: 0%, 25%, 50%, 75%

指標:

- scan throughput before / during / after
- T2 bytes_used before / after
- T1 から参照されない T2 bytes
- bytes copied
- bytes reclaimed
- reclaimed bytes / copied bytes
- reorganization duration
- foreground p99 latency during reorganization
- major page faults per operation before / after reorganization
- fork time
- stop-the-world time
- WAL replay time
- CoW page faults, if available

この実験では、reorganization を単なる scan 高速化ではなく、不要データの回収処理としても評価します。

---

### E5. Simple mmap KVS baseline

目的:

- VMemKV が単なる mmap file access ではなく、T1/T2 による検索・scan・reorganization の制御を持つ設計であることを示す。

baseline:

- mmap-backed value file + simple unordered_map index
- T1 sorted_region / append_region なし
- T1/T2 reorganization なし
- durability scope は VMemKV とできるだけ揃える。揃わない場合は差を明記する。

workload:

- get hit / miss
- update
- scan
- larger-than-memory workload
- value-growth update-heavy workload

指標:

- throughput
- p50 / p95 / p99 latency
- major/minor page faults
- SSD read/write bandwidth
- storage usage

この baseline は、VMemKV の比較対象を増やすためではなく、mmap だけでは説明できない部分を切り分けるために使います。

---

### E6. Recovery and crash-consistency sanity check

目的:

- checkpoint + WAL replay で復旧できることを示す。
- RocksDB comparison の durability scope を明確にする。

crash point:

- load 後
- update-heavy workload 実行中
- checkpoint reload 中
- reorganization 中

指標:

- recovery time
- replayed WAL bytes
- recovered key count
- checkpoint size
- WAL size
- checksum / full scan による整合性確認

この実験は大規模な crash testing ではなく、recovery path が機能することを示す sanity check として扱います。

---

### E7. Implementation responsibility table

目的:

- VMemKV が buffer pool や複雑な compaction を持たない単純な storage engine であることを示す。

形式:

- 1 枚の比較表でよい。
- benchmark section として大きく扱わない。

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

## 5. Appendix / optional に回す実験

本編では扱わず、余力がある場合だけ追加します。

- RocksDB BlobDB
- LMDB
- LevelDB
- full YCSB A-F
- full value-size sweep: 64 B, 4 KiB など
- latency CDF
- Bitcask-like baseline
- append-update-only VMemKV variant
- multiple storage tiers

優先度:

| 実験 | 優先度 | 理由 |
| --- | --- | --- |
| RocksDB BlobDB | 高 | large-value workload の直接対抗になる |
| LMDB | 中 | mmap-based KVS 代表として有用 |
| full YCSB A-F | 中 | 再現性は上がるが本編には多い |
| full value-size sweep | 中 | 傾向確認用。主張には 1 KiB / 16 KiB で足りる |
| latency CDF | 低 | p99 と時系列で代替可能 |
| Bitcask-like baseline | 低 | 自作 baseline の公平性説明が必要 |
| append-update-only variant | 低 | in-place update は中心主張ではない |
| multiple storage tiers | 低 | 有用だが実験環境の制約が大きい |

---

## 6. 必要な指標

### Performance

- operations/sec
- p50 / p95 / p99 latency
- throughput time-series
- p99 latency time-series

### Storage

- logical bytes written
- WAL bytes written
- device bytes written
- write amplification
- storage usage
- T2 bytes_used
- T2 bytes appended
- T2 bytes overwritten in place, if measured
- T1 から参照されない T2 bytes
- bytes reclaimed by reorganization

### OS / Hardware

- major page faults
- minor page faults
- page faults per operation
- RSS
- cgroup memory usage
- SSD read/write bandwidth
- SSD read/write bandwidth time-series
- CPU utilization
- dTLB misses, if available
- LLC misses, if available

### Reorganization / checkpoint

- reorganization count
- reorganization duration
- bytes copied
- bytes reclaimed
- reclaimed bytes / copied bytes
- foreground p99 latency during reorganization
- scan throughput before / during / after reorganization
- fork time
- stop-the-world time
- WAL replay time
- CoW page faults, if available

### Recovery

- recovery time
- replayed WAL bytes
- recovered key count
- checksum / full scan result

---
## 9. Evaluation section の構成案

```text
8. Evaluation
   8.1 Experimental Setup
       Hardware, filesystem, SSD, cgroup memory limit, RocksDB options,
       VMemKV durability scope, thread counts, warmup, repetitions.

   8.2 Larger-than-Memory Behavior
       Dataset/memory ratio, page faults, value size, skew,
       thread scaling, throughput/p99/faults/bandwidth time-series.

   8.3 Comparison with RocksDB
       Load, read-heavy, update-heavy, scan-heavy workloads.
       1 KiB and 16 KiB values.
       Throughput, tail latency, storage usage, write amplification.

   8.4 T1/T2 Design Breakdown
       AppendMap, BloomFilter, SimdScan, MemoryHints, Inline64.
       Show only workloads where each optimization should matter.

   8.5 Reorganization and Checkpoint Behavior
       Scan before/during/after, T2 unreachable bytes,
       copied bytes, reclaimed bytes, fork time, stop-the-world time,
       WAL replay time.

   8.6 Simple mmap Baseline
       mmap-backed value file + unordered_map index.
       Used to show that VMemKV is not just mmap file access.

   8.7 Recovery and Crash-Consistency
       Crash during load/update/checkpoint/reorganization.
       Recovery time, WAL replay bytes, recovered key count, checksum.

   8.8 Implementation Responsibility
       One table comparing DB-side responsibilities.
```

---

## 8. 論文に載せたい図・表

### Figure 1. Larger-than-memory throughput and tail latency

- x-axis: dataset / memory ratio
- y-axis: throughput and p99 latency
- series: VMemKV, RocksDB, simple mmap

### Figure 2. Larger-than-memory time-series behavior

- x-axis: elapsed time
- y-axis: throughput, p99 latency, major faults, SSD bandwidth

### Figure 3. Thread scalability

- x-axis: thread count
- y-axis: throughput and p99 latency
- series: VMemKV, RocksDB, simple mmap

### Figure 4. VMemKV vs RocksDB workload comparison

- workload: load, read-heavy, update-heavy, scan-heavy
- value size: 1 KiB and 16 KiB
- y-axis: throughput, p99 latency, write amplification

### Figure 5. T1/T2 ablation

- variants: baseline, AppendMap, BloomFilter, SimdScan, MemoryHints, Inline64, all-on
- show only relevant operations for each optimization

### Figure 6. Reorganization and checkpoint behavior

- scan throughput before / during / after
- T2 bytes_used before / after
- T1 から参照されない T2 bytes
- bytes copied and bytes reclaimed
- fork time / stop-the-world time / WAL replay time

### Figure 7. Recovery behavior

- x-axis: dataset size or WAL size
- y-axis: recovery time
- include recovered key count or checksum result in the caption/table

### Table 1. Implementation responsibility

VMemKV, RocksDB/LSM, WiscKey-like value-log, Bitcask-like KVS, LMDB, explicit buffer-pool engine を比較する。

---

## 9. Negative results の扱い

### RocksDB が一部 workload で速い場合

問題ありません。VMemKV はすべての workload で勝つ必要はありません。

重要なのは以下です。

- target workload で競争力があること
- DRAM より大きい dataset 条件で、OS に委譲した value residency が破綻しないこと
- 実装責務が少ないこと
- T1/T2 の責務分離が測定可能な効果を持つこと

### high thread count で性能が頭打ちになる場合

適用条件として扱います。OS 委譲型の mmap design は page fault / eviction / page table 操作の影響を受けるため、高並行で不利になる可能性があります。この結果は、VMemKV の限界を示す重要な情報です。

### very fast NVMe で mmap-backed T2 が不利な場合

適用条件として扱います。OS に委譲した value 常駐管理の有利不利は、storage latency と workload locality に依存します。

### uniform cold workload で page fault が支配的になる場合

想定内です。VMemKV の得意条件と不得意条件を明確にする結果として扱います。平均値だけでなく時系列を見ることで、初期状態と steady state を分けて説明します。

### simple mmap baseline が一部 workload で速い場合

問題ありません。その場合は、VMemKV の T1/T2 分離、reorganization、memory hints の効果が出る条件と出ない条件を明確にします。

### in-place update の効果が限定的な場合

中心主張は崩れません。in-place update は補助的な強みであり、主張の中心は OS に委譲した larger-than-memory 管理と T1/T2 の責務分離です。

---

## 10. 評価の一文目標

> VMemKV が、単なる mmap file access ではなく、RAM-resident T1 index と mmap-backed T2 value layer の責務分離によって、OS に larger-than-memory 管理を委譲しながら、単純な実装と実用的な性能を両立できる条件を示す。

あわせて、VMemKV が不利になる条件も明確にし、OS 委譲型設計の適用範囲を示す。
