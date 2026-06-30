# VMemKV 評価計画

このメモは、VMemKV 論文の評価戦略を定義するための計画文書です。最終的な論文本体ではありません。

対象ブランチ / 実装基準: `wasanemon/VMemKV` の `paper-writing` ブランチ。設計方針としては `pr-14` の方向性を基準にします。

---

## 0. 評価計画の前提

VMemKVの設計思想は、以下です。

> VMemKV は、データ管理を OS の仮想メモリ機構（`mmap`, `fork`, `mincore`, `madvise`）へ可能な限り委譲する Larger-than-memory KVS である。

構成要素:

- Tier 1: RAM 常駐のインデックス層
- Tier 2: file-backed mmap を用いた大容量データ層
- WAL
- Checkpoint
- Background Jobs

狙い:

- buffer pool を自前実装しない
- page replacement algorithm を自前実装しない
- 複雑な multi-level compaction に依存しすぎない
- OS の virtual memory / page cache に value residency を委譲する
- 実装を単純に保ちながら、実用可能な性能を狙う

競合・比較対象としての位置づけ:

- LSM-tree 系、特に RocksDB / LevelDB に比肩する性能を狙う
- Bitcask のような単純な設計を狙う
- LMDB のような mmap-based KVS よりも大規模な data set を扱えることを狙う

したがって、評価の中心は以下であるべきです。

> **VMemKV は、OS に larger-than-memory 管理を委譲しつつ、T1/T2 の責務分離によって、単純な実装と競争力のある性能を両立できるか。**

in-place update は重要ですが、VMemKV が LSM / value-log 系と異なる update path を持つことを示す補助的な評価軸として扱います。

---

## 1. 論文の中心主張と評価の対応

### 中心主張

> VMemKV is a larger-than-memory key-value store that delegates value residency and paging to the OS virtual memory subsystem while retaining KV-specific logical control in a compact RAM-resident Tier 1 index.

日本語では、次のように置きます。

> VMemKV は、value の residency / paging / caching を OS の virtual memory subsystem に委譲しつつ、KV 固有の lookup / update / scan / delete / fragmentation control を compact な RAM 常駐 Tier 1 index に保持する larger-than-memory KVS である。

### 評価で示すべきこと

1. **OS-delegated LTM management が実用的な性能を出せるか**
2. **T1/T2 の責務分離が効いているか**
3. **buffer pool や複雑な compaction を持たない単純な設計で競争力があるか**
4. **reorganize / checkpoint / background jobs によって fragmentation と durability boundary を管理できるか**
5. **in-place-first update は、VMemKV の補助的な強みとして update-heavy workload で効くか**

---

## 2. 評価で答えるべき問い

### RQ1. OS に委譲した larger-than-memory value management は、どの条件で競争力を持つか？

最重要の問いです。

VMemKV は user-space buffer pool を持たず、Tier 2 value region を mmap-backed file として扱います。したがって、評価では dataset が DRAM を超える条件で、OS page cache / page fault / storage latency が性能にどう効くかを示す必要があります。

変化させる要素:

- dataset size / DRAM ratio: 0.5x, 1x, 2x, 4x, 8x memory
- value size: 64 B, 1 KiB, 4 KiB, 16 KiB
- access distribution: uniform, Zipf alpha 0.8, 1.0, 1.2, 1.5
- run condition: cold, warm, steady-state
- workload mix: read-only, read-heavy, write-heavy, mixed

必要な指標:

- throughput
- p50 / p95 / p99 / p999 latency
- major page faults / operation
- minor page faults / operation
- page fault time, if available
- SSD read/write bandwidth
- CPU cycles / instructions
- LLC misses, dTLB misses, if available
- RSS / virtual memory size

期待される解釈:

- VMemKV は、T1 が memory-resident に保たれ、T2 が memory を超える条件で最も説得力を持つ。
- skewed workload では、OS page cache による hot value residency が効くはずである。
- uniform cold access が極端に大きい dataset では page fault が支配的になり、不利になってもよい。この場合は target workload の scope として説明する。

---

### RQ2. VMemKV は RocksDB / LevelDB に対して、どの workload で競争力を持つか？

VMemKV の high-level design は、LSM-tree に比肩する性能を目標にしています。したがって、RocksDB / LevelDB との比較は必須です。

評価する workload:

