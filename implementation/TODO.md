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
* **Follow-up: checkpoint-based benchmark corpus reuse** (blocked on this item):
  - While investigating why a local full benchmark run took 5.4 hours, `Delete`'s
    `reuse_store=false` pattern (a fresh full-corpus `populate()` before each of its 4
    thread-count measurements) turned out to be the dominant cost. The unsafe part of
    this (unbounded iteration count against a possibly-huge LTM corpus) is already
    fixed in `implementation/benchmark/bench_kv.cpp` -- `Delete` now runs a wall-clock
    time-bounded loop instead, so no cell can blow up regardless of how badly a given
    engine degrades, while still populating the *full* `corpus_size` each time to keep
    LTM scenarios genuinely larger-than-memory.
  - The remaining cost is `populate()` itself being rebuilt from scratch once per
    thread-count. Measured locally, VMemKV's own `populate()` was the single largest
    contributor across all four engines (514.6s across 4 calls for just Delete/1KB) --
    VMemKV has no `bulk_load_impl` equivalent and just fans out plain `store.insert()`
    calls across threads, unlike RocksDB/RocksDB-BlobDB/LMDB's batched bulk loaders.
  - Once this item lands (VMemKV can checkpoint/restore its own T1+T2 state from
    disk), apply the same snapshot-and-restore technique to **all four** engines in one
    pass: populate each `(store, value_size)` corpus once, snapshot it via each
    engine's checkpoint/restore primitive (RocksDB already has
    `rocksdb::Checkpoint::CreateCheckpoint`, hardlink-based and nearly free; LMDB just
    needs a file copy of its single `.lmdb` file; VMemKV would use the checkpoint file
    from this item), and restore from that snapshot instead of re-running `populate()`
    for every multi-thread-count benchmark group that currently uses
    `reuse_store=false`. Doing this piecemeal per-engine now would leave VMemKV's
    slowest path unaddressed and create two different benchmark methodologies to
    reconcile later.

---

## 3. Startup Swap Validation Check
* **Status**: 🔴 **Not Implemented**
* **Objective**:
  - Implement a startup check in the main application / VMemKV initialization to verify that the system has an active Swap file of sufficient capacity (e.g., at least 64GB / 1TB on production environments) to prevent OOM Killer.
  - If a Swap file is not configured or fails validation under memory-constrained environments, raise an initialization warning/error or exit gracefully.

---