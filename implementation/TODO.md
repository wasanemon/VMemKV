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

## 4. Standardizing on Thread-Safe Reorganize (Deprecating Fork)
* **Status**: 🟢 **Design Standardized (LLD Revision Needed)**
* **LLD Specification**:
  - The Low Level Design specified using `fork()` to build checkpoints in a child process (copy-on-write).
* **Consensus & Rationale**:
  - **Deprecate `fork()`**: Forking in multi-threaded C++ applications carries a high risk of deadlocks (e.g., if another thread holds a mutex at the fork point, it remains permanently locked in the child process). Additionally, `fork()` causes physical memory overcommit issues due to copy-on-write page replication under write-heavy workloads, and degrades OS portability.
  - **Prefer Thread-Safe In-Process Sync**: The refined `read_optimistic` (SeqLock) protocol inside `T1Index` already enables safe, concurrent, and lock-free reader operations. Reorganizing using internal coordination thread-locks is more resilient, portable, and memory-efficient.
* **Action Items**:
  - Revise the HLD/LLD documents to remove references to `fork()` during reorganize and formally standardize on the thread-safe background thread/mutex-exclusion model.

---

## 5. T1 Index Checkpointing (Fast Boot Recovery)
* **Status**: 🟡 **Deferred/Optional**
* **Specification Requirement**: 
  For very large stores (e.g., 256GB+), scanning the entire T2 file during boot can take several seconds.
* **Action Items**:
  - Design a checkpointing scheme to serialize T1's `SortedRegion` and `BloomFilter` to a dedicated metadata file (`t1_index.chk`) upon clean shutdowns.
  - Allow fast-boot recovery by loading the T1 checkpoint and only tailing/scanning the T2 file from the checkpoint timestamp.

---

## 6. Code Quality & CI Automation
* **Status**: 🔴 **Not Implemented**
* **Goal**: Establish automated gates for C++ style consistency, static analysis, and regression testing.
* **Action Items**:
  - **`clang-format` (Local Pre-commit Hook via CMake)**:
    - Introduce a `.clang-format` style configuration.
    - Set up a Git pre-commit hook in `githooks/pre-commit` to automatically run `clang-format -i` and re-stage C++ changes upon `git commit`.
    - Integrate a git hook configurations script inside `CMakeLists.txt` (using `core.hooksPath`) to automatically and dependency-free install the hook when developers run CMake.
  - **`clang-tidy` (CI-only Target)**:
    - Configure a `.clang-tidy` profile to flag memory safety issues, redundant copies, and C++20/C++23 code violations.
    - *Note on Hook Exclusion*: **Do NOT run `clang-tidy` in the local pre-commit hook**. Since `clang-tidy` compiles the AST (abstract syntax tree) and requires `compile_commands.json` dependencies, running it on commit introduces high latency (seconds to minutes) and breaks developer workflow loop. Instead, defer static analysis entirely to CI.
  - **GitHub Actions (CI)**: Set up a workflow (`.github/workflows/ci.yml`) to automatically trigger on push/PR to:
    1. Run `clang-format --dry-run` style checks.
    2. Run `clang-tidy` static analysis (where compile commands are stably built).
    3. Compile the repository under both Clang and GCC.
    4. Run all unit tests and verify they pass cleanly.
