# VMemKV Implementation Gap & Remaining TODO List

This document compares the current code state against the academic specifications (High Level Design / Low Level Design) and physical durability trade-offs of VMemKV, identifying architectural gaps, pending implementations, and necessary design revisions.

---

## 1. Crash Recovery & Index Reload via T2 (Critical Spec Gap)
* **Status**: 🔴 **Not Implemented**
* **The Inline-Value Recovery Paradox**:
  - `InlineShort` and `Inline8B` optimizations store raw values directly within T1 (in-memory index) payload fields and completely bypass T2 disk writes.
  - Since T1 is purely volatile, a sudden crash wipes out all inlined values.
  - If recovery is done solely by scanning T2 records sequentially, **all inline values are permanently lost** because they were never committed to the T2 file.
* **Resolution (T2 Inline Logging Model)**:
  - Add a **`RECORD_FLAG_INLINE`** bit flag to `ValueRecordHeader::flags`.
  - When inserting an inlined value, still write a lightweight inline record (metadata header + raw 1-8B value) sequentially into T2, and call `fsync()`.
  - Upon recovery, scan T2 sequentially. If the `RECORD_FLAG_INLINE` flag is set, pack the value back into T1 payload instead of registering a T2 file offset.
  - During `reorganize` (compaction), these inline records will automatically be GC'ed (omitted from the new T2 file) because they are not referenced by offsets in T1.

---

## 2. Durability & the `MAP_PRIVATE` Conundrum
* **Status**: 🔴 **Design Inconsistency**
* **The Conundrum**:
  - The persistent file mapping `T2FlatFile` currently uses `MAP_PRIVATE` for write operations on the memory-mapped region.
  - Under `MAP_PRIVATE`, memory updates are copy-on-write (COW) and are **never synchronized back to the underlying storage media** by the OS kernel automatically.
  - Thus, unless a `reorganize` (checkpoint) merges memory changes back to disk sequentially, any updates to T2 are lost on crash/reboot.
* **Resolution (Asymmetric Read-Write Model)**:
  - **Write path**: Completely bypass mmap writes. Perform T2 appends via standard `write()` system calls, followed by `fdatasync()` / `fsync()` to guarantee transaction durability before returning a commit to the client.
  - **Read path**: Keep the read path lock-free by accessing the file via `mmap(MAP_PRIVATE | PROT_READ)`.

---

## 3. Specification (HLD / LLD) Revisions
* **Status**: 🔴 **Not Implemented**
* **Task**: Update `docs/specification/high_level_design.md` and `low_level_design.md` to reflect these major architectural changes:
  - **Remove WAL**: Replace references to a separate `Wal` structure and double-write logging with the asymmetric T2-as-WAL (No-Double-Write) model.
  - **Inline Logging**: Document the `RECORD_FLAG_INLINE` mechanism.
  - **mmap Avoidance on Write**: Document why mmap writing is deprecated (citing Andy Pavlo's *mmap-in-DBMS* limitations) and how T2 is read via `MAP_PRIVATE` but written via `write()/fsync()`.

---

## 4. Fork-Based Checkpointing vs. Thread-Safe Reorganize
* **Status**: 🟡 **Specification Divergence**
* **LLD Specification**:
  - The Low Level Design specifies that `reorganize` should create a child process via `fork()` (Copy-on-Write) to compile the new T2 file in the background while the parent continues servicing client requests.
* **Current State**:
  - The current codebase uses a simpler thread-safe mutex-exclusion block (`reorganize_mutex_`) to rebuild T2 inline.
* **Action Items**:
  - Evaluate if the complexity of fork-based background execution is necessary, or update the LLD to standardize on the thread-safe in-process mutex approach.

---

## 5. T1 Index Checkpointing (Fast Boot Recovery)
* **Status**: 🟡 **Deferred/Optional**
* **Specification Requirement**: 
  For very large stores (e.g., 256GB+), scanning the entire T2 file during boot can take several seconds.
* **Action Items**:
  - Design a checkpointing scheme to serialize T1's `SortedRegion` and `BloomFilter` to a dedicated metadata file (`t1_index.chk`) upon clean shutdowns.
  - Allow fast-boot recovery by loading the T1 checkpoint and only tailing/scanning the T2 file from the checkpoint timestamp.
