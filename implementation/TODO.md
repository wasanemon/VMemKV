# VMemKV Implementation Plan & Remaining TODO List

This document outlines the roadmap to implement the full, robust architecture of VMemKV based on the academic specifications (HLD/LLD), supporting both disk-efficient In-place updates and crash-resilient Write-Ahead Logging (WAL).

---

## 1. Implement Write-Ahead Log (WAL) & Crash Recovery
* **Status**: 🔴 **Not Implemented**
* **Specification Requirement**: 
  - To support safe **In-place updates** on Tier 2 (already implemented in `VMemKVImpl::update_impl`) without risking data corruption (Torn Writes), a dedicated **Write-Ahead Log (WAL)** is required.
  - All write operations (`insert`, `update`, `delete`), including in-memory `InlineShort` and `Inline8B` mutations, must be sequentially appended to the WAL and `fsync`ed before client commit.
* **T2 Memory & Durability Alignment**:
  - **T2 Map Private**: With the WAL securely `fsync`ed, the main `T2FlatFile` database can safely remain mapped via `MAP_PRIVATE` (buffered in memory). Runtime disk synchronization I/O for T2 is 0.
  - **Crash Protection**: If a crash occurs during a T2 in-place update, the corruption is completely resolved upon recovery by playing back the WAL to rebuild the consistent memory state.
* **Action Items**:
  - Implement the `Wal` class handles sequential logging and crash-safe file flushes conforming to HLD/LLD structures (e.g., LLD 2.3).
  - Integrate WAL logging into `VMemKVImpl` write pipelines.
  - Implement the recovery parser to play back log records and restore active T1/T2 states.

---

## 2. Implement T1/T2 Checkpointing & WAL Truncation (No-Fork)
* **Status**: 🔴 **Not Implemented**
* **The Checkpoint Strategy (Fast Boot)**:
  - Periodically, or during `reorganize`, dump a stable T1 index checkpoint file (`t1_index.chk`) alongside a defragmented T2 database file to disk.
  - This allows **Fast Boot Recovery**: upon startup, the system loads the T1 checkpoint instantaneously and only scans the tail-end of the WAL (log entries created after the checkpoint LSN) to recover state, bypassing the need to scan the entire T2 file.
* **Deprecating Forking (LLD Revision Needed)**:
  - Standardize on **thread-safe, in-process background serialization** instead of the LLD-specified `fork()` (Copy-on-Write) model. This eliminates the risk of multi-threaded deadlocks (fork-safety issues) and OOM Killer page-replication overhead.
* **Action Items**:
  - Design the T1 index serialization interface (`t1_index.chk`).
  - Implement in-process thread-safe checkpointing and log truncation (`wal.truncate_before(checkpoint_lsn)`).
  - Update `docs/specification/low_level_design.md` to deprecate `fork()` in favor of thread-safe background threads.

---

## 3. Startup Swap Validation Check
* **Status**: 🔴 **Not Implemented**
* **Objective**:
  - Implement a startup check in the main application / VMemKV initialization to verify that the system has an active Swap file of sufficient capacity (e.g., at least 64GB / 1TB on production environments) to prevent OOM Killer.
  - If a Swap file is not configured or fails validation under memory-constrained environments, raise an initialization warning/error or exit gracefully.

---

## 4. Fix T1Index Scan Algorithmic Bottleneck (Full-sort Bug)
* **Status**: 🔴 **Not Implemented**
* **Issue**:
  - The current implementation of `T1Index::scan` copies the entire `sorted_region` into a temporary vector, merges it with the `append_region`, and performs a full `std::sort` ($O(N \log N)$) on every scan request.
  - This results in a massive CPU/memory bottleneck, dropping throughput to <100 ops/s when the total record count $N$ is large (e.g. Value=8B/1KB).
* **Action Items**:
  - Refactor `T1Index::scan` to conform to the Low-Level Design (LLD 3.5):
    1. Binary-search (`std::lower_bound`) the already-sorted `sorted_region` to extract only the range of entries between `lo` and `hi` ($O(\log S)$).
    2. Scan the small `append_region` to gather range candidates.
    3. Merge and deduplicate the two small candidate sets, achieving $O(\log S + A \log A)$ complexity.
  - Fix the performance drop in Scan benchmarks for 8B and 1KB value sizes.

## 6. Get(Hit) Borrowed Read Path with Versioned Validation
* **Status**: 🟡 **Planned**
* **Goal**:
  - Add a zero-copy `Get(Hit)` read path for large values by returning a borrowed view into T2 instead of always materializing `std::vector<std::byte>`.
  - Use a version-based validation loop (seqlock-style retry) to ensure the borrowed view is not observed across an in-place update boundary.
* **Action Items**:
  - Introduce a borrowed read API that can carry the T2 memory lifetime guard together with the value span.
  - Validate the record version before and after value access; retry if an update is observed in flight.
  - Restrict the borrowed path to read-only consumers so the existing copy-returning API can remain available when materialization is required.
