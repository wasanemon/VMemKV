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

Source: `benchmark_results/pages/2026081000_charts.html` Winners Matrix (threads:32, fixed vs. fastest rival). Analysis done 2026-08-11; no AWS work started yet on any of these.

* **Status**: 🟡 **In Progress** -- 4.2 and 4.3 improved (see below), 4.1/4.4/4.5 still analysis only.

### 4.1 Get (Hit) 64KB LTM -- 0.38x-0.51x vs RocksDB / RocksDB-BlobDB
* Likely cause: RocksDB-BlobDB stores large values in a dedicated blob file and reaches them via a single direct `pread` at a known offset -- effectively the same idea `base_mmap` is going for, but purpose-built. VMemKV still pays ~16 page faults per 64KB record via mmap even with the `ScanBaseSequential`/base_mmap fast path.
* Candidate countermeasures:
  - Explicit `pread`/`readv` for large-value records instead of relying on mmap page faults.
  - Tune NVMe `read_ahead_kb` to match large-record size.
  - Dedicated large-value file/mapping designed like BlobDB's blob files.

### 4.2 Scan (Zipf/Uniform) 8B In-Memory -- 0.63x-0.64x vs LMDB
* **Status**: 🟢 **Improved, not fully closed** (commit 7991345 on main, 2026-08-12)
* Root cause confirmed: SimdScan is a red herring (only accelerates T1's small append region, never the sorted region range scans dominate). The real asymmetry vs Get (which already beats LMDB here) is that Scan must reconstruct and *deliver* the key per record, while Get doesn't (caller already has it) -- plus `T1Index::scan()` unconditionally buffered every sorted-region match into a `candidates` vector for a merge/dedup pass even when there was nothing to merge (no concurrent append-region writes in range).
* Fix applied: (1) bit_width-based key trim instead of a byte loop, one key copy instead of two (`vmemkv_impl.hpp`); (2) `T1Index::scan()` streams directly from the sorted region when append regions can't overlap the query range, skipping the candidates buffer entirely, via a new shared `walk_sorted_region()` helper (no code duplication vs the existing slow path).
* Measured (local, alternating A/B, threads:20): Zipf +25-29%, Uniform +4-11%. Local ratio vs LMDB: Zipf 0.65x -> 0.81x. Gap not fully closed -- Uniform's smaller gain suggests memory-latency (cache-miss-bound random access), not CPU overhead, dominates there, which this fix doesn't address.
* Not yet verified on AWS/real concurrency scale (local dev box, threads:20 not 32).

### 4.3 Scan (Zipf) 64KB LTM -- 0.49x vs LMDB
* **Status**: 🟢 **Fixed, now winning** (0.49x -> 1.08x vs LMDB, AWS i4i.8xlarge, 2026-08-12)
* Re-measurement (threads:32, 1GiB budget, isolated cgroup): Zipf ratio 0.49x -> **0.55x** (4.2's `walk_sorted_region()` fast path is in the shared `T1Index::scan()` path, so it applies here too, for free). Uniform is now essentially at parity (1.05x, wasn't a red badge before either).
* New finding: comparing each store's *own* Zipf/Uniform ratio at 64KB LTM -- LMDB itself is 3.45x faster on Zipf than Uniform (2,526/s vs 732/s), while VMemKV is only 1.80x faster (1,381/s vs 767/s) for the exact same corpus/access pattern. Both benefit from repeated-hot-range skew (page cache reuse), but LMDB benefits *much* more.
* Also worth remembering going in: LMDB is not "in-memory" -- it's natively an mmap'd, disk-backed B+tree, so "LTM" isn't a special mode for it the way it is for VMemKV (T1 index + T2 mmap + WAL + checkpoints as separate layers all sharing one memory budget). LMDB has ~15+ years of engineering specifically for "huge mmap'd file under real memory pressure" as its core use case; VMemKV's LTM support (ScanBaseSequential etc.) is comparatively new.
* **Root cause found** (perf stat + /proc/diskstats profiling, AWS i4i.8xlarge, 2026-08-12, 4 legs: {vmemkv,lmdb} x {zipf,uniform} @ threads:32): normalized per benchmark iteration (one iteration = one `scan_count_reorg`=100-record window = 6.25MiB logical), actual bytes pulled from the block device via `/proc/diskstats` sector deltas:
  | | major-faults/iter | disk bytes/iter | vs 6.25MiB logical window | bytes/major-fault |
  |---|---|---|---|---|
  | VMemKV Zipf | 24.3 | 9.10 MiB | 1.46x | 383 KB |
  | VMemKV Uniform | 62.7 | 22.37 MiB | 3.58x | 365 KB |
  | LMDB Zipf | 21.8 | 5.96 MiB | 0.95x | 280 KB |
  | LMDB Uniform | 71.8 | 21.62 MiB | 3.46x | 308 KB |

  Under Uniform (essentially all-cold access) both stores are close (~3.5x the logical window, i.e. similar absolute read amplification). The gap is specific to Zipf: LMDB reads *less* than the logical window (0.95x -- most of the hot range is already page-cache resident), while VMemKV still reads 1.46x the logical window even on repeated/hot access. Consistently across both distributions, VMemKV pulls ~25-30% more bytes per major fault than LMDB (383KB vs 280KB on Zipf, 365KB vs 308KB on Uniform). This is explained by `vmemkv_impl.hpp:1470`: the `ScanBaseSequential` second mapping is unconditionally `madvise(MADV_SEQUENTIAL)`'d, which makes the kernel's readahead window wide regardless of distribution. Wide readahead is a good trade under Uniform (next access is elsewhere anyway) but is pure waste under Zipf, where the win comes from a *narrow* hot range staying resident -- the extra readahead bytes rarely get reused before eviction, and their fetch/cache-pressure cost eats into the skew's benefit. This is why VMemKV's own Zipf/Uniform speedup (1.80x) undershoots LMDB's (3.21x measured in this profiling run, consistent with the 3.45x figure above) even though both benefit from the same corpus-level skew.
* **First fix attempt (reverted): removing `MADV_SEQUENTIAL` outright regressed Get.** Blanket-removing the `madvise(MADV_SEQUENTIAL)` on `ScanBaseSequential`'s base_mmap fixed Scan (Zipf ratio vs LMDB 0.49x -> 1.08x, own Zipf/Uniform speedup 1.80x -> 3.67x) but was never actually scoped to Scan alone: `try_read_base_record()` -- the function this mapping feeds -- is shared by `get_impl()` too (extended there per `docs/benchmark/20260810_t2_no_madvise_random.md`, since a 64KB record spans ~16 pages and wants that batched in one readahead). A broader AWS A/B (Get/Hit and Scan, x Zipf/Uniform, x LTM/In-Memory, threads:32, before=HEAD vs after=this fix) caught it: Get/Hit under LTM regressed **0.749x (Zipf) and 0.478x (Uniform)** -- Uniform got cut in half. In-Memory (no memory pressure) was unaffected for both ops in every case, confirming the mechanism is entirely about LTM/swap-pressure readahead behavior. This change was reverted (not committed) -- the Scan win didn't come close to justifying halving Get.
* **Actual fix: split into two separate mappings of the same base region, one per access pattern.** madvise is per-VMA, and the codebase already had precedent for this (the base_mmap/`ScanBaseSequential` split itself exists specifically because Get/Update's primary mapping needed `MADV_RANDOM` while Scan wanted something else). Applied the same trick one level deeper: `T2Memory::base_mmap` (unchanged, `MADV_SEQUENTIAL`, read by `get_impl()`) and a new `T2Memory::base_mmap_scan` (kernel default/no madvise, read by `scan_impl()`) -- both PROT_READ|MAP_PRIVATE mappings of the identical `[0, capacity)` region, independently advised, sharing the same underlying page-cache pages. `try_read_base_record()` takes a `for_scan` bool to pick which mapping to read through. ~60 lines added across `t2_flat_file.hpp` (new field + destructor unmap), `vmemkv_impl.hpp` (second mmap block in `mmap_t2_memory()`, call-site routing), `config.hpp` (doc comment). Full local test suite (340 cases + clang-tidy) passes.
* **Re-measured on AWS i4i.8xlarge** (same broad A/B methodology: Get/Hit and Scan, x Zipf/Uniform, x LTM/In-Memory, threads:32, before=HEAD 931f051 vs after=dual-mapping fix, one fresh instance so all 16 legs share the same baseline run):
  | Op | Dist | Mode | before/s | after/s | after/before |
  |---|---|---|---:|---:|---:|
  | Get/Hit | Zipf | LTM | 61,309.5 | 70,829.4 | **1.155x** |
  | Get/Hit | Uniform | LTM | 33,852.2 | 37,224.5 | **1.100x** |
  | Get/Hit | Zipf | In-Mem | 462,013.7 | 463,443.5 | 1.003x |
  | Get/Hit | Uniform | In-Mem | 432,219.6 | 435,470.9 | 1.008x |
  | Scan | Zipf | LTM | 1,317.6 | 2,764.1 | **2.098x** |
  | Scan | Uniform | LTM | 819.2 | 817.4 | 0.998x |
  | Scan | Zipf | In-Mem | 4,420.9 | 4,344.8 | 0.983x |
  | Scan | Uniform | In-Mem | 4,411.0 | 4,376.0 | 0.992x |

  Clean result: Get/Hit LTM is not regressed (actually mildly *up*, 1.10-1.16x, within run-to-run noise but clearly not worse), Scan/LTM/Zipf more than doubles (2.10x), and every In-Memory (no-memory-pressure) cell is ~1.0x as expected since this change only affects LTM/swap-pressure readahead behavior. This resolves 4.3 without the collateral damage the first attempt had.
* Not yet committed -- pending user confirmation.

### 4.4 YCSB-E 64KB LTM -- 0.60x vs RocksDB
* Likely cause: YCSB-E is scan-heavy, so this probably just inherits 4.3's weakness. Expect it to improve once 4.3 is fixed -- verify before investing separately.

### 4.5 Minor (lower priority)
* Scan (Zipf) 1KB LTM -- 0.88x vs RocksDB (light lose)
* YCSB-E 1KB LTM -- 0.91x vs RocksDB (light lose)

**Suggested order**: 4.3 (Scan 64KB LTM vs LMDB) next -- note the 4.2 fix's `walk_sorted_region()` fast path is in the *shared* `T1Index::scan()` code path, so it already applies to every value size, not just 8B. Worth re-measuring 4.3/4.4's current gap before investing in I/O-specific tuning (madvise/readahead), since some of it may already be closed for free. Then 4.1 (structurally harder -- BlobDB's design is purpose-built for this).

---

## 5. LTM/1KB Get(Hit) "regression" -- investigated, fix reverted, root cause still unknown

* **Status**: 🟡 **Inconclusive -- do not re-attempt the same fix without re-reading this first**
* **Background**: The `2026081000_charts.html` rawData appeared to show `+ScanBaseSequential` losing badly to `+Prefault` for LTM/1KB Get(Hit) (reported as ~605,876 -> ~93,990 items/s at threads:32 Zipf). Note `+Prefault` and `+ScanBaseSequential` are two different **compile-time** variants (cumulative feature stack), not a before/after of one code change.
* **Fix attempted (and reverted)**: added a `require_multi_page` runtime guard to `try_read_base_record()` so `get_impl()` skips the `base_mmap` fast path for records <= 4096B (single page), falling back to the primary mmap+seqlock path -- on the theory that `base_mmap`'s benefit only comes from batching multi-page reads.
* **AWS verification result (i4i.8xlarge, isolated cgroup, real 32-core)**: the guard fix made **no measurable difference** (Zipf@32: 127,150 -> 126,641 items/s; Uniform@32: 34,375 -> 34,414 items/s), even though debug instrumentation confirmed the guard fires on 100% of these Get calls. A second hypothesis (the `T2Memory` constructor eagerly `MADV_POPULATE_READ`-prefaulting the *entire* `base_mmap` region under `UsePrefaulting`, doubling memory pressure) was also ruled out by disabling that prefault and re-measuring -- still no change (Zipf@32: 129,310/s, Uniform@32: 38,834/s).
* **Open question**: the absolute numbers measured in this investigation (~127K items/s Zipf@32) don't match *either* endpoint of the originally reported 605,876/93,990 figures, under the exact same harness config (`run_bench_aws_c6id.sh`'s 1GiB memory budget, target_ratio 8.0, i4i.8xlarge). This means the original 2026081000 report's rawData cell for this comparison may itself need re-verification (possible anomaly/fluke run, or a config/commit mismatch) before chasing a fix further.
* **Next step if resumed**: re-run `+Prefault` and `+ScanBaseSequential` as actual compile-time variants side by side, on the same instance, under the same conditions, to confirm the regression is real and reproducible *before* attempting another fix. Don't reuse the `require_multi_page` guard approach without new evidence -- it's confirmed not to be the mechanism.
