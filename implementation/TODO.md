# VMemKV Implementation Plan & Remaining TODO List

This document outlines the roadmap to implement the full, robust architecture of VMemKV based on the academic specifications (HLD/LLD), supporting both disk-efficient In-place updates and crash-resilient Write-Ahead Logging (WAL).

---

## 3. Startup Swap Validation Check
* **Status**: 🔴 **Not Implemented**
* **Objective**:
  - Implement a startup check in the main application / VMemKV initialization to verify that the system has an active Swap file of sufficient capacity (e.g., at least 64GB / 1TB on production environments) to prevent OOM Killer.
  - If a Swap file is not configured or fails validation under memory-constrained environments, raise an initialization warning/error or exit gracefully.

---

## Report: distinguish forced vs. natural T1 reorg in YCSB-E timeline charts
* **Status**: 🔴 **Not Implemented**
* **Background**: `bench_kv.cpp`'s YCSB-E benchmark now forces a T1-only `reorganize(false)`
  once, deterministically, at the t=15s mark of the 30s timeline window (see the
  `forced_reorg_done` block in the YCSB-E benchmark body). This exists because, under LTM
  scenarios, VMemKV's natural reorg trigger is unreliable within the 30s window (the append
  region's soft-limit threshold is often never reached at LTM's much lower throughput), so
  those charts previously showed no reorg vertical line at all. The forced reorg's count is
  tracked in a separate JSON field, `t1_forced_reorg_ops`, alongside the pre-existing
  `t1_reorg_ops` (natural) and `t2_reorg_ops` fields in each `ycsb_e_timeline_*.json`'s
  per-second `timeline` entries -- the two are mutually exclusive per second (the forced
  call's delta is deliberately absorbed out of `t1_reorg_ops` at the moment it fires, so no
  double counting).
* **Objective**: When the chart-generation scripts next regenerate a report page from a run
  that includes this field (i.e. any run captured after this TODO was written), update the
  chart JS (`reorgLinesPlugin` / `makeYcsbTimelineConfig`, in the generated
  `benchmark_results/pages/*_charts.html`) to draw the forced-reorg vertical line(s) in a
  visually distinct color/style from the natural-reorg line(s), instead of merging both into
  the current single `reorgSecs` (currently computed purely from `t1_reorg_ops > 0`). Also
  update `benchmark_results/pages/2026072900_charts.html`'s data-generation scripts
  (`gen_chart_data.py`'s `build_timeline_data()`) to pass the new field through once a run
  with it exists -- the 2026072900 data predates this change and has no
  `t1_forced_reorg_ops` field.