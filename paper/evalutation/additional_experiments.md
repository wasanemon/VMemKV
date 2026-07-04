# VMemKV Evaluation: Additional Experiments to Add

このメモは、現在の `evaluation_plan.md` に追加すべき実験を簡潔にまとめる。
目的は、VMemKV の thesis を擁護することではなく、査読者が疑う点を切り分けて検証できる評価にすることである。

## Summary

追加すべき中核実験は以下の 4 つである。

| Experiment | Priority | Role |
| --- | --- | --- |
| Regime protocol | Must | `MAP_PRIVATE` T2 の dirty/private 状態と clean file-backed read 状態を分ける |
| pread twin | Must | mmap の効果を T1/T2 layout から切り分ける |
| fio O_DIRECT bound | Must | device 上限と kernel / mmap / VMemKV 側 bottleneck を切り分ける |
| RocksDB BlobDB | Must for large values | large value 比較を公平にする |

---

## 1. Regime Protocol

### Question

VMemKV は、どの状態の T2 を測っているのか。

`MAP_PRIVATE` の T2 では、load/update 直後の dirty page は backing file ではなく private COW page になる。
この状態を larger-than-memory read として測ると、file-backed page cache ではなく anonymous memory / swap の挙動を測ってしまう危険がある。

### Add to Evaluation

少なくとも以下を分けて測る。

| Regime | Description | Why it matters |
| --- | --- | --- |
| Load/write regime | load/update 直後。dirty private COW pages が多い | write-heavy LTM の現実的制約を示す |
| Clean read regime | reorganize/checkpoint/reopen 後。clean file-backed pages を読む | OS page cache / mmap read residency の本命評価 |
| Mixed regime | clean generation に一部 update を加えた状態 | 長時間運用時の clean/dirty 混在を示す |

### Required Protocol

本命の larger-than-memory read 実験では、次の手順を明記する。

```text
load
reorganize or checkpoint generation creation
drop page cache
measure read workload
```

### Metrics

- throughput
- p50 / p95 / p99 / p99.9 latency
- major/minor page faults
- RSS
- cgroup memory usage / high-water mark
- swap in/out
- SSD read/write bandwidth
- dirty/private page ratio, if available

---

## 2. pread Twin

### Question

VMemKV の性能差は mmap によるものか、T1/T2 layout によるものか。

### Add to Evaluation

同じ T1、同じ T2 record layout、同じ offset を使い、T2 value access だけを差し替える。

| Variant | T1 | T2 layout | Value access |
| --- | --- | --- | --- |
| VMemKV mmap | same | same | mmap pointer dereference |
| VMemKV pread twin | same | same | `pread` / `preadv` |

必要なら `posix_fadvise` の有無も小さな ablation として測る。

### Why It Is Necessary

これがないと、VMemKV vs RocksDB の差分が以下の要因で交絡する。

- T1/T2 split
- mmap page fault path
- OS page cache
- T2 record layout
- reorganization
- RocksDB の LSM / block cache / compaction

Bitcask / WiscKey 系の査読者には、「mmap ではなく pread でよいのでは」という疑問への直接回答になる。

### Metrics

- throughput
- p50 / p95 / p99 / p99.9 latency
- major/minor page faults
- syscall count
- SSD bandwidth
- CPU cycles/op
- instructions/op
- dTLB misses / TLB shootdowns

---

## 3. fio O_DIRECT Bound

### Question

VMemKV は SSD/NVMe の素の性能をどの程度使えているのか。

### Add to Evaluation

VMemKV の random read / scan 実験と同じ device に対して、`fio` で O_DIRECT の上限を測る。

代表的には以下を測る。

- random 4 KiB read
- random value-size read, if value size is fixed
- sequential scan bandwidth
- queue depth sweep
- thread / job count sweep

### Why It Is Necessary

VMemKV が遅い場合に、原因を分離できる。

| Observation | Possible interpretation |
| --- | --- |
| fio も遅い | device / platform limit |
| fio は速いが VMemKV mmap が遅い | mmap fault path / kernel bottleneck |
| fio と pread twin は速いが VMemKV mmap が遅い | mmap-specific bottleneck |
| fio と mmap twin は速いが VMemKV が遅い | T1/T2 implementation overhead |

Crotty / vmcache 系の先行研究に対して、「device bound の何%か」を示すためにも必要である。

### Metrics

- IOPS
- bandwidth
- p50 / p95 / p99 latency
- queue depth
- CPU utilization

---

## 4. RocksDB BlobDB

### Question

large value 比較で、VMemKV は value-separated RocksDB と比べても競争力があるか。

### Add to Evaluation

RocksDB baseline を以下に分ける。

| Baseline | Use case |
| --- | --- |
| RocksDB | production LSM baseline |
| RocksDB BlobDB | large value / key-value separation baseline |

BlobDB は少なくとも 16 KiB value では必須とする。
1 KiB value では optional でもよい。

### Why It Is Necessary

VMemKV は T1 に metadata、T2 に value を置く value-separated design に近い。
そのため、large value で vanilla RocksDB だけに勝っても、WiscKey / BlobDB 系の既知の利点を再確認しただけに見える。

BlobDB を入れることで、比較の問いを以下に変えられる。

```text
VMemKV は、value separation そのものではなく、
OS-managed value residency + RAM-resident T1 split によって何を得るのか。
```

### Required Configuration Notes

最低限、以下を明記する。

- `enable_blob_files`
- `min_blob_size`
- blob file size
- blob GC settings
- compression on/off
- WAL/sync policy
- block cache size
- direct I/O / buffered I/O
- memory budget

### Metrics

- throughput
- p50 / p95 / p99 / p99.9 latency
- storage usage
- logical bytes written
- RocksDB / BlobDB engine-written bytes
- device bytes written
- engine write amplification
- device write amplification
- compaction / blob GC foreground interference

---

## Recommended Placement

These additions should be reflected as follows.

| Existing section | Change |
| --- | --- |
| E0 Experimental setup | Add regime protocol and fio setup |
| E1 Larger-than-memory behavior | Split load/write, clean read, and mixed regimes; add fio bound |
| E2 YCSB RocksDB comparison | Add BlobDB as required for 16 KiB values |
| E5 mmap-only microbaseline | Rename to mmap/pread microbaseline pair; add pread twin |
| E7 Responsibility table | Mark BlobDB/WiscKey/Bitcask/vmcache responsibilities separately |

## Bottom Line

These four additions are not optional polish.
They are the experiments that make the evaluation defensible:

- Regime protocol prevents measuring the wrong memory regime.
- pread twin isolates mmap from the architecture.
- fio bound anchors results to hardware limits.
- BlobDB makes large-value RocksDB comparison fair.
