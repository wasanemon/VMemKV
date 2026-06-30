# VMemKV Evaluation Plan

This memo defines the evaluation strategy for the VMemKV paper.  It is written as a planning document, not as final paper text.

Target branch / implementation baseline: `wasanemon/VMemKV` `paper-writing`, based on the `pr-14` design direction.

Core paper claim:

> VMemKV is an in-place-first, two-tier key-value store over virtual memory. It keeps KV-specific metadata and update control in a compact mutable T1 index, while delegating larger-than-memory value residency to the OS through an mmap-backed T2 region.

Therefore, the evaluation should not be a generic KV benchmark suite.  It should answer whether this specific claim is credible.

---

## 1. Evaluation Questions

### RQ1. Does in-place-first updating reduce update amplification?

VMemKV's most distinctive claim is that existing keys can usually be updated without turning every update into a new logical version.

The key experiment is not just throughput.  We need to measure whether VMemKV writes fewer bytes per logical update than append/version-based designs.

Required metrics:

- update throughput
- p50 / p95 / p99 update latency
- logical value bytes updated
- T2 bytes appended
- T2 bytes overwritten in place
- physical device bytes written if available
- in-place update hit rate
- append fallback rate
- T2 garbage bytes generated

Expected result:

- Fixed-size updates should mostly hit the T2 in-place path.
- Value-growth updates should fall back to append + T1 offset swing.
- VMemKV should show low update amplification for stable-size values and degrade gracefully as values grow.

This is the most important evaluation for the paper.

---

### RQ2. Where is the boundary between in-place update and append fallback?

VMemKV should not claim that every update is physically in place.  The correct claim is in-place-first.

We need an experiment that intentionally crosses the boundary:

- same-size overwrite
- shrink update
- small growth within preallocated `alloc_len`, if supported
- growth beyond `alloc_len`
- random growth distribution

Required metrics:

- in-place hit rate
- append fallback rate
- latency distribution
- T2 bytes_used growth
- unreachable T2 bytes
- reorganize reclaimable bytes

Expected result:

- same-size and shrink updates should be stable and cheap
- value-growth updates should increase T2 garbage and eventually require reorganization

If `alloc_len` currently equals `value_len`, then the first implementation can only show same-size/shrink as in-place and growth as append fallback.  A later slack-allocation variant can test whether reserve space improves in-place hit rate.

---

### RQ3. Does OS-delegated larger-than-memory value management work under intended conditions?

VMemKV delegates value residency to the OS.  This should be evaluated under larger-than-memory conditions, not only in-memory microbenchmarks.

Vary:

- dataset size / DRAM ratio: in-memory, near-memory, 2x memory, 4x memory
- access distribution: uniform, Zipf alpha 0.8, 1.0, 1.2
- value size: 64 B, 1 KiB, 4 KiB, 16 KiB
- workload mix: read-only, read-heavy, update-heavy, mixed

Required metrics:

- throughput
- p50 / p95 / p99 / p999 latency
- major page faults
- minor page faults
- page fault time if available
- SSD read bandwidth
- SSD write bandwidth
- CPU cycles / instruction count
- LLC misses and dTLB misses if available

Expected result:

- VMemKV should be most convincing when T1 remains memory-resident and T2 exceeds memory.
- Uniform cold access over a much larger-than-memory value region may be bad; this is acceptable if the paper scopes the target workload honestly.
- Skewed workloads should show the value of OS page cache residency.

---

### RQ4. Does the mutable T1 index remain efficient across both sorted and append regions?

The unusual part of T1 is that both `sorted_region` and `append_region` can update payloads without moving the ordering key.

We need to show update cost for keys in:

- append_region before reorganize
- sorted_region after reorganize
- mixed state with both regions populated

Required metrics:

- update throughput by key location
- get throughput by key location
- scan throughput before / after T1 reorganize
- append_region size sensitivity
- effect of AppendMap

Expected result:

- Existing-key updates should not become expensive simply because a key moved into sorted_region.
- AppendMap should matter for point lookup/update when append_region is large.
- Reorganize should improve scan and negative lookup behavior.

---

