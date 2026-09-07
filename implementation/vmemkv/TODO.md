# VMemKV Implementation Plan & Remaining TODO List

This document outlines the roadmap to implement the full, robust architecture of VMemKV based on the academic specifications (HLD/LLD), supporting both disk-efficient In-place updates and crash-resilient Write-Ahead Logging (WAL).

---

## 3. Startup Swap Validation Check
* **Status**: 🟢 **Implemented** -- `VMemKVImpl` construction runs `validate_swap_for_ltm()`
  (`src/core/swap_check.hpp`) first: warns once per process when the host has no swap, throws
  when `VMEMKV_REQUIRE_SWAP_BYTES` names a floor the host does not meet. Silent without
  `/proc/meminfo` (non-Linux). Covered by `tests/test_swap_check.cpp` (fake meminfo files).

## 5. Scan-side offset-order read reordering (no storage compaction)
* **Status**: 🟢 **Implemented** -- `VMemKVImpl::scan_impl()` reads each bounded batch (128
  entries) in ascending T2 physical-offset order and invokes the caller's callback back in T1
  key order. No writer-stop, no space reclaimed, purely Scan-internal. Callback order and
  `total_count` semantics unchanged; covered by `scan after churn returns key order with latest
  values` (`tests/test_kv_store.cpp`) plus all pre-existing scan tests.
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
* **Landed**: a self-throttling mechanism in `bulk_load_impl()` (`src/vmemkv_impl.hpp`): a
  `CgroupMemoryThrottle` (`src/core/cgroup_memory_throttle.hpp`) samples the enclosing cgroup v2
  `memory.current` against `memory.high` every 1024 keys and sleeps briefly near the limit (2ms
  at/over budget, 200µs past 95%), lowering the dirtying rate so synchronous direct reclaim keeps
  up. Inactive without a cgroup v2 limit (negligible cost); unit-tested with fake cgroup dirs
  (`tests/test_cgroup_throttle.cpp`); overridable via `VMEMKV_CGROUP_ROOT`.
* **Next step**: validate on AWS with the `target_ratio` sweep from
  `docs/benchmark/20260829_ltm_full_corpus_thrashing_investigation.md` (`<=1.5` safe / `>=1.65`
  hangs pre-fix) -- local machines lack the cgroup setup to reproduce the stall.

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

## 11. Validate organic-split impact with non-monotonic keys
* **Status**: 🟡 **Probe implemented, AWS run pending**. `bench_kv.cpp --key-pattern=random`
  (per-thread seeded draws over a 2^48 space, corpus still grows) plus
  `run_organic_split_probe.sh`'s optional 5th arg spread writes over all shards; results carry a
  `key_pattern` field the report pipeline ignores harmlessly. Default stays `monotonic`, so all
  existing outputs are unchanged.
* **Next step**: run both patterns on AWS and compare per-split degradation vs. shard count in
  the "Organic Per-Shard Splits" report section.

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
