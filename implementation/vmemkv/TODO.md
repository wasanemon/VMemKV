# VMemKV Implementation Plan & Remaining TODO List

This document outlines the roadmap to implement the full, robust architecture of VMemKV based on the academic specifications (HLD/LLD), supporting both disk-efficient In-place updates and crash-resilient Write-Ahead Logging (WAL).

---

## 8. `ShardedT1Index` single-shard "routing tax"
* **Status**: 🟡 **Partially mitigated, needs AWS re-measurement** -- found 2026-09-07,
  `docs/t1_sharding_design.md`.
* **Problem**: for a corpus small enough to never cross one shard's split threshold, every
  put/get/scan still pays `with_routing_guard()`'s directory-lookup/epoch-guard overhead with none
  of sharding's contention-spreading benefit to offset it. Measured up to ~30% Insert/Update
  throughput regression at high thread counts (`ltm/64KB`, 131K-key corpus) vs. the pre-sharding
  baseline.
* **Finding**: T1-only microbenchmarks put the single-shard tax at ~10ns/op, ~7ns of it the
  routing epoch guard itself. Bypassing `with_routing_guard()` while `shard_count() == 1` (the
  original idea) is **unsafe** and was rejected: an unguarded reader races the first split's
  reclamation (use-after-free, same class as three previously fixed bugs). Landed instead: a
  guard-preserving single-shard fast path (`resolve`/`resolve_for_write` skip the binary search,
  `scan` skips the leaf-vector allocation).
* **Next step**: re-measure end-to-end on AWS (`ltm/64KB`, 16/32 threads). At µs-scale per-op
  costs (64KB values + WAL) the residual ns-scale tax should be ~1%; if the ~30% gap persists,
  its cause lies outside the routing layer.

## 12. `defragment()` (T2 space reclamation) needs a full redesign
* **Status**: 🔴 **Not implemented** -- permanently no-op'd (round 1) then fully removed from the
  codebase (round 3); a real gap, not a resolved one. `docs/benchmark/20260823_maintenance_ops_priority_triage.md`.
* **Problem**: the original implementation couldn't keep up with Insert at `64KB/LTM` (0.4x its
  throughput) and collapsed concurrent-write TPS by up to 98% (effectively a 60s+ stall) -- the
  same "maintenance work competing with the foreground write path" failure mode `ShardedT1Index`'s
  background-worker-pool design was built to avoid for T1.
* **Direction**: redesign around the same principle that worked for T1 sharding (dedicated
  background worker pool + per-region/per-shard scoped backpressure, never the accessing thread
  doing the compaction itself) rather than reviving the old implementation as-is.
* **Design**: `docs/benchmark/20260907_t2_defragment_redesign.md` -- why the old whole-file
  rewrite + writer-stop design failed (numbers), the segment-scoped incremental background
  compaction direction, crash-safety ordering, phased plan (0: observability, 1: correctness
  gate, 2: background + interference <=20%, 3: bounded footprint), and acceptance criteria.
  Implementation starts at phase 0.