### RQ5. Is reorganization a deferred repair mechanism rather than the normal update path?

VMemKV's story is not that reorganization disappears.  The story is that reorganization is decoupled from the normal update path.

Evaluate:

- insert-heavy workload causing ordering fragmentation
- delete-heavy workload causing T2 unreachable records
- fixed-size update-heavy workload causing little T2 garbage
- value-growth update-heavy workload causing T2 garbage
- scan before and after reorganize
- foreground latency during background or periodic reorganization

Required metrics:

- reorganize duration
- stop-the-world or publish pause time, if any
- bytes copied during T2 reorganization
- bytes reclaimed
- T1 append_region size before / after
- T2 bytes_used before / after
- foreground p99 latency during reorganize

Expected result:

- Fixed-size updates should create little or no T2 garbage.
- Deletes and growth updates should create reclaimable T2 garbage.
- Reorganization should improve scan locality and reclaim space, but should not be necessary for every update to become visible.

---

### RQ6. Which optimizations are essential, and which are secondary?

The implementation already exposes many variants.  The evaluation should avoid treating all optimizations as equally important.

Existing variant families:

- Baseline
- cumulative T1 optimizations: AppendMap -> BloomFilter -> SimdScan -> MemoryHints
- subtractive ablations: No AppendMap / No BloomFilter / No SimdScan / No MemoryHints
- inline value variants: no inline / 1-7B inline / 8B inline / all inline
- RocksDB adapter

Required metrics:

- point lookup latency
- update latency
- negative lookup latency
- scan latency
- memory overhead
- T2 access count

Expected result:

- AppendMap should be essential for point operations when append_region is large.
- BloomFilter should mainly help negative lookup after reorganize.
- SIMD and MemoryHints should be treated as secondary unless measurements show otherwise.
- Inline values should matter for tiny value workloads, but should not be the main paper claim.

---

### RQ7. Does VMemKV reduce implementation complexity?

Implementation simplicity is a contribution, but it should be presented carefully.  Do not claim "easy" subjectively.  Measure or tabulate what VMemKV does not implement.

Suggested evidence:

- component table comparing VMemKV, RocksDB/LSM, value-log design, buffer-pool design
- lines of code for core T1/T2/reorganize path, excluding tests and benchmarks
- number of major storage-engine mechanisms required:
  - user-space buffer pool
  - page replacement policy
  - multi-level compaction scheduler
  - value-log GC
  - page-id-to-frame table
  - pointer swizzling
  - explicit read cache

Expected framing:

> VMemKV reduces DB-side responsibility by keeping logical control in T1 and delegating physical value residency to the OS.  This does not eliminate storage management; it narrows it.

This should be a small table in the paper, not a large benchmark section.

---

## 2. Baselines

### Required baselines

#### B1. RocksDB

Purpose:

- compare against a practical LSM-based storage engine
- already integrated through the `VMemKV_RocksDB` adapter

Use for:

- point get/update/delete/insert
- mixed workload
- range scan if adapter supports comparable scan semantics
- update amplification comparison if physical bytes can be measured

Notes:

- RocksDB tuning must be documented.
- Compression should probably be disabled for clean byte-amplification comparisons.
- WAL / fsync policy must be stated, especially because VMemKV durability is not finalized.

#### B2. VMemKV append-update baseline

Purpose:

- isolate the value of in-place-first updates

This is the most important missing baseline.

Definition:

- On every update, always append a new T2 record and update the T1 offset.
- Do not call `update_value_at`, even when the value fits in the old allocation.

This mimics value-log update behavior while keeping the same T1/T2 infrastructure.

Needed implementation:

- add a config tag such as `AppendOnlyUpdate` or `DisableT2InPlaceUpdate`
- expose it as a benchmark variant

Use for:

- RQ1 update amplification
- RQ2 in-place boundary
- RQ5 reorganization pressure

#### B3. VMemKV baseline / all-on variants

Purpose:

- distinguish the base architecture from opt-in optimizations

Use:

- `VMemKV_Baseline`
- cumulative variants
- all-on variant
- subtractive ablations

#### B4. mmap-only / OS-only baseline

Purpose:

- show that VMemKV is not merely mmap

Minimal version:

- mmap-backed array/file access without T1/T2 logical control
- or a simple unordered-map index + mmap value file, if implementing a true KVS baseline is feasible

This is useful but not more important than B2.

### Optional baselines

#### B5. pread + LRU baseline

Purpose:

- compare OS-delegated residency against explicit user-space caching

Existing microbenchmark results can motivate this, but a KV-integrated pread+LRU baseline would be stronger.

Use only if time permits.

#### B6. LevelDB

Purpose:

- classic LSM baseline

Optional if RocksDB is already used and benchmark time is limited.

#### B7. Bitcask-like append-only baseline

Purpose:

- compare against append-only data file + in-memory index

Useful if implemented cheaply.  Otherwise, B2 already captures the most important value-log-style update behavior inside the VMemKV framework.

---

## 3. Workloads

### W1. Load

- sequential insert
- random insert
- dataset construction for later workloads

Metrics:

- load throughput
- bytes written
- T2 bytes_used
- T1 size

Purpose:

- show append-friendly insert behavior
- prepare datasets for read/update/scan experiments

---

### W2. Fixed-size update

Input:

- preload N keys with fixed-size values
- repeatedly update existing keys with the same value size

Parameters:

- value size: 8 B, 64 B, 1 KiB, 4 KiB
- access distribution: uniform and Zipf
- thread count: 1, 4, hardware concurrency

Metrics:

- update throughput
- p50/p95/p99 latency
- in-place hit rate
- append fallback rate
- bytes written per logical update
- T2 bytes_used growth

Purpose:

- primary proof for in-place-first update claim

---

### W3. Value-growth update

Input:

- preload N keys with values of size S
- update with values of size S, 2S, 4S, or random growth

Parameters:

- initial size: 64 B, 1 KiB
- new size: same, +16 B, 2x, 4x
- optional slack allocation: 0%, 25%, 50%, 100% if implemented

Metrics:

- in-place hit rate
- append fallback rate
- T2 garbage bytes
- reorganization reclaim ratio
- update latency

Purpose:

- draw the boundary of the in-place-first claim

---

### W4. Mixed read/update workload

Suggested YCSB-like mixes:

- A-like: 50% read / 50% update
- B-like: 95% read / 5% update
- C-like: 100% read
- F-like: read-modify-write

Parameters:

- uniform vs Zipf alpha 1.0
- value size: 64 B, 1 KiB, 4 KiB
- dataset / memory ratio: in-memory, 2x, 4x

Metrics:

- throughput
- latency CDF
- update amplification
- page faults
- SSD bandwidth

Purpose:

- show end-to-end behavior under realistic mixes

---

### W5. Range scan and scan-after-reorganize

Input:

- preload ordered keys
- test scan windows of different sizes
- run before and after T1/T2 reorganize

Parameters:

- scan window: 100, 1K, 10K keys
- dataset size: in-memory and larger-than-memory
- append_region fraction: 0%, 10%, 50%, 100% before reorg

Metrics:

- scan throughput
- per-record scan latency
- page faults
- T2 read locality
- effect of reorganization

Purpose:

- support the claim that reorganization repairs ordering/locality rather than making updates visible

---

### W6. Delete-heavy workload

Input:

- preload N keys
- delete a fraction of keys
- measure reads/scans before and after reorganize

Parameters:

- delete ratio: 10%, 50%, 90%
- uniform vs clustered deletes

Metrics:

- delete throughput
- T2 unreachable bytes
- scan cost before/after reorg
- bytes reclaimed by reorg

Purpose:

- show T1 tombstone behavior and deferred T2 garbage collection

---

## 4. Measurement Methodology

### Performance metrics

- operations per second
- p50 / p95 / p99 / p999 latency
- latency CDF for update-heavy workloads
- throughput during background reorganization

### Storage metrics

- logical payload bytes inserted/updated
- T2 bytes appended
- T2 bytes overwritten in place
- T2 bytes_used
- unreachable T2 bytes
- bytes reclaimed by reorganization
- physical device bytes written, if available

Derived metrics:

```text
update_amplification_logical = T2_bytes_written_or_appended / logical_update_bytes
append_fallback_rate = append_fallback_updates / total_updates
in_place_update_rate = in_place_updates / total_updates
reclaim_ratio = bytes_reclaimed_by_reorg / unreachable_bytes_before_reorg
```

### OS / hardware metrics

Linux preferred:

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
  - minor / major faults per process
- `iostat -dx`
  - read/write bandwidth
  - device utilization
- `/proc/diskstats`
  - physical sectors read/written

### Memory control

Larger-than-memory evaluation needs controlled memory pressure.

Options:

- run inside a cgroup with memory limit
- use a fixed-memory VM
- vary dataset size relative to machine memory
- explicitly drop page cache between cold runs, if permitted
- report warm and cold results separately

Do not mix cold-start and steady-state results without labeling them.

---

## 5. Required Instrumentation

Current benchmark support is useful but not enough for the paper's main claim.

Add a lightweight `Stats` structure to VMemKV, ideally exposed through the benchmark harness.

Suggested counters:

```cpp
struct VMemKVStats {
    uint64_t get_count;
    uint64_t insert_count;
    uint64_t update_count;
    uint64_t delete_count;

    uint64_t t1_existing_payload_updates;
    uint64_t t1_append_inserts;
    uint64_t t1_tombstone_updates;

    uint64_t t2_appends;
    uint64_t t2_in_place_updates;
    uint64_t t2_append_update_fallbacks;
    uint64_t t2_bytes_appended;
    uint64_t t2_bytes_overwritten;
    uint64_t t2_unreachable_bytes_estimate;

    uint64_t reorganize_count;
    uint64_t reorganize_bytes_copied;
    uint64_t reorganize_bytes_reclaimed;
    uint64_t reorganize_duration_ns;
    uint64_t reorganize_publish_pause_ns;
};
```

Minimum required counters:

- `t2_in_place_updates`
- `t2_append_update_fallbacks`
- `t2_bytes_appended`
- `t2_bytes_overwritten`
- `reorganize_bytes_reclaimed`
- `reorganize_duration_ns`

Without these, the paper cannot convincingly support the in-place-first claim.

---

## 6. Priority Plan

### P0: Must-have for the paper

1. **Fixed-size update vs append-update baseline**
   - VMemKV all-on
   - VMemKV append-update baseline
   - RocksDB
   - metrics: throughput, latency, update amplification, T2 bytes growth

2. **Value-growth boundary**
   - same-size vs growing updates
   - metrics: in-place rate, fallback rate, garbage bytes, latency

3. **Larger-than-memory mixed workload**
   - dataset / memory ratio sweep
   - read-heavy and update-heavy mixes
   - metrics: throughput, p99, page faults, SSD bandwidth

4. **Reorganization effect**
   - before / after scan
   - delete-heavy and value-growth workloads
   - metrics: reclaimed bytes, duration, scan improvement, foreground p99

5. **Ablation using existing VMemKV variants**
   - baseline, cumulative, all-on, no AppendMap, no BloomFilter, no MemoryHints, inline variants
   - focus on which optimization supports which claim

### P1: Strongly recommended

1. Implementation simplicity table
2. T1 update cost by key location: append_region vs sorted_region
3. Negative lookup after reorganize with BloomFilter
4. Memory ratio and Zipf alpha sweep
5. Tail latency during reorganization

### P2: Optional / only if time permits

1. pread+LRU KV baseline
2. LevelDB baseline
3. Bitcask-like baseline
4. THP / madvise sweep
5. crash recovery / restart time, only after durability design is finalized

---

## 7. What Not to Evaluate Yet

Avoid spending effort on evaluations that do not support the current paper claim.

Do not prioritize:

- distributed transactions
- phantom avoidance
- secondary indexes
- full SQL/DBMS workloads
- crash consistency, unless the write/recovery design is finalized
- detailed SIMD microbenchmarks beyond the ablation
- every possible RocksDB tuning permutation

The paper should be judged on:

1. in-place-first update behavior
2. OS-delegated larger-than-memory value management
3. T1/T2 responsibility separation
4. reorganization as deferred repair
5. implementation simplicity

---

## 8. Suggested Evaluation Section Outline

```text
8. Evaluation
   8.1 Experimental Setup
       Hardware, OS, filesystem, storage, memory limit, compiler, RocksDB configuration.

   8.2 Update Amplification and In-Place Hit Rate
       Fixed-size update vs append-update baseline vs RocksDB.

   8.3 Boundary of In-Place-First Updates
       Same-size, shrink, and value-growth update experiments.

   8.4 Larger-than-Memory Behavior
       Dataset/memory ratio, skew, value size, page faults, and tail latency.

   8.5 Reorganization as Deferred Repair
       Scan before/after reorg, delete/value-growth garbage, reclaimed bytes, pause time.

   8.6 Component Ablation
       AppendMap, BloomFilter, SIMD, MemoryHints, InlineShort, Inline8B.

   8.7 Implementation Complexity
       Small table comparing DB-side mechanisms across VMemKV, LSM, value-log, buffer-pool designs.
```

If the paper must be shorter, merge 8.3 into 8.2 and 8.6 into the relevant experiment sections.

---

## 9. Minimal Benchmark Implementation Checklist

For Nakazono-san or implementation owner:

- [ ] Add VMemKV stats counters for T1/T2 update paths.
- [ ] Add config to disable T2 in-place update and force append-on-update.
- [ ] Add benchmark mode: fixed-size update.
- [ ] Add benchmark mode: value-growth update.
- [ ] Add benchmark mode: mixed read/update with configurable ratios.
- [ ] Add benchmark mode: dataset size / value size sweep.
- [ ] Add benchmark mode: scan before/after reorganize.
- [ ] Add benchmark output as machine-readable CSV or JSON.
- [ ] Record page faults and process RSS for each run.
- [ ] Record T2 bytes_used and bytes reclaimed.
- [ ] Document RocksDB options and sync policy.

---

## 10. Expected Paper Figures

### Figure 1: Update amplification

- x-axis: value size or workload type
- y-axis: bytes written per logical update
- lines: VMemKV in-place, VMemKV append-update, RocksDB

### Figure 2: In-place boundary

- x-axis: new value size / old value size
- y-axis: in-place hit rate and update latency

### Figure 3: Larger-than-memory performance

- x-axis: dataset / memory ratio
- y-axis: throughput and p99 latency
- series: VMemKV, RocksDB, optional pread+LRU

### Figure 4: Page fault behavior

- x-axis: dataset / memory ratio or Zipf alpha
- y-axis: major faults / operation

### Figure 5: Reorganization effect

- before/after bars for scan throughput, T2 bytes_used, reclaimed bytes

### Figure 6: Ablation

- bar chart for key operations under VMemKV variants
- emphasize AppendMap / BloomFilter / Inline effects only where relevant

### Table 1: Implementation responsibility

Compare VMemKV, LSM/RocksDB, WiscKey-like value-log, buffer-pool engine.

Columns:

- explicit buffer pool
- page replacement policy
- multi-level compaction
- value-log GC
- in-place update path
- OS-managed value residency
- ordered scan support

---

## 11. How to Interpret Possible Negative Results

The evaluation should be robust even if VMemKV does not win every throughput graph.

### If RocksDB is faster in some workloads

This does not invalidate the paper if VMemKV shows:

- lower update amplification for stable-size updates
- simpler implementation responsibility
- competitive larger-than-memory behavior under the intended workload regime

### If mmap-backed T2 is worse on very fast NVMe

Frame this as a scope condition.  OS-delegated residency is expected to be workload- and device-dependent.

### If value-growth updates create significant garbage

This is expected.  It supports the in-place-first boundary and motivates reorganization.

### If SIMD or MemoryHints do not help much

Treat them as secondary optimizations.  Do not make them central claims.

---

## 12. One-Sentence Evaluation Goal

The evaluation should demonstrate that VMemKV's architecture is not just "mmap plus a hash table".  It should show that a mutable T1 index, allocation-preserving T2 updates, and deferred reorganization together make in-place-first larger-than-memory KV storage plausible and measurable.
