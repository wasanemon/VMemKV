# VMemKV Implementation Plan & Remaining TODO List

This document outlines the roadmap to implement the full, robust architecture of VMemKV based on the academic specifications (HLD/LLD), supporting both disk-efficient In-place updates and crash-resilient Write-Ahead Logging (WAL).

---

## 3. Startup Swap Validation Check
* **Status**: 🔴 **Not Implemented**
* **Objective**:
  - Implement a startup check in the main application / VMemKV initialization to verify that the system has an active Swap file of sufficient capacity (e.g., at least 64GB / 1TB on production environments) to prevent OOM Killer.
  - If a Swap file is not configured or fails validation under memory-constrained environments, raise an initialization warning/error or exit gracefully.

---

## 4. Winners Matrix Red-Badge Countermeasures (vs RocksDB/-BlobDB/LMDB)

Source: `benchmark_results/pages/2026081000_charts.html` Winners Matrix (threads:32, fixed vs. fastest rival).

* **Status**: 🟢 Fixed (2026-08-13). Scan/Get's base-region reads (`ScanBaseSequential`) now pick
  per record, from that record's own embedded size hint, among three mechanisms: `base_mmap_scan`
  (kernel-default-readahead mmap, large records), `base_mmap_scan_seq` (MADV_SEQUENTIAL mmap,
  single-page-or-smaller records), and `read_fd` (bounded `pread()`, Get's large-record path).
  Uniform now wins or ties RocksDB/RocksDB-BlobDB/LMDB across the board; Zipf remains a light
  loss in two cells (4.5).

### 4.5 Minor (lower priority)
* Get (Hit) 64KB LTM Zipf -- 0.88x vs RocksDB/RocksDB-BlobDB (light lose; Uniform now wins)
* Scan (Zipf) 1KB LTM -- 0.88x vs RocksDB (light lose)
* YCSB-E 1KB LTM -- 0.91x vs RocksDB (light lose)

---

## 5. LTM/1KB Get(Hit) "regression" -- investigated, fix reverted, root cause still unknown

* **Status**: 🟡 **Inconclusive -- do not re-attempt the same fix without re-reading this first**
* **Background**: The `2026081000_charts.html` rawData appeared to show `+ScanBaseSequential` losing badly to `+Prefault` for LTM/1KB Get(Hit) (reported as ~605,876 -> ~93,990 items/s at threads:32 Zipf). Note `+Prefault` and `+ScanBaseSequential` are two different **compile-time** variants (cumulative feature stack), not a before/after of one code change.
* **Fix attempted (and reverted)**: added a `require_multi_page` runtime guard to `try_read_base_record()` so `get_impl()` skips the `base_mmap` fast path for records <= 4096B (single page), falling back to the primary mmap+seqlock path -- on the theory that `base_mmap`'s benefit only comes from batching multi-page reads.
* **AWS verification result (i4i.8xlarge, isolated cgroup, real 32-core)**: the guard fix made **no measurable difference** (Zipf@32: 127,150 -> 126,641 items/s; Uniform@32: 34,375 -> 34,414 items/s), even though debug instrumentation confirmed the guard fires on 100% of these Get calls. A second hypothesis (the `T2Memory` constructor eagerly `MADV_POPULATE_READ`-prefaulting the *entire* `base_mmap` region under `UsePrefaulting`, doubling memory pressure) was also ruled out by disabling that prefault and re-measuring -- still no change (Zipf@32: 129,310/s, Uniform@32: 38,834/s).
* **Open question**: the absolute numbers measured in this investigation (~127K items/s Zipf@32) don't match *either* endpoint of the originally reported 605,876/93,990 figures, under the exact same harness config (`run_bench_aws_c6id.sh`'s 1GiB memory budget, target_ratio 8.0, i4i.8xlarge). This means the original 2026081000 report's rawData cell for this comparison may itself need re-verification (possible anomaly/fluke run, or a config/commit mismatch) before chasing a fix further.
* **Next step if resumed**: re-run `+Prefault` and `+ScanBaseSequential` as actual compile-time variants side by side, on the same instance, under the same conditions, to confirm the regression is real and reproducible *before* attempting another fix. Don't reuse the `require_multi_page` guard approach without new evidence -- it's confirmed not to be the mechanism.
