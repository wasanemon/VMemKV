# VMemKV 評価計画

このメモは、VMemKV 論文の evaluation section を設計するための簡潔な計画書です。
対象は `wasanemon/VMemKV` の `paper-writing` ブランチ、設計方針は `pr-14` を基準にします。

---

## 1. 評価の中心問い

VMemKV の評価で答えるべき中心問いは、次です。

> **VMemKV は、OS に larger-than-memory 管理を委譲しつつ、T1/T2 の責務分離によって、単純な実装と競争力のある性能を両立できるか。**

設計思想として、VMemKV は `mmap`, `fork`, `mincore`, `madvise` などの OS 仮想メモリ機構に I/O / residency 管理をできるだけ委譲する larger-than-memory KVS です。

論文では、VMemKV を以下のように位置づけます。

- RocksDB / LevelDB のような LSM-tree に比肩する性能を目指す。
- Bitcask のような単純な設計を目指す。
- LMDB のような mmap-based KVS より大規模な dataset を扱うことを目指す。
- buffer pool、page replacement policy、複雑な multi-level compaction を自前で持たない。

in-place update は重要な特徴ですが、評価の中心ではありません。中心は **OS-delegated larger-than-memory management** と **T1/T2 responsibility separation** です。in-place update は update-heavy workload における補助的な強みとして評価します。

---

## 2. 評価軸

### Q1. OS 委譲型 larger-than-memory 管理は機能するか？

T2 value region が DRAM を超える条件で、OS page cache / page fault / storage latency が性能にどう影響するかを測ります。

見るもの:

- dataset size / memory ratio
- page faults
- throughput
- tail latency
- SSD read/write bandwidth
- access skew の影響

---

### Q2. RocksDB / LevelDB に対して競争力があるか？

VMemKV の主 baseline は RocksDB です。可能なら LevelDB も追加します。

見る workload:

- load
- get hit / miss
- insert
- update
- delete
- scan
- read-heavy / write-heavy mixed workload

VMemKV が全 workload で勝つ必要はありません。重要なのは、想定 workload で競争力を示し、得意条件と不得意条件を明確にすることです。

---

### Q3. T1/T2 の責務分離は効いているか？

VMemKV の中核は、RAM 常駐 T1 index と mmap-backed T2 value region の分離です。

評価すること:

- T1 が hot metadata / logical control plane として機能するか。
- T2 が OS-managed large value region として機能するか。
- T1 optimizations が point lookup / scan にどう効くか。
- T2 access が page fault / SSD I/O とどう結びつくか。

---

### Q4. Reorganize / background jobs は fragmentation を管理できるか？

VMemKV は複雑な LSM compaction の代わりに、T1/T2 reorganization と background jobs で ordering fragmentation / storage fragmentation を修復します。

見るもの:

- scan before / after reorganize
- T2 bytes_used before / after reorganize
- bytes reclaimed
- reorganize duration
- foreground p99 latency during background work

---

### Q5. 実装責務は本当に少ないか？

「実装が簡単」という主張は主観ではなく、設計責務の比較表で示します。

比較対象:

- VMemKV
- RocksDB / LSM
- WiscKey-like value-log
- Bitcask-like KVS
- LMDB / mmap B+tree
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

---

### Q6. in-place update はどこで効くか？

in-place update は主役ではなく、補助評価として扱います。

見るもの:

- same-size update
- value-growth update
- update amplification
- append fallback rate
- T2 garbage bytes

比較対象:

- VMemKV normal
- VMemKV append-update baseline, if implemented
- RocksDB

---

## 3. 必須実験

### E1. Larger-than-memory sweep

目的:

- OS-delegated value residency の有効性を示す。

変化させるもの:

- dataset / memory ratio: 0.5x, 1x, 2x, 4x, 8x
- value size: 64 B, 1 KiB, 4 KiB, 16 KiB
- access distribution: uniform, Zipf alpha 1.0, 1.2, 1.5
- run condition: cold / warm / steady-state

指標:

- throughput
- p50 / p95 / p99 latency
- major/minor page faults
- SSD read/write bandwidth

---

### E2. RocksDB comparison

目的:

- LSM-tree baseline に対する競争力を示す。

workload:

- load
- get hit / miss
- insert
- update
- delete
- scan
- mixed read/write

指標:

- throughput
- latency
- bytes written, if available
- storage usage
- tail latency

注意:

- RocksDB options, compression, WAL/sync policy を必ず明記する。
- VMemKV の durability scope と揃わない場合は、その差を明記する。

---

### E3. T1/T2 ablation

目的:

- VMemKV の設計要素ごとの効果を示す。

使う variant:

- baseline
- all-on
- cumulative T1 optimizations
- subtractive ablations
- inline variants

見るもの:

- AppendMap: point lookup / update
- BloomFilter: negative lookup
- SimdScan: scan
- MemoryHints: larger-than-memory / reorganization
- InlineShort / Inline8B: tiny value workload

---

### E4. Reorganization effect

目的:

- reorganize が ordering / storage fragmentation を修復することを示す。

workload:

- insert-heavy
- delete-heavy
- value-growth update-heavy
- scan before / after reorganize

指標:

- scan throughput before / after
- T2 bytes_used before / after
- bytes copied
- bytes reclaimed
- reorganize duration
- foreground p99 latency during reorganization

---

### E5. Implementation responsibility table

目的:

- VMemKV が buffer pool や複雑な compaction を持たない単純な storage engine であることを示す。

形式:

- 1 枚の比較表でよい。
- 大きな benchmark section にしない。

---

## 4. 推奨実験

時間があれば以下を追加します。

### E6. Simple mmap KVS baseline

目的:

- VMemKV が単なる mmap file access ではなく、T1/T2 logical control を持つ設計であることを示す。

候補:

- mmap-backed value file + simple unordered_map index
- T1/T2 reorganize なし

---

### E7. in-place update behavior

目的:

- VMemKV の update path 上の強みを補助的に示す。

workload:

- same-size update
- shrink update
- value-growth update

指標:

- update throughput
- update latency
- in-place update rate
- append fallback rate
- T2 bytes appended
- T2 bytes overwritten
- T2 garbage bytes

---

### E8. LMDB / LevelDB / Bitcask-like baseline

optional baseline です。

- LevelDB: classic LSM baseline
- LMDB: mmap-based KVS baseline
- Bitcask-like: simple append-only KVS baseline

入れられれば有用ですが、RocksDB comparison と LTM sweep より優先度は低いです。

---

## 5. 必要な指標

最低限、benchmark output には以下を含めたいです。

### Performance

- operations/sec
- p50 / p95 / p99 latency
- latency CDF, if possible

### Storage

- T2 bytes_used
- T2 bytes appended
- T2 bytes overwritten in place, if measured
- T2 unreachable bytes estimate
- bytes reclaimed by reorganization

### OS / Hardware

- major page faults
- minor page faults
- RSS
- SSD read/write bandwidth
- CPU cycles / instructions, if available
- dTLB / LLC misses, if available

### Reorganization

- reorganize count
- reorganize duration
- bytes copied
- bytes reclaimed
- foreground p99 latency during reorganize

---

## 6. 実装担当者向け checklist

- [ ] benchmark output を CSV または JSON にする。
- [ ] run ごとに dataset size, value size, thread count, workload mix を記録する。
- [ ] page faults と RSS を記録する。
- [ ] T2 bytes_used / bytes_appended を記録する。
- [ ] T1 hit/miss breakdown を記録する。
- [ ] reorganization duration / copied bytes / reclaimed bytes を記録する。
- [ ] dataset / memory ratio sweep を実行できるようにする。
- [ ] value size sweep を実行できるようにする。
- [ ] mixed read/write ratio を configurable にする。
- [ ] RocksDB options と sync policy を文書化する。
- [ ] VMemKV variants を benchmark output で明確に識別する。
- [ ] 可能なら simple mmap KVS baseline を追加する。
- [ ] 可能なら append-update-only VMemKV variant を追加する。

---

## 7. Evaluation section の構成案

```text
8. Evaluation
   8.1 Experimental Setup
       Hardware, OS, filesystem, storage, memory limit, compiler, RocksDB configuration.

   8.2 Larger-than-Memory Behavior
       Dataset/memory ratio, page faults, value size, skew, throughput, tail latency.

   8.3 Comparison with LSM-tree Baselines
       VMemKV vs RocksDB/LevelDB for load, get, update, delete, scan, mixed workloads.

   8.4 T1/T2 Design Breakdown
       VMemKV variants, T1 optimizations, T2 access behavior, inline values.

   8.5 Reorganization and Background Jobs
       Scan before/after reorg, storage reclaim, foreground latency during background work.

   8.6 Update Path Behavior
       Stable-size update, value-growth update, in-place vs append-update baseline.
       This section supports, but does not dominate, the paper claim.

   8.7 Implementation Simplicity
       Table comparing DB-side responsibilities across VMemKV, LSM, value-log, mmap-based, and buffer-pool designs.
```

論文を短くする場合は、8.6 を 8.3 または 8.4 に統合します。

---

## 8. 論文に載せたい図・表

### Figure 1. Larger-than-memory throughput and tail latency

- x-axis: dataset / memory ratio
- y-axis: throughput and p99 latency
- series: VMemKV, RocksDB, optional mmap-only / LMDB

### Figure 2. Page fault behavior

- x-axis: dataset / memory ratio or Zipf alpha
- y-axis: major/minor faults per operation

### Figure 3. VMemKV vs RocksDB workload comparison

- workload: load, get, update, delete, scan, mixed
- y-axis: throughput or normalized throughput

### Figure 4. T1/T2 ablation

- variants: baseline, AppendMap, BloomFilter, SimdScan, MemoryHints, Inline variants, all-on
- show only relevant operations for each optimization

### Figure 5. Reorganization effect

- scan throughput before/after
- T2 bytes_used before/after
- bytes reclaimed
- foreground p99 during background work

### Figure 6. Update path behavior

- stable-size vs value-growth update
- VMemKV normal vs append-update baseline
- update amplification / append fallback rate

### Table 1. Implementation responsibility

VMemKV, RocksDB/LSM, WiscKey-like value-log, Bitcask-like KVS, LMDB, explicit buffer-pool engine を比較する。

---

## 9. Negative results の扱い

### RocksDB が一部 workload で速い場合

問題ありません。VMemKV はすべての workload で勝つ必要はありません。

重要なのは以下です。

- target workload で競争力があること
- LTM 条件で OS-delegated value management が破綻しないこと
- 実装責務が少ないこと
- T1/T2 の責務分離が測定可能な効果を持つこと

### very fast NVMe で mmap-backed T2 が不利な場合

scope condition として扱います。OS-delegated residency の有利不利は、storage latency と workload locality に依存します。

### uniform cold workload で page fault が支配的になる場合

想定内です。VMemKV の得意条件と不得意条件を明確にする結果として扱います。

### in-place update の効果が限定的な場合

中心主張は崩れません。in-place update は補助的な強みであり、主張の中心は OS-delegated LTM management と T1/T2 responsibility separation です。

---

## 10. 評価の一文目標

> VMemKV が、単なる mmap file access ではなく、RAM-resident T1 index と mmap-backed T2 value layer の責務分離によって、OS に larger-than-memory 管理を委譲しながら、単純な実装と実用的な性能を両立できることを示す。

in-place update は、この主張を補強する update-path 上の特徴として評価する。
