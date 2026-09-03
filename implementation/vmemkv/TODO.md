# VMemKV Implementation Plan & Remaining TODO List

This document outlines the roadmap to implement the full, robust architecture of VMemKV based on the academic specifications (HLD/LLD), supporting both disk-efficient In-place updates and crash-resilient Write-Ahead Logging (WAL).

---

## 3. Startup Swap Validation Check
* **Status**: 🔴 **Not Implemented**
* **Objective**:
  - Implement a startup check in the main application / VMemKV initialization to verify that the system has an active Swap file of sufficient capacity (e.g., at least 64GB / 1TB on production environments) to prevent OOM Killer.
  - If a Swap file is not configured or fails validation under memory-constrained environments, raise an initialization warning/error or exit gracefully.

## 5. Scan-side offset-order read reordering (no storage compaction)
* **Status**: 🔴 **Not Implemented**. No mechanism in the codebase mitigates key-order/physical-offset
  decorrelation.
* **Idea**: Scan already reads in bounded batches. Within one batch, sort the batch's candidates
  by *T2 physical offset* before reading them (instead of T1's key order), then re-sort the
  fetched results back to key order before invoking the caller's callback. No writer-stop, no
  space reclaimed, purely a Scan-internal read-ordering change.
* **Known limitation**: does not address space amplification.

## 6. LTM full-corpus checkpoint()/write-path stalls under severe memory pressure
* **Status**: 🟡 **Root-caused, no dedicated fix landed** -- `memory.high`'s synchronous-reclaim-only
  behavior under sustained write bursts (a general cgroup v2 characteristic, not VMemKV-specific)
  turns any crossing of the memory budget into a dirtying-vs-reclaim race that doesn't reliably
  self-correct. Full investigation log: `docs/benchmark/20260829_ltm_full_corpus_thrashing_investigation.md`.
* **Durable finding**: for this project's `ltm/1KB` write-burst workload, `target_ratio<=1.5` is
  reliably safe and `target_ratio>=1.65` reliably hangs; `1.5-1.65` is a probabilistic knife's edge.
* **Landed as a partial mitigation**: `T1AppendCapacityLog2` shrunk 22->21, halving each
  `AppendRegion` generation's fixed RSS footprint (`append_region_live_count`/
  `append_region_peak_count` in `VMemKVStatistics` track this).
* **Next steps**: (a) a self-throttling/backpressure mechanism in `bulk_load()`'s write burst
  (check memory pressure, voluntarily pace writes, rather than relying solely on the kernel's
  `memory.high` response); (b) item 7 below (`reorganize()`'s O(corpus) cost) is a second,
  independent contributor to this class of stall and should be considered alongside any fix here.

## 7. `T1Index::reorganize()`'s O(corpus) merge cost
* **Status**: 🔴 **Not Implemented** -- found 2026-08-31.
* **Problem**: `reorganize()`'s in-memory merge of `sorted_region_` with `append_active_`'s region
  walks the full existing `sorted_region_` every call, regardless of how small the delta being
  merged in is.
  `checkpoint_internal()`'s T1-checkpoint-file rewrite inherits this same O(corpus) cost, since it
  depends on this merge to produce a coherent snapshot. This is the structural reason checkpoint
  and T1-only-reorganize auto-triggering can compound under sustained writes: each cycle pays the
  same fixed corpus-sized cost, so triggering more often does not reduce total overhead
  proportionally, and a write burst that outpaces one cycle's completion can cascade into a
  worsening backlog (observed directly this session during a population-phase benchmark run).
* **Real fix**: an incremental/leveled T1 checkpoint format (LSM-style memtable-flush +
  compaction) so durability cost is O(delta), decoupling WAL-rotation frequency from full-corpus
  compaction frequency. Deemed too complex to take on immediately; documented here for future work.
* **Mitigation in use for benchmarking**: `VMEMKV_SUPPRESS_AUTO_REORG` lets a benchmark driver
  suppress the auto-triggered Checkpoint branch specifically during a population/setup phase (see
  `reorg_worker_loop()` in `vmemkv_impl.hpp`), without touching the underlying cost.
