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
  `AppendRegion` generation's fixed RSS footprint; separately, `reorganize()`/`checkpoint()`'s own
  O(corpus) cost (a second, independent contributor to this class of stall) is now resolved by
  `ShardedT1Index` (see item 8 below) -- remaining gap is `bulk_load()`'s write burst itself.
* **Next step**: a self-throttling/backpressure mechanism in `bulk_load()` (check memory pressure,
  voluntarily pace writes, rather than relying solely on the kernel's `memory.high` response).

## 8. `ShardedT1Index` single-shard "routing tax"
* **Status**: 🔴 **Not implemented** -- found 2026-09-07, `docs/t1_sharding_design.md`.
* **Problem**: for a corpus small enough to never cross one shard's split threshold, every
  put/get/scan still pays `with_routing_guard()`'s directory-lookup/epoch-guard overhead with none
  of sharding's contention-spreading benefit to offset it. Measured up to ~30% Insert/Update
  throughput regression at high thread counts (`ltm/64KB`, 131K-key corpus) vs. the pre-sharding
  baseline.
* **Idea**: a fast path that bypasses `with_routing_guard()` while `shard_count() == 1`.

## 9. `ShardedT1Index` data inconsistency under extreme concurrency + cycling writes
* **Status**: 🔴 **Not root-caused** -- `docs/t1_sharding_design.md`. Lower priority: does not
  reproduce at production-representative scale or with single-pass (non-cycling) writes.
* **Problem**: small `target_shard_size` + high thread count + sustained cycling updates on a
  small key range produces real data inconsistency, distinct from (and surviving past) the three
  split-protocol bugs fixed this round.

## 10. `ShardedT1Index` migration gaps from the pre-sharding `T1Index`
* **Status**: 🔴 **Not implemented** -- dropped during the `VMemKVImpl` wiring, `docs/t1_sharding_design.md`.
* Delete-pressure trigger equivalent to the old `maybe_reorganize_if_needed_for_delete()`.
* Scan-active dynamic append-threshold reduction (kept the append region L2-cache-sized during a
  scan).

## 11. Validate organic-split impact with non-monotonic keys
* **Status**: 🔴 **Not measured**, `docs/t1_sharding_design.md`. `run_organic_split_probe.sh`'s
  monotonically-increasing-key workload always concentrates writes on exactly one "hot" shard
  regardless of shard count, so it structurally can't show whether a split's throughput impact
  *shrinks* as shard count grows -- that would need a random-key (or otherwise multi-hot-shard)
  variant of the same probe.

## 12. `defragment()` (T2 space reclamation) needs a full redesign
* **Status**: 🔴 **Not implemented** -- permanently no-op'd (round 1) then fully removed from the
  codebase (round 3); a real gap, not a resolved one. `docs/benchmark/20260823_maintenance_ops_priority_triage.md`.
* **Problem**: the original implementation couldn't keep up with Insert at `64KB/LTM` (0.4x its
  throughput) and collapsed concurrent-write TPS by up to 98% (effectively a 60s+ stall) -- the
  same "maintenance work competing with the foreground write path" failure mode `ShardedT1Index`'s
  background-worker-pool design was built to avoid for T1.
* **Direction**: redesign around the same principle that worked for T1 sharding (dedicated
  background worker pool + per-region/per-shard scoped backpressure, never the accessing thread
  doing the compaction itself) rather than reviving the old implementation as-is. Not yet scoped.