- sequential load
- random load
- point get hit
- point get miss
- insert
- update
- delete
- range scan
- read-heavy mixed
- write-heavy mixed
- larger-than-memory mixed workload

必要な指標:

- throughput
- p50 / p95 / p99 latency
- bytes written per operation
- device read/write bandwidth
- storage space usage
- tail latency during background work

期待される解釈:

- VMemKV がすべての workload で RocksDB に勝つ必要はない。
- 重要なのは、より単純な構造で、想定 workload において競争力を示すことである。
- RocksDB が有利な workload と VMemKV が有利な workload を正直に分ける。

---

### RQ3. T1/T2 の責務分離は効いているか？

VMemKV の中心設計は、RAM 常駐 Tier 1 と mmap-backed Tier 2 の分離です。

評価すること:

- T1 が hot metadata として機能しているか
- T2 が large value region として機能しているか
- T1 の最適化が point lookup / scan にどう効くか
- T2 access が page fault / SSD I/O とどう結びつくか

必要な実験:

- T1-only / T1-heavy microbenchmarks
- T2 value size sweep
- T1 optimization ablation
- T2 mmap vs alternative access path, if implemented
- key size / value size sweep

必要な指標:

- T1 lookup latency
- T2 access latency
- T2 page faults
- T1 memory usage
- T2 bytes_used
- operations per second
- scan throughput

期待される解釈:

- T1 の役割は、単なる hash table ではなく、logical control plane である。
- T2 の役割は、OS-managed data plane である。
- この分離が、VMemKV の単純性と larger-than-memory 性を支える。

---

### RQ4. VMemKV は Bitcask のような単純性を保てているか？

VMemKV は、Bitcask のような単純な設計を目指しています。ただし Bitcask と同一ではありません。

Bitcask 的な観点:

- simple append path
- in-memory index
- simple recovery model
- background merge / compaction

VMemKV の違い:

- T1 は ordered scan を意識した two-region index
- T2 は file-backed mmap large value region
- OS が value residency を管理する
- Reorganize は ordering repair と storage garbage collection を兼ねる

評価すること:

- 実装責務の少なさ
- 必要な storage-engine mechanisms の数
- insert path の単純性
- background reorganize の役割

提示方法:

大規模な性能評価ではなく、比較表がよいです。

比較対象:

- VMemKV
- RocksDB / LSM
- Bitcask-like design
- WiscKey-like value-log design
- LMDB / mmap-based B+tree design
- explicit buffer-pool engine

比較軸:

- explicit buffer pool
- page replacement policy
- multi-level compaction
- value-log GC
- mmap-backed value storage
- in-memory index
- ordered scan support
- larger-than-memory target
- background reorganization / merge

---

### RQ5. LMDB / mmap-based design と比べて、VMemKV の larger-than-memory 性はどこにあるか？

VMemKV は LMDB より大規模な data set を扱うことを目指す、と設計文書にあります。この比較は慎重に扱う必要があります。

LMDB は mmap-based であり、VMemKV と同じく OS の virtual memory を使う比較対象になり得ます。一方で、LMDB は B+tree / copy-on-write / transaction を持つため、VMemKV とは機能セットが違います。

評価するなら見るべき点:

- dataset size / memory ratio sweep
- mmap map size / address space pressure
- write amplification
- update latency
- read latency
- range scan
- storage growth under updates/deletes

注意:

- LMDB は optional baseline とする。
- LMDB に勝つことを必須条件にしない。
- 比較する場合は、transaction / durability semantics の違いを明記する。

---

### RQ6. Reorganize / checkpoint / background jobs は fragmentation と運用コストを管理できるか？

VMemKV は、複雑な LSM compaction の代わりに、T1/T2 reorganization、checkpoint、background jobs で fragmentation と永続化境界を管理します。

評価すること:

- T1 append_region の肥大化による ordering fragmentation
- T2 unreachable records による storage fragmentation
- reorganize 前後の scan performance
- reorganize 前後の T2 bytes_used
- background work 中の foreground latency
- checkpoint / reload の pause time, if implemented
- WAL replay / recovery time, if implemented

必要な workload:

- insert-heavy
- delete-heavy
- update-heavy with value growth
- scan-heavy before/after reorganize
- mixed workload during background jobs

必要な指標:

- reorganize duration
- bytes copied
- bytes reclaimed
- append_region size before/after
- T2 bytes_used before/after
- foreground p99 latency during background work
- checkpoint size
- checkpoint duration
- WAL replay time, if implemented

注意:

- `pr-14` では durability / recovery 周りに未確定要素があります。未実装のものは評価対象にせず、設計上の将来課題として扱う。

---

### RQ7. in-place-first update は、補助的な強みとしてどこで効くか？

in-place update は重要ですが、論文全体の中心ではなく、update-heavy workload における VMemKV の性質として評価します。

評価すること:

- stable-size update で T2 append が抑えられるか
- value-growth update で append fallback が起きる境界
- append-only update baseline と比べた update amplification
- delete が T1 tombstone だけで済む効果

必要な baseline:

- VMemKV normal
- VMemKV append-update baseline: update 時に常に T2 append + T1 offset swing
- RocksDB

必要な指標:

- update throughput
- p50 / p95 / p99 update latency
- in-place update rate
- append fallback rate
- T2 bytes appended
- T2 bytes overwritten
- T2 garbage bytes

期待される解釈:

- fixed-size update では VMemKV の in-place-first path が効くはずである。
- value-growth update では garbage が増え、reorganize が必要になる。
- これは VMemKV の主貢献ではなく、T1/T2 mutable layout の有用な副産物として扱う。

---

## 3. Baselines

### 必須 baseline

#### B1. RocksDB

目的:

- 実用 LSM-tree storage engine との比較
- VMemKV の競争力を示すための主 baseline

使用対象:

- load
- get
- insert
- update
- delete
- mixed workload
- scan, if comparable
- larger-than-memory workload

注意:

- RocksDB options を明記する。
- compression の有無を明記する。
- WAL / sync policy を明記する。
- VMemKV の durability scope と揃わない場合は、fairness を明記する。

#### B2. VMemKV variants

目的:

- VMemKV 内部の設計要素の効果を分離する。

使用するもの:

- VMemKV baseline
- VMemKV all-on
- cumulative T1 optimizations
- subtractive ablations
- inline variants

評価対象:

- AppendMap
- BloomFilter
- SimdScan
- MemoryHints
- InlineShort
- Inline8B

#### B3. mmap-only / simple mmap KVS baseline

目的:

- VMemKV が単なる mmap file access ではないことを示す。

候補:

- mmap-backed value file + simple unordered_map index
- T1/T2 reorganization なし
- OS residency は同じだが、VMemKV の logical control を持たない baseline

優先度:

- 可能なら実装する。
- VMemKV の主張を明確にする上で有用。

#### B4. Append-update VMemKV baseline

目的:

- in-place-first update の効果を分離する。

定義:

- update 時に常に T2 append + T1 offset update を行う。
- `update_value_at()` を使わない。

優先度:

- update-heavy 評価では重要。
- ただし論文全体の主 baseline ではない。

### Optional baseline

#### B5. LevelDB

RocksDB だけで十分な場合は optional。LSM の古典 baseline として入れられるなら有用。

#### B6. LMDB

mmap-based KVS として有用。ただし transaction / COW / map size / durability semantics が異なるため、比較は慎重に行う。

#### B7. Bitcask-like baseline

VMemKV の「単純な設計」という主張を補強するために有用。安く実装できる場合のみ。

#### B8. pread + LRU baseline

OS-delegated residency と user-space caching を比較するための baseline。KV-integrated に実装できれば強いが、時間がなければ既存 microbenchmark を motivation として使う。

---

## 4. Workloads

### W1. Load workload

目的:

- append-friendly insert path を測る。
- 後続 workload の dataset を構築する。

パターン:

- sequential load
- random load

指標:

- load throughput
- load latency
- T2 bytes_used
- storage bytes written
- T1 size

---

### W2. Larger-than-memory point lookup

目的:

- OS-delegated value residency の基本性能を測る。

パラメータ:

- dataset / memory ratio: 0.5x, 1x, 2x, 4x, 8x
- value size: 64 B, 1 KiB, 4 KiB, 16 KiB
- distribution: uniform, Zipf alpha 1.0, 1.2, 1.5
- run: cold / warm / steady-state

指標:

- get throughput
- latency CDF
- page faults / operation
- SSD read bandwidth
- CPU overhead

---

### W3. Mixed read/write workload

目的:

- 実運用に近い workload で RocksDB 等と比較する。

YCSB-like mixes:

- 95% read / 5% update
- 80% read / 20% update
- 50% read / 50% update
- read-modify-write

指標:

- throughput
- p50 / p95 / p99 latency
- page faults
- write bandwidth
- T2 bytes growth

---

### W4. Scan workload

目的:

- T1 の ordered access と reorganize の効果を測る。

パラメータ:

- scan window: 100, 1K, 10K keys
- append_region fraction: 0%, 10%, 50%, 100%
- before / after T1 reorganize
- before / after T2 reorganize, if implemented

指標:

- scan throughput
- per-record latency
- T2 page faults
- T2 read locality
- reorganize improvement ratio

---

### W5. Update workload

目的:

- in-place-first update を補助的に評価する。

パターン:

- same-size update
- shrink update
- value-growth update
- random-size update

指標:

- update throughput
- update latency
- in-place update rate
- append fallback rate
- update amplification
- T2 garbage bytes

---

### W6. Delete-heavy workload

目的:

- T1 tombstone と T2 deferred garbage collection を評価する。

パラメータ:

- delete ratio: 10%, 50%, 90%
- uniform deletes
- clustered deletes

指標:

- delete throughput
- scan before/after reorganize
- T2 unreachable bytes
- bytes reclaimed

---

### W7. Background job workload

目的:

- reorganize / checkpoint / background jobs が foreground workload に与える影響を測る。

評価対象:

- T1 reorganize
- T2 reorganize
- checkpoint, if implemented
- WAL replay / recovery, if implemented

指標:

- background job duration
- foreground throughput during background job
- foreground p99 latency during background job
- pause time
- bytes copied / reclaimed

---

## 5. Measurement Methodology

### Performance metrics

- operations per second
- p50 / p95 / p99 / p999 latency
- latency CDF
- throughput during background jobs

### Storage metrics

- logical bytes inserted/updated
- T2 bytes_used
- T2 bytes appended
- T2 bytes overwritten in place
- T2 unreachable bytes
- bytes reclaimed by reorganize
- physical device bytes written, if available

### OS / hardware metrics

Linux を推奨します。

- `perf stat`
  - page-faults
  - minor-faults
  - major-faults
  - cycles
  - instructions
  - cache-misses
  - dTLB-load-misses
  - LLC-load-misses
- `/proc/self/stat` or `getrusage`
  - process minor / major faults
- `iostat -dx`
  - read/write bandwidth
  - device utilization
- `/proc/diskstats`
  - physical sectors read/written

### Memory control

larger-than-memory 評価には controlled memory pressure が必要です。

方法:

- cgroup memory limit
- fixed-memory VM
- dataset size / physical memory ratio sweep
- cold run 前の page cache drop, if permitted
- warm / cold / steady-state の明確なラベル付け

cold-start と steady-state を混ぜてはいけません。

---

## 6. 必要な Instrumentation

### 必須 counters

VMemKV の評価には、少なくとも以下の counters が必要です。

```cpp
struct VMemKVStats {
    uint64_t get_count;
    uint64_t insert_count;
    uint64_t update_count;
    uint64_t delete_count;

    uint64_t t1_lookup_count;
    uint64_t t1_append_hits;
    uint64_t t1_sorted_hits;
    uint64_t t1_misses;

    uint64_t t2_reads;
    uint64_t t2_appends;
    uint64_t t2_bytes_appended;
    uint64_t t2_bytes_used;

    uint64_t t2_in_place_updates;
    uint64_t t2_append_update_fallbacks;
    uint64_t t2_bytes_overwritten;
    uint64_t t2_unreachable_bytes_estimate;

    uint64_t reorganize_count;
    uint64_t reorganize_bytes_copied;
    uint64_t reorganize_bytes_reclaimed;
    uint64_t reorganize_duration_ns;
    uint64_t reorganize_publish_pause_ns;
};
```

特に必要なもの:

- T1 hit/miss breakdown
- T2 read count
- T2 bytes_used
- page faults per run
- reorganization bytes copied/reclaimed
- foreground latency during background work

in-place update 用 counters は重要ですが、評価全体の一部です。

---

## 7. 優先度付き計画

### P0: 論文に必須

1. **Larger-than-memory point/mixed workload**
   - VMemKV vs RocksDB
   - dataset / memory ratio sweep
   - page faults, throughput, p99 latency

2. **RocksDB / LSM comparison**
   - load, get, update, delete, scan, mixed
   - VMemKV の target workload での競争力を示す

3. **T1/T2 ablation**
   - VMemKV baseline / all-on / cumulative / subtractive variants
   - AppendMap, BloomFilter, MemoryHints, Inline の効果

4. **Reorganization effect**
   - scan before/after
   - delete/growth workload 後の T2 reclaim
   - background job 中の p99 latency

5. **Implementation responsibility table**
   - VMemKV, RocksDB/LSM, Bitcask-like, WiscKey-like, LMDB, buffer-pool engine を比較

### P1: 強く推奨

1. mmap-only / simple mmap KVS baseline
2. update workload with in-place vs append-update baseline
3. value size sweep
4. Zipf alpha sweep
5. T1 sorted vs append hit breakdown

### P2: optional

1. LevelDB baseline
2. LMDB baseline
3. Bitcask-like baseline
4. pread+LRU KV baseline
5. checkpoint / recovery evaluation after durability design is finalized
6. THP / madvise sweep

---

## 8. 評価しすぎないもの

現在の論文主張を支えない評価には、労力を割きすぎないようにします。

優先しないもの:

- distributed transactions
- phantom avoidance
- secondary indexes
- full SQL/DBMS workloads
- durability design が未確定な状態での crash consistency
- detailed SIMD microbenchmark
- RocksDB tuning permutation の網羅
- in-place update だけを中心にした過剰な評価

---

## 9. Evaluation section の推奨 outline

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

## 10. 最小限の benchmark 実装 checklist

中園氏、または実装担当者向け:

- [ ] benchmark output を CSV または JSON にする。
- [ ] page faults / RSS / elapsed time を run ごとに記録する。
- [ ] T1 hit/miss breakdown を追加する。
- [ ] T2 read count / bytes_used / bytes_appended を追加する。
- [ ] reorganization duration / bytes copied / bytes reclaimed を記録する。
- [ ] dataset size / memory ratio sweep を実行できるようにする。
- [ ] value size sweep を追加する。
- [ ] mixed read/write ratio を configurable にする。
- [ ] RocksDB options と sync policy を文書化する。
- [ ] VMemKV variants を benchmark output で明確に識別する。
- [ ] 可能なら simple mmap KVS baseline を追加する。
- [ ] 可能なら append-update-only VMemKV variant を追加する。

---

## 11. 論文に載せたい図・表

### Figure 1: Larger-than-memory throughput and tail latency

- x-axis: dataset / memory ratio
- y-axis: throughput and p99 latency
- series: VMemKV, RocksDB, optional mmap-only / LMDB

### Figure 2: Page fault behavior

- x-axis: dataset / memory ratio or Zipf alpha
- y-axis: major faults / operation, minor faults / operation

### Figure 3: VMemKV vs RocksDB workload comparison

- workload: load, get, update, delete, scan, mixed
- y-axis: throughput or normalized throughput

### Figure 4: T1/T2 ablation

- variants: baseline, AppendMap, BloomFilter, SimdScan, MemoryHints, Inline variants, all-on
- show only relevant operations for each optimization

### Figure 5: Reorganization effect

- before/after scan throughput
- T2 bytes_used before/after
- bytes reclaimed
- foreground p99 during background work

### Figure 6: Update path behavior

- stable-size vs value-growth update
- VMemKV normal vs append-update baseline
- update amplification / append fallback rate

### Table 1: Implementation responsibility

Compare:

- VMemKV
- RocksDB / LSM
- WiscKey-like value-log
- Bitcask-like KVS
- LMDB / mmap B+tree
- explicit buffer-pool engine

Columns:

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

## 12. Negative results の扱い

### RocksDB が一部 workload で速い場合

問題ありません。VMemKV はすべての workload で RocksDB に勝つ必要はありません。

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

## 13. 評価の一文目標

評価の一文目標は次です。

> VMemKV が、単なる mmap file access ではなく、RAM-resident T1 index と mmap-backed T2 value layer の責務分離によって、OS に larger-than-memory 管理を委譲しながら、単純な実装と実用的な性能を両立できることを示す。

in-place update は、この主張を補強する update-path 上の特徴として評価する。
