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

in-place update は重要な特徴ですが、評価の中心ではありません。中心は **OS に委譲した larger-than-memory 管理** と **T1/T2 の責務分離** です。in-place update は update-heavy workload における補助的な強みとして評価します。

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
- page cache が満杯になり、OS がページを追い出し始める前後の性能変化

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
- YCSB A-F に相当する read/write/scan mix

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

- scan before / during / after reorganize
- T2 bytes_used before / after reorganize
- T1 から参照されない T2 bytes
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
- page cache が満杯になり、OS がページを追い出し始める前後で性能が大きく崩れないかを確認する。

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
- throughput, p99 latency, major faults, SSD bandwidth の時系列

この実験では、run 全体の平均値だけでなく、時間経過も記録します。平均値だけでは、page cache が空いている初期状態と、ページの追い出しが続く状態を区別できないためです。

---

### E2. RocksDB comparison

目的:

- LSM-tree baseline に対する競争力を示す。
- value size と write amplification が性能差にどう影響するかを示す。

workload:

- load
- get hit / miss
- insert
- update
- delete
- scan
- mixed read/write
- YCSB A-F に相当する workload

value size:

- 1 KiB
- 16 KiB
- 可能なら 64 B と 4 KiB も追加する。

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
- VMemKV の durability scope と揃わない場合は、その差を明記する。
- compression は無効を基本とし、有効にする場合は別条件として扱う。

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
- update/delete によって発生した不要な T2 record をどれだけ回収できるかを示す。
- reorganize 中に foreground workload がどれだけ影響を受けるかを測る。

workload:

- insert-heavy
- delete-heavy
- same-size update-heavy
- value-growth update-heavy
- scan before / during / after reorganize
- T1 から参照されない T2 record の割合: 0%, 25%, 50%, 75%

指標:

- scan throughput before / during / after
- T2 bytes_used before / after
- T1 から参照されない T2 bytes
- bytes copied
- bytes reclaimed
- reclaimed bytes / copied bytes
- reorganize duration
- foreground p99 latency during reorganization
- major page faults per operation before / after reorganize

この実験では、reorganize を単なる scan 高速化ではなく、不要データの回収処理としても評価します。VMemKV では、delete や value-growth update によって T1 から参照されない T2 record が残るため、reorganize がその領域を回収できることを示す必要があります。

---

### E5. Simple mmap KVS baseline

目的:

- VMemKV が単なる mmap file access ではなく、T1/T2 logical control と reorganize を持つ設計であることを示す。

baseline:

- mmap-backed value file + simple unordered_map index
- T1 sorted_region / append_region なし
- T1/T2 reorganize なし
- durability scope は VMemKV とできるだけ揃える。揃わない場合は差を明記する。

workload:

- get hit / miss
- update
- scan
- larger-than-memory sweep
- value-growth update-heavy

指標:

- throughput
- p50 / p95 / p99 latency
- major/minor page faults
- SSD read/write bandwidth
- storage usage

この baseline は、VMemKV の比較対象を増やすためではなく、mmap だけでは説明できない部分を切り分けるために使います。VMemKV が優位な場合は、T1/T2 分離、reorganize、memory hints のどれが効いているかを E3 と合わせて説明します。

---

### E6. Implementation responsibility table

目的:

- VMemKV が buffer pool や複雑な compaction を持たない単純な storage engine であることを示す。

形式:

- 1 枚の比較表でよい。
- 大きな benchmark section にしない。

---

## 4. 推奨実験

時間があれば以下を追加します。

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

### E8. LMDB / Bitcask-like baseline

optional baseline です。

- LMDB: mmap-based KVS baseline
- Bitcask-like: simple append-only KVS baseline

入れられれば有用ですが、RocksDB comparison、larger-than-memory sweep、simple mmap baseline より優先度は低いです。LevelDB は E2 で扱える場合に追加します。

---

## 5. 必要な指標

最低限、benchmark output には以下を含めたいです。

### Performance

- operations/sec
- p50 / p95 / p99 latency
- latency CDF, if possible
- long run における operations/sec と p99 latency の時系列

### Storage

- logical bytes written
- WAL bytes written
- device bytes written
- write amplification: device bytes written / logical bytes written
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
- SSD read/write bandwidth
- SSD read/write bandwidth の時系列
- CPU cycles / instructions, if available
- dTLB / LLC misses, if available

### Reorganization

- reorganize count
- reorganize duration
- bytes copied
- bytes reclaimed
- reclaimed bytes / copied bytes
- foreground p99 latency during reorganize
- scan throughput before / during / after reorganize

---

## 6. 実装担当者向け checklist

- [ ] benchmark output を CSV または JSON にする。
- [ ] run ごとに dataset size, value size, thread count, workload mix を記録する。
- [ ] page faults と RSS を記録する。
- [ ] T2 bytes_used / bytes_appended を記録する。
- [ ] T1 から参照されない T2 bytes を記録する。
- [ ] logical bytes written / WAL bytes written / device bytes written を記録する。
- [ ] write amplification を計算できるようにする。
- [ ] T1 hit/miss breakdown を記録する。
- [ ] reorganization duration / copied bytes / reclaimed bytes を記録する。
- [ ] reorganization before / during / after の foreground latency を記録する。
- [ ] dataset / memory ratio sweep を実行できるようにする。
- [ ] value size sweep を実行できるようにする。
- [ ] 1 KiB / 16 KiB value で RocksDB comparison を実行できるようにする。
- [ ] YCSB A-F に相当する workload mix を実行できるようにする。
- [ ] mixed read/write ratio を configurable にする。
- [ ] long run の throughput / p99 / page faults / SSD bandwidth を時系列で出力する。
- [ ] RocksDB options と sync policy を文書化する。
- [ ] VMemKV variants を benchmark output で明確に識別する。
- [ ] simple mmap KVS baseline を追加する。
- [ ] 可能なら append-update-only VMemKV variant を追加する。

---

## 7. Evaluation section の構成案

```text
8. Evaluation
   8.1 Experimental Setup
       Hardware, OS, filesystem, storage, memory limit, compiler, RocksDB configuration.

   8.2 Larger-than-Memory Behavior
       Dataset/memory ratio, page faults, value size, skew, throughput, tail latency,
       time-series behavior, and simple mmap baseline.

   8.3 Comparison with LSM-tree Baselines
       VMemKV vs RocksDB/LevelDB for load, get, update, delete, scan, mixed workloads,
       YCSB A-F style workloads, value-size sensitivity, and write amplification.

   8.4 T1/T2 Design Breakdown
       VMemKV variants, T1 optimizations, T2 access behavior, inline values.

   8.5 Reorganization and Background Jobs
       Scan before/during/after reorg, storage reclaim, copied bytes, foreground latency.

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
- series: VMemKV, RocksDB, simple mmap, optional LMDB

### Figure 2. Larger-than-memory time-series behavior

- x-axis: elapsed time
- y-axis: throughput, p99 latency, major faults, SSD bandwidth
- show cold start, warm state, and steady state separately if needed

### Figure 3. VMemKV vs RocksDB workload comparison

- workload: load, YCSB A-F style workloads, get, update, delete, scan, mixed
- value size: 1 KiB and 16 KiB
- y-axis: throughput, p99 latency, write amplification

### Figure 4. T1/T2 ablation

- variants: baseline, AppendMap, BloomFilter, SimdScan, MemoryHints, Inline variants, all-on
- show only relevant operations for each optimization

### Figure 5. Reorganization effect

- scan throughput before / during / after
- T2 bytes_used before / after
- T1 から参照されない T2 bytes
- bytes copied and bytes reclaimed
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
- DRAM より大きい dataset 条件で、OS に委譲した value residency が破綻しないこと
- 実装責務が少ないこと
- T1/T2 の責務分離が測定可能な効果を持つこと

### very fast NVMe で mmap-backed T2 が不利な場合

scope condition として扱います。OS-delegated residency の有利不利は、storage latency と workload locality に依存します。

### uniform cold workload で page fault が支配的になる場合

想定内です。VMemKV の得意条件と不得意条件を明確にする結果として扱います。平均値だけでなく時系列を見ることで、初期状態と steady state を分けて説明します。

### simple mmap baseline が一部 workload で速い場合

問題ありません。その場合は、VMemKV の T1/T2 分離、reorganize、memory hints の効果が出る条件と出ない条件を明確にします。

### in-place update の効果が限定的な場合

中心主張は崩れません。in-place update は補助的な強みであり、主張の中心は OS に委譲した larger-than-memory 管理と T1/T2 の責務分離です。

---

## 10. 評価の一文目標

> VMemKV が、単なる mmap file access ではなく、RAM-resident T1 index と mmap-backed T2 value layer の責務分離によって、OS に larger-than-memory 管理を委譲しながら、単純な実装と実用的な性能を両立できることを示す。

in-place update は、この主張を補強する update-path 上の特徴として評価する。
