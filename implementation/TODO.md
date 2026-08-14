# VMemKV Implementation Plan & Remaining TODO List

This document outlines the roadmap to implement the full, robust architecture of VMemKV based on the academic specifications (HLD/LLD), supporting both disk-efficient In-place updates and crash-resilient Write-Ahead Logging (WAL).

---

## 3. Startup Swap Validation Check
* **Status**: 🔴 **Not Implemented**
* **Objective**:
  - Implement a startup check in the main application / VMemKV initialization to verify that the system has an active Swap file of sufficient capacity (e.g., at least 64GB / 1TB on production environments) to prevent OOM Killer.
  - If a Swap file is not configured or fails validation under memory-constrained environments, raise an initialization warning/error or exit gracefully.

## 4. Profile the full-stall during a T2 rebuild (do_t2_rebuild=true)
* **Status**: 🔴 **Not Investigated**
* **Objective**:
  - `benchmark_results/2026073000/ycsb_e_timeline_in_memory_VMemKV-Bloom-Simd-T1InlineValue_Bloom-Simd-T1InlineValue_8B.json` shows total ops (scan+insert) dropping to **exactly 0 for one full second** (sec 27) around a `t2_reorg_ops=1` event (recorded at sec 28), immediately after a steep decline (sec 25: ~960K -> sec 26: ~128K), then fully recovering the very next second (sec 29: ~1.01M).
  - This is suspicious because `scan_impl()` is never gated by `writer_stop_`/`reorg_running_` (only `insert`/`update`/`delete` paths are), so a lock-based explanation doesn't fit a full stall of *both* scan and insert. The likely cause is system-wide resource saturation from the rebuild's own I/O (reading+copying the whole live corpus, fsync) rather than any locking mechanism -- but this is unconfirmed.
  - Profile a real `defragment()`/`checkpoint()`-triggered full T2 rebuild while a concurrent read/write workload runs (e.g. `perf record`, `iostat`/`pidstat` during the rebuild window, or per-thread wall-clock breakdown) to identify whether the stall is fsync-bound, disk-bandwidth-bound, CPU-bound (memcpy/compaction), or something else. `rebuild_t2_and_maybe_checkpoint()` has since been redesigned (single pre/post-stop scan passes instead of an open-ended convergence loop, T1 published exactly once at the end) for exception-safety, not for this stall -- profiling should confirm whether that redesign happens to help here too, or whether a separate fix (I/O pacing/rate-limiting the rebuild, `ionice`, incremental fsync) is still needed.

