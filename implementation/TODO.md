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

## 3. Address Massive Clang-Tidy Warnings (1.2M+ Warnings)
* **Status**: 🔴 **Not Implemented**
* **Goal**: Address or suppress the astronomical number of clang-tidy warnings generated during static analysis.
* **Why this occurs**:
  - The massive warning count (1.2M+ warnings) is primarily due to standard library expansions inside templated core files (`vmemkv_impl.hpp`, `t1_index.hpp`), and strict, cumulative rules enabled by default in `.clang-tidy`.
* **Action Items**:
  - Fine-tune `.clang-tidy` rules to exclude system headers and standard library template expansions correctly (e.g., configure `HeaderFilterRegex` and disable noisy rules like `modernize-use-nodiscard` or `cppcoreguidelines-avoid-magic-numbers`).
  - Systematically clean up legitimate code-quality issues flagged in VMemKV core src files (e.g., `vmemkv_impl.hpp`, `t1_index.hpp`).
  - Add explicit `NOLINT` comments on false positives or unavoidable OCC concurrency mutations.

## 4. Introduce Background Reorganize thread
TBW. 現在はreornigazeが非同期実行されないのでinsertのベンチがappend_regionの限界(200万エントリ）で詰まる。それより細かい頻度でreorganizeするか、append_regionを広げるか、何かしらの措置が必要
