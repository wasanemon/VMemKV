# VMemKV 評価計画

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

## 2. Evaluation section の構成案

```text
8. Evaluation
   8.1 Experimental Setup
       Hardware, filesystem, SSD, cgroup memory limit, RocksDB options,
       VMemKV durability scope, msync/writeback policy, thread counts,
       warmup, repetitions.

   8.2 Larger-than-Memory Behavior and Thread Scalability
       DRAM-resident vs larger-than-memory runs, dataset/memory ratio,
       page faults, value size, skew, throughput/p99/p99.9/faults/
       bandwidth time-series, cycles/op, TLB shootdowns.

   8.3 YCSB-based Comparison with RocksDB
       YCSB load phase and workloads A, B, C, E, and F.
       Workload D is optional and reported in the appendix if space permits.
       Use 1 KiB and 16 KiB values.
       Compare VMemKV with RocksDB under matched memory and durability settings.
       Include RocksDB BlobDB for 16 KiB values to avoid an unfair comparison
       against a non-value-separated RocksDB configuration.
       Report throughput, p50/p95/p99/p99.9 latency, storage usage,
       logical bytes written, WAL bytes written, device bytes written,
       device write amplification, and engine write amplification.

   8.4 T1/T2 Design Breakdown
       AppendMap, BloomFilter, SimdScan, MemoryHints, Inline64.
       Show only workloads where each optimization should matter.

   8.5 Reorganization and Checkpoint Behavior
       Scan before/during/after, T2 unreachable bytes,
       copied bytes, reclaimed bytes, fork time, stop-the-world time,
       WAL replay time, memory high-water mark, TLB shootdowns.

   8.6 mmap-only Microbaseline
       mmap-backed value file + in-memory unordered_map index.
       Used only to estimate the raw cost of mmap-backed value access;
       it is not a durability-matched competitor.

   8.7 Recovery and Crash-Consistency
       Crash during load/update/checkpoint/reorganization.
       Recovery time as a function of WAL size and checkpoint age,
       WAL replay bytes, recovered key count, checksum.

   8.8 Implementation Responsibility
       One table comparing DB-side responsibilities.
```

---

## 3. Scope

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

## 4. 評価軸

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
- cycles/op, instructions/op, TLB shootdowns

### Q2. YCSB-based RocksDB 比較で競争力があるか？

VMemKV の主 baseline は RocksDB です。KVS 論文としての標準 macrobenchmark を明確にするため、RocksDB comparison は YCSB-based comparison として構成します。

本編では YCSB load phase と Workload A, B, C, E, F を使います。Workload D は read-latest / insert-heavy の補助 workload として扱い、余力があれば appendix に回します。

16 KiB value の write amplification 比較では、通常の RocksDB だけでなく RocksDB BlobDB も測ります。これは、VMemKV が large value を T2 に分離する設計であるため、RocksDB 側にも value-separated variant を含めないと比較が不公平になりやすいためです。可能なら LevelDB も補助 baseline として追加します。

見る workload:

- YCSB load phase
- YCSB Workload A: update-heavy
- YCSB Workload B: read-heavy
- YCSB Workload C: read-only
- YCSB Workload E: short scan
- YCSB Workload F: read-modify-write
- YCSB Workload D: read-latest / insert-heavy, optional

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
- foreground p99 / p99.9 latency during reorganization
- fork time
- stop-the-world time
- WAL replay time
- checkpoint 中の memory high-water mark

### Q5. Durability / recovery は成立するか？

VMemKV は WAL と checkpoint による durability を持つ設計です。評価では、crash 後に checkpoint + WAL replay で復旧できることを確認します。

見るもの:

- recovery time
- checkpoint age
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

## 5. 本編に残す必須実験

### E0. Experimental setup and fairness

目的:

- すべての比較の前提を明確にする。
- VMemKV と baselines の memory budget / durability / I/O policy をできるだけ揃える。
- mmap dirty page writeback と `msync` の方針を固定し、再現性を確保する。

記録するもの:

- hardware
- CPU core count
- NUMA topology and CPU pinning policy
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
- VMemKV `msync` policy
- checkpoint file `fsync` policy
- T2 mapping mode: `MAP_SHARED` or `MAP_PRIVATE`
- Linux dirty page settings: `vm.dirty_background_ratio` or bytes, `vm.dirty_ratio` or bytes, `vm.dirty_expire_centisecs`, `vm.dirty_writeback_centisecs`
- warmup duration
- run duration
- repetition count
- error bars or standard deviation

注意:

- VMemKV は OS page cache を使うため、プロセス RSS だけでは memory budget を比較できない。
- cgroup などで総物理メモリを制限し、VMemKV と RocksDB を同じ budget で比較する。
- RocksDB の buffered I/O / direct I/O はどちらかに固定し、設定を明記する。
- compression は無効を基本とする。有効にする場合は別条件として扱う。
- 多ソケット環境では、原則として単一 NUMA node に pin する。複数 NUMA node を使う場合は別条件として扱う。

---

### E1. Larger-than-memory behavior and thread scalability

目的:

- OS に委譲した value 常駐管理の有効性と限界を示す。
- thread count を増やしたときに throughput と tail latency がどう変化するかを示す。
- DRAM 常駐条件と larger-than-memory 条件を分け、T1/T2 実装コストと OS page fault / eviction コストを切り分ける。

本編で変化させるもの:

- residency condition:
  - DRAM-resident: dataset fits in memory
  - larger-than-memory: dataset / memory ratio 4x, 8x
- value size: 1 KiB, 16 KiB
- access distribution: uniform, Zipf alpha 1.2
- run condition: warm, steady-state
- thread count: 1, 4, 8, physical cores, logical cores

指標:

- throughput
- p50 / p95 / p99 / p99.9 latency
- maximum latency
- major/minor page faults
- page faults per operation
- SSD read/write bandwidth
- CPU utilization
- cycles/op
- instructions/op
- dTLB misses/op
- TLB shootdowns
- throughput, p99 latency, major faults, SSD bandwidth の時系列

この実験では、run 全体の平均値だけでなく時系列も記録します。DRAM-resident run は index / synchronization cost を見るために使い、larger-than-memory run は OS page fault / eviction cost を上乗せして見るために使います。

---

### E2. YCSB-based comparison with RocksDB

目的:

- KVS 論文として標準的な YCSB-based macrobenchmark で、LSM-tree baseline に対する競争力を示す。
- value size と write amplification が性能差にどう影響するかを示す。
- RocksDB と VMemKV の thread scaling を比較する。
- VMemKV が全 workload で勝つことではなく、target workload での競争力と得意・不得意条件を明確にする。

本編で使う workload:

- YCSB load phase
- YCSB Workload A: update-heavy
- YCSB Workload B: read-heavy
- YCSB Workload C: read-only
- YCSB Workload E: short scan
- YCSB Workload F: read-modify-write

optional / appendix:

- YCSB Workload D: read-latest / insert-heavy
- full YCSB A-F summary, if space permits

value size:

- 1 KiB
- 16 KiB

thread count:

- 1
- 4
- 8
- physical cores
- logical cores

比較対象:

- VMemKV
- RocksDB
- RocksDB BlobDB, for 16 KiB values and write amplification comparison
- LevelDB, optional

比較条件:

- VMemKV と RocksDB の memory budget を揃える。
- VMemKV と RocksDB の durability scope を揃えた条件を最低 1 本作る。
- RocksDB の block cache size, compression, WAL/sync policy, I/O mode を明記する。
- 16 KiB value では RocksDB BlobDB を含め、non-value-separated RocksDB だけとの不公平な比較を避ける。

指標:

- throughput
- p50 / p95 / p99 / p99.9 latency
- storage usage
- logical bytes written
- WAL bytes written
- device bytes written
- device write amplification: device bytes written / logical bytes written
- engine write amplification: engine-written bytes / logical bytes written
- reorganization copied bytes
- cycles/op
- instructions/op
- TLB shootdowns

`engine-written bytes` は、VMemKV では WAL bytes、T2 append / overwrite bytes、reorganization copied bytes を含めます。RocksDB では WAL / memtable flush / compaction / blob write など、engine が発行した書き込みを含めます。device write amplification と engine write amplification は混同しません。

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
- checkpoint reload の停止時間、replay cost、memory spike を測る。

workload:

- delete-heavy
- value-growth update-heavy
- scan before / during / after reorganization
- T1 から参照されない T2 record の割合: 0%, 25%, 50%, 75%
- long run: VMemKV checkpoint reload と RocksDB compaction が少なくとも 1 回は発生する長さにする。

指標:

- scan throughput before / during / after
- T2 bytes_used before / after
- T1 から参照されない T2 bytes
- bytes copied
- bytes reclaimed
- reclaimed bytes / copied bytes
- reorganization duration
- foreground p99 / p99.9 latency during reorganization
- maximum latency during reorganization
- major page faults per operation before / after reorganization
- fork time
- stop-the-world time
- WAL replay time
- CoW page faults
- parent RSS during checkpoint
- child RSS during checkpoint
- cgroup memory high-water mark during checkpoint
- cgroup OOM event count
- TLB shootdowns

この実験では、reorganization を単なる scan 高速化ではなく、不要データの回収処理としても評価します。checkpoint の foreground cost と recovery time の関係を説明できるように、E6 と合わせて解釈します。

---

### E5. mmap-only microbaseline

目的:

- durability-matched competitor ではなく、raw mmap-backed value access のコストを見積もる。
- VMemKV の T1/T2 organization、reorganization、checkpoint、WAL recovery が追加する機能コストを切り分ける。
- VMemKV の性能が単に mmap を使っているだけで説明できるのか、それとも T1/T2 設計による差分があるのかを確認する。

baseline:

- mmap-backed value file + in-memory `unordered_map<key, offset/length>` index
- T1 sorted_region / append_region なし
- T1/T2 reorganization なし
- checkpoint なし
- WAL recovery なし
- crash consistency なし、または VMemKV より弱い durability scope として明記する

workload:

- get hit / miss
- update
- scan
- larger-than-memory workload
- value-growth update-heavy workload

指標:

- throughput
- p50 / p95 / p99 / p99.9 latency
- major/minor page faults
- SSD read/write bandwidth
- storage usage
- cycles/op
- instructions/op
- TLB shootdowns

この baseline は VMemKV や RocksDB と公平に競わせる competitor ではありません。機能を削った最小構成により、mmap-backed value access 自体の性能参照点を得るための microbaseline として扱います。

---

### E6. Recovery and crash-consistency sanity check

目的:

- checkpoint + WAL replay で復旧できることを示す。
- checkpoint frequency が foreground cost と recovery time の間に作る trade-off を示す。
- RocksDB comparison の durability scope を明確にする。

変化させるもの:

- WAL size since last checkpoint
- checkpoint age
- dataset size

crash point:

- load 後
- update-heavy workload 実行中
- checkpoint reload 中
- reorganization 中

指標:

- recovery time
- checkpoint load time
- WAL replay time
- replayed WAL bytes
- recovered key count
- checkpoint size
- WAL size
- checksum / full scan による整合性確認

この実験は大規模な crash testing ではなく、recovery path が機能することを示す sanity check として扱います。本文では、checkpoint file の完成判定、commit marker または atomic rename、incomplete checkpoint の扱い、WAL replay の開始 LSN / 終了 LSN を説明します。

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

## 6. Appendix / optional に回す実験

本編では扱わず、余力がある場合だけ追加します。

- YCSB Workload D
- full YCSB A-F summary, if not all workloads fit in the main text
- LMDB
- LevelDB
- full value-size sweep: 64 B, 4 KiB, 64 KiB など
- latency CDF
- Bitcask-like baseline
- append-update-only VMemKV variant
- multiple storage tiers

優先度:

| 実験 | 優先度 | 理由 |
| --- | --- | --- |
| YCSB Workload D | 中 | read-latest / insert-heavy の補助 workload。中心主張には A/B/C/E/F の方が直結する |
| full YCSB A-F summary | 中 | 標準 benchmark としての見栄えは上がるが、本編には多い場合がある |
| LMDB | 中 | mmap-based KVS 代表として有用 |
| full value-size sweep | 中 | 傾向確認用。主張には 1 KiB / 16 KiB で足りる |
| latency CDF | 低 | p99 / p99.9 と時系列で代替可能 |
| Bitcask-like baseline | 低 | 自作 baseline の公平性説明が必要 |
| append-update-only variant | 低 | in-place update は中心主張ではない |
| multiple storage tiers | 低 | 有用だが実験環境の制約が大きい |

RocksDB BlobDB は 16 KiB value の write amplification 比較では本編に含めます。それ以外の workload では optional baseline として扱います。

---

## 7. 必要な指標

### Performance

- operations/sec
- p50 / p95 / p99 / p99.9 latency
- maximum latency
- throughput time-series
- p99 / p99.9 latency time-series

### Storage

- logical bytes written
- WAL bytes written
- device bytes written
- device write amplification
- engine write amplification
- storage usage
- T2 bytes_used
- T2 bytes appended
- T2 bytes overwritten in place, if measured
- T1 から参照されない T2 bytes
- reorganization copied bytes
- bytes reclaimed by reorganization

### OS / Hardware

- major page faults
- minor page faults
- page faults per operation
- RSS
- parent / child RSS during checkpoint
- cgroup memory usage
- cgroup memory high-water mark
- cgroup OOM event count
- SSD read/write bandwidth
- SSD read/write bandwidth time-series
- dirty page writeback statistics
- CPU utilization
- cycles/op
- instructions/op
- dTLB misses/op
- LLC misses/op
- TLB shootdowns

### Reorganization / checkpoint

- reorganization count
- reorganization duration
- bytes copied
- bytes reclaimed
- reclaimed bytes / copied bytes
- foreground p99 / p99.9 latency during reorganization
- scan throughput before / during / after reorganization
- fork time
- stop-the-world time
- WAL replay time
- CoW page faults
- TLB shootdowns during checkpoint / reorganization

### Recovery

- recovery time
- checkpoint load time
- WAL replay time
- replayed WAL bytes
- recovered key count
- checksum / full scan result

---

## 8. 論文に載せたい図・表

### Figure 1. DRAM-resident vs larger-than-memory thread scalability

- x-axis: thread count
- y-axis: throughput and p99 / p99.9 latency
- series: VMemKV DRAM-resident, VMemKV larger-than-memory, RocksDB, mmap-only microbaseline

### Figure 2. Larger-than-memory time-series behavior

- x-axis: elapsed time
- y-axis: throughput, p99 / p99.9 latency, major faults, SSD bandwidth

### Figure 3. YCSB-based VMemKV vs RocksDB workload comparison

- workload: YCSB load, A, B, C, E, F
- optional / appendix: YCSB D
- value size: 1 KiB and 16 KiB
- y-axis: throughput, p99 latency, device write amplification, engine write amplification
- include RocksDB BlobDB for 16 KiB write amplification

### Figure 4. CPU and TLB overhead

- x-axis: thread count or workload
- y-axis: cycles/op, instructions/op, dTLB misses/op, TLB shootdowns
- compare DRAM-resident and larger-than-memory conditions

### Figure 5. T1/T2 ablation

- variants: baseline, AppendMap, BloomFilter, SimdScan, MemoryHints, Inline64, all-on
- show only relevant operations for each optimization

### Figure 6. Reorganization and checkpoint behavior

- scan throughput before / during / after
- T2 bytes_used before / after
- T1 から参照されない T2 bytes
- bytes copied and bytes reclaimed
- fork time / stop-the-world time / WAL replay time
- checkpoint memory high-water mark

### Figure 7. Recovery behavior

- x-axis: WAL size since last checkpoint or checkpoint age
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

適用条件として扱います。DRAM-resident run と larger-than-memory run の差分を見て、T1/T2 実装側の競合と OS page fault / eviction 経路のコストを分けて説明します。

### very fast NVMe で mmap-backed T2 が不利な場合

適用条件として扱います。OS に委譲した value 常駐管理の有利不利は、storage latency と workload locality に依存します。

### uniform cold workload で page fault が支配的になる場合

想定内です。VMemKV の得意条件と不得意条件を明確にする結果として扱います。平均値だけでなく時系列を見ることで、初期状態と steady state を分けて説明します。

### mmap-only microbaseline が速い場合

想定内です。mmap-only microbaseline は durability を揃えた競合ではなく、raw mmap-backed value access の性能参照点です。VMemKV がその参照点に対してどれだけの overhead を払い、T1/T2 organization、reorganization、checkpoint、WAL recovery を提供しているかを説明します。

### checkpoint 中に memory spike が出る場合

適用条件として扱います。checkpoint reload は fork と page cache に依存するため、cgroup memory limit 下では memory high-water mark と OOM risk を明示します。

### in-place update の効果が限定的な場合

中心主張は崩れません。in-place update は補助的な強みであり、主張の中心は OS に委譲した larger-than-memory 管理と T1/T2 の責務分離です。

---

## 10. 評価の一文目標

> VMemKV が、単なる mmap file access ではなく、RAM-resident T1 index と mmap-backed T2 value layer の責務分離によって、OS に larger-than-memory 管理を委譲しながら、単純な実装と実用的な性能を両立できる条件を示す。

あわせて、VMemKV が不利になる条件も明確にし、OS 委譲型設計の適用範囲を示す。
