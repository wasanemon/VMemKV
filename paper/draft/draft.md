Draft status: Evaluation-free first full draft. No empirical results included.

This draft follows the uploaded drafting brief. 
Repository grounding: the planning documents define the central evaluation question as whether VMemKV can combine OS-delegated larger-than-memory management with T1/T2 responsibility separation, without claiming universal dominance over RocksDB or other KVSs.

---

# Title

## Title candidates

1. **VMemKV: OS-Delegated Larger-than-Memory Value Residency with a T1/T2 Key-Value Store Split**
2. **Separating Metadata Control from Value Residency: A Two-Tier mmap-Backed Key-Value Store**
3. **VMemKV: A Standalone Key-Value Store with RAM-Resident Control and mmap-Backed Larger-than-Memory Values**

## Chosen title

# VMemKV: OS-Delegated Larger-than-Memory Value Residency with a T1/T2 Key-Value Store Split

[日本語コメント]
狙い: タイトルで OS 委譲と T1/T2 責務分離を同時に出す。
対応する評価: E1 larger-than-memory behavior, E3 T1/T2 breakdown, E5 mmap-only microbaseline, E7 responsibility table.
根拠: planning repo は中心主張を OS 仮想メモリ委譲と T1/T2 split に置いている。
repoで確認すべき点: 現行実装が `mmap`, `fork`, `mincore`, `madvise` のどこまでを実際に使っているか。
過剰主張の注意: “fastest” や “solves larger-than-memory KVS” といった万能主張を避ける。

---

# Abstract

Key-value stores increasingly face workloads in which value bytes are much larger than the RAM budget available to the storage engine. Existing engines address this problem through mature but responsibility-heavy mechanisms such as LSM-tree compaction, value-log garbage collection, or explicit buffer-pool management. This paper presents **VMemKV**, a standalone key-value store that explores a different design point: delegate larger-than-memory value residency as much as possible to operating-system virtual memory mechanisms, while retaining explicit engine control over hot metadata. VMemKV separates a RAM-resident **T1** layer, which stores fixed-size key metadata, lookup state, scan control, tombstones, and T2 offsets or inline values, from an mmap-backed **T2** layer, which stores variable-size key/value records and relies on the OS for page residency. Reorganization, checkpoint reload, and WAL replay are designed around this T1/T2 split so that VMemKV can repair ordering and storage fragmentation without making LSM-style multi-level compaction the central mechanism. VMemKV does not claim that mmap always replaces a database buffer manager, nor that it outperforms production LSM engines on all workloads. Instead, the evaluation is designed to characterize when OS-delegated value residency is practical, when it breaks down, and how much implementation responsibility the T1/T2 split avoids relative to LSM-tree, value-log, mmap-B-tree, and explicit-buffer-manager designs.

[日本語コメント]
狙い: 評価結果なしで論文全体の thesis を固定する。
対応する評価: E1, E2, E3, E4, E5, E7.
根拠: evaluation plan は「全 workload で最速」ではなく成立条件と限界を明らかにする方針を明記している。 また high-level design は T1, T2, WAL, checkpoint, background jobs を構成要素としている。
repoで確認すべき点: WAL/checkpoint/recovery は設計文書にはあるが、現行コードでは checkpoint-free coordinator という記述があるため、実装済み範囲の確認が必要。
過剰主張の注意: “We show that” を避け、characterize / investigate / evaluate whether に留める。

---

# 1. Introduction

Key-value stores are often evaluated as lookup engines, but larger-than-memory value management turns them into full storage engines. Once the value layer exceeds DRAM, the engine must decide which bytes remain resident, how cold bytes are fetched, how writes interact with dirty page writeback, how obsolete versions are reclaimed, how scans avoid pathological layout, and how recovery reconstructs a consistent state after failure. These responsibilities are not incidental implementation details; they shape the architecture of the KVS.

[日本語コメント]
狙い: 問題設定を「mmap を使いたい」ではなく「larger-than-memory value 管理の責務」から始める。
対応する評価: E1 larger-than-memory behavior, E4 reorganization/checkpoint, E7 responsibility table.
根拠: outline は larger-than-memory values が buffering, eviction, write amplification, fragmentation repair, recovery を要求する流れから導入する方針を示している。
repoで確認すべき点: 現行 benchmark が larger-than-memory 条件を実際に含むか。
過剰主張の注意: OS 委譲が常に良いとは書かない。

Production KVSs have converged on several successful answers. LSM-tree engines such as Bigtable, LevelDB, and RocksDB absorb writes into memory, persist immutable sorted files, and use compaction to maintain lookup and scan efficiency [Bigtable, LevelDB, RocksDB]. Key-value separation systems such as WiscKey and BlobDB reduce the cost of moving large values through the LSM hierarchy by placing values in a separate log-like layer [WiscKey, BlobDB]. Bitcask-like systems use an in-memory key directory over append-only data files, making point lookup simple but making ordered scan and merge management more constrained [Bitcask]. Buffer-manager-oriented systems such as LeanStore and vmcache retain explicit DBMS control over larger-than-memory data placement [LeanStore, vmcache]. mmap-based systems such as LMDB delegate more address translation and file caching to the OS, but the database literature has also documented the risks of page-fault latency, eviction unpredictability, dirty writeback, and TLB shootdowns [LMDB, AreYouSureMMAP].

[日本語コメント]
狙い: 既存手法を strawman にせず、それぞれ成功している設計として扱う。
対応する評価: E2 RocksDB/BlobDB comparison, E5 mmap-only microbaseline, E7 responsibility table.
根拠: contribution plan は LSM, WiscKey/BlobDB, LMDB, vmcache との差分をこの軸で整理している。
repoで確認すべき点: must_read_papers には WiscKey が明示されており、VMemKV との近さが記されている。
過剰主張の注意: 既存システムを複雑すぎるだけのものとして扱わない。

VMemKV explores another point in this design space. Its central idea is to split responsibilities between two layers. **T1** is a RAM-resident metadata and control plane: it stores compact key metadata, payload bits, tombstones, lookup/scan control, and optional small-value coverage. **T2** is an mmap-backed value and residency plane: it stores variable-size key/value records in a file-backed byte region, and the OS is allowed to manage page residency for values. The novelty is not key-value separation alone. The paper’s thesis is that key-value separation becomes a different storage-engine design point when hot metadata remains under explicit engine control while larger-than-memory value residency is delegated to OS virtual memory.

[日本語コメント]
狙い: T1/T2 split を単なる value separation ではなく responsibility split として定義する。
対応する評価: E3 T1/T2 breakdown, E5 mmap-only microbaseline.
根拠: high-level design は Tier 1 を RAM 常駐 index、Tier 2 を file-backed mmap data layer と定義している。 low-level design は T1 entry と T2 record layout を定義している。
repoで確認すべき点: 現行コード上の T1 slot layout と design doc の `IndexEntry` が完全一致するか。
過剰主張の注意: key-value separation 自体を新規性として主張しない。

This split is intended to reduce the amount of DB-side machinery needed for larger-than-memory values. VMemKV does not implement a user-space buffer pool as the central residency mechanism, nor does it rely on LSM-style multi-level compaction as the primary repair mechanism. Instead, the OS handles much of T2 value residency, while VMemKV uses T1 reorganization, T2 reorganization, checkpoint reload, and WAL replay to repair fragmentation and reconstruct state. In-place update is an important auxiliary feature: when a new value fits the existing T2 allocation, VMemKV can update the record in place; otherwise it appends a new T2 record and updates T1’s offset. This can reduce garbage generation for fixed-size or bounded-growth update-heavy workloads, but it is not the central contribution.

[日本語コメント]
狙い: simplicity claim を responsibility reduction として表現する。
対応する評価: E4 reorganization/checkpoint behavior, E7 responsibility table.
根拠: contribution plan は reorganization/checkpoint/WAL replay を LSM-style multi-level compaction の代替中心機構として位置づけ、ただし compaction/GС を完全排除とは書かない方針を示している。 in-place update 条件は high-level design と low-level design の両方にある。
repoで確認すべき点: 現行実装では WAL/checkpoint が design-level か実装済みかを確認する。
過剰主張の注意: “compaction eliminated” ではなく “LSM-style multi-level compaction is not the central mechanism” とする。

VMemKV is intentionally scoped as a standalone KVS. It supports Get, Insert, Update, Delete, and Scan as the operations considered in this paper. It is not a transactional DBMS, does not provide a SQL layer, does not address phantom avoidance, and does not include replication. Although VMemKV may eventually be used below systems such as LineairDB or Kamo, this paper does not claim to solve their concurrency-control problem.

[日本語コメント]
狙い: 査読者が transaction, SQL, replication を要求する前に scope を切る。
対応する評価: E0 fairness, E6 recovery sanity check, E7 responsibility table.
根拠: evaluation plan は standalone KVS として Get/Insert/Update/Delete/Scan, larger-than-memory, T1/T2, reorganization, checkpoint reload, WAL replay を対象にし、transactions/SQL/phantom/LineairDB/Kamo/replication を対象外としている。 high-level design も API と non-goals を示している。
repoで確認すべき点: 現行 API が paper scope の Scan/Delete/Get/Insert/Update をすべて安定提供するか。
過剰主張の注意: LineairDB/Kamo の concurrency-control contribution にしない。

This paper makes five contributions. First, it presents OS-delegated larger-than-memory value residency as a standalone KVS design point. Second, it develops the T1/T2 responsibility split: T1 retains hot metadata and control, while T2 stores mmap-backed value bytes. Third, it describes reorganization, checkpoint reload, and WAL replay tailored to that split, without making LSM-style multi-level compaction the central mechanism. Fourth, it frames simplicity as an auditable comparison of storage-engine responsibilities rather than a subjective claim. Fifth, it includes in-place update as an auxiliary feature for update-heavy workloads whose values fit existing allocations.

[日本語コメント]
狙い: contribution を 5 点に整理し、in-place update を最後の補助的 contribution に置く。
対応する評価: E1–E7 全体。
根拠: contribution plan は OS 委譲、T1/T2 split、fragmentation repair/recovery、implementation responsibility comparison を主 contribution として整理している。
repoで確認すべき点: contribution 3 は実装済みではなく design-level の可能性がある。
過剰主張の注意: “simple implementation” は責務表で auditable にする。

The planned evaluation is designed to answer three broad questions. Can OS-delegated value residency provide practical behavior under larger-than-memory conditions? Does the T1/T2 split matter beyond the raw cost of mmap-backed value access? How much storage-engine responsibility is shifted away from VMemKV compared with LSM-tree engines, value-log engines, and explicit buffer-pool systems? The evaluation compares VMemKV with RocksDB under matched memory and durability settings, includes RocksDB BlobDB for large values, uses a mmap-only microbaseline to isolate value-access cost, and measures reorganization, checkpoint, and recovery behavior. No result is claimed in this draft.

[日本語コメント]
狙い: 評価につながる問いを提示しつつ結果を断定しない。
対応する評価: Section 8 full plan.
根拠: evaluation plan は 8.1–8.8 の構成、RocksDB/BlobDB, mmap-only baseline, reorganization/checkpoint, recovery, responsibility table を明記している。
repoで確認すべき点: BlobDB support と YCSB support は現行実装 repo で未確認。
過剰主張の注意: “We show” を使わない。

---

# 2. Background and Motivation

## 2.1 LSM-tree KVSs and compaction

LSM-tree KVSs organize writes around memory-resident buffers and immutable sorted files. A write is typically recorded in a WAL, inserted into a MemTable, flushed into an SSTable, and later merged through compaction. This design is robust and production-proven: it converts random writes into sequential file creation, supports ordered iteration through sorted runs, and provides a flexible framework for compression, Bloom filters, snapshots, and recovery. Bigtable introduced many of the ideas later embodied in LevelDB and RocksDB [Bigtable, LevelDB, RocksDB].

[日本語コメント]
狙い: LSM を強い baseline として尊重して導入する。
対応する評価: E2 YCSB RocksDB comparison, E7 responsibility table.
根拠: planning files identify RocksDB/LevelDB/Bigtable as LSM-tree comparison targets.
repoで確認すべき点: RocksDB options and durability settings for fair comparison.
過剰主張の注意: LSM-tree を単に悪い設計として扱わない。

The cost is that compaction becomes a central storage-engine responsibility. It controls space amplification, read amplification, write amplification, and scan layout, but it also introduces scheduling complexity and configuration sensitivity. For large values, compaction can be especially costly if values are repeatedly moved through levels or rewritten with keys. Production systems expose many options to tune compaction, caching, write buffering, and I/O behavior because no single configuration dominates all workloads.

[日本語コメント]
狙い: VMemKV の simplicity story に向けて LSM の責務を明確化する。
対応する評価: E2 write amplification/storage usage, E7 responsibility table.
根拠: evaluation plan は RocksDB comparison で logical bytes, WAL bytes, device bytes, device write amplification, engine write amplification を測る予定。
repoで確認すべき点: RocksDB baseline settings, direct/buffered I/O, compression, WAL/sync policy.
過剰主張の注意: RocksDB が大きな value に常に弱いとは書かない。BlobDB を比較に入れる。

## 2.2 Key-value separation and value logs

Key-value separation reduces value movement by keeping large values outside the LSM tree. WiscKey stores keys and value pointers in the LSM tree and places values in a separate value log [WiscKey]. BlobDB brings a related idea into RocksDB by storing large values as blobs outside the normal LSM-managed key path [BlobDB]. These designs address an important weakness of conventional LSM storage for large values: they avoid repeatedly compacting value bytes with keys.

[日本語コメント]
狙い: VMemKV に最も近い関連研究を正面から扱う。
対応する評価: E2 RocksDB BlobDB comparison.
根拠: must_read_papers README は WiscKey を VMemKV に近い「value pointer in LSM + append-only value file」方式として位置づけている。 evaluation plan は 16 KiB values で BlobDB を含める理由を明記している。
repoで確認すべき点: BlobDB 実験設定が実装・script に入っているか。
過剰主張の注意: VMemKV の value separation が WiscKey より常に優れるとは言わない。

However, key-value separation does not eliminate storage-engine responsibility; it moves much of it into value-log garbage collection, crash consistency between index and value log, and range-scan trade-offs. If values are appended out of key order, range scans may require random value-log reads. If old values accumulate, GC must identify live values, copy them, and update pointers safely. VMemKV shares the intuition that large values should be separated from hot key metadata, but it differs by making the separated value layer mmap-backed and by treating OS-managed residency as a central design choice.

[日本語コメント]
狙い: WiscKey/BlobDB との差分を “GC と OS residency” の軸で出す。
対応する評価: E4 reorganization/checkpoint, E5 mmap-only microbaseline, E7 responsibility table.
根拠: contribution plan contrasts WiscKey/BlobDB value-log GC with VMemKV’s T1/T2 reorganization.
repoで確認すべき点: VMemKV の T2 reorganization が value-log GC と同等に live record copy を行う範囲。
過剰主張の注意: VMemKV も reorganization という GC-like operation を持つ。

## 2.3 Append-only log plus in-memory index designs

Bitcask-like systems use an in-memory key directory that maps keys to offsets in append-only data files [Bitcask]. This design is attractive because point lookup is simple: consult memory, then read the record at the stored offset. Writes are append-friendly, and merge processes reclaim obsolete data. For workloads dominated by point operations and where the key directory fits in RAM, this architecture can be compact and understandable.

[日本語コメント]
狙い: Bitcask を “simple point lookup design” として認め、VMemKV の比較対象にする。
対応する評価: E7 responsibility table.
根拠: evaluation plan positions Bitcask-like log + in-memory index as a design comparison target.
repoで確認すべき点: responsibility table に Bitcask-like KVS を含めるか。
過剰主張の注意: Bitcask を obsolete な設計として扱わない。

The limitation is ordered access. A key directory optimized for point lookup does not automatically provide efficient range scan ordering unless it is supplemented with sorted metadata or separate index structures. VMemKV borrows the spirit of a memory-resident directory over value storage, but T1 is not merely a hash table: it contains a sorted region and an append region, allowing it to control lookup and scan order while T2 stores value bytes.

[日本語コメント]
狙い: T1 の sorted_region / append_region が Bitcask keydir と違う点を示す。
対応する評価: E3 T1/T2 design breakdown, E4 reorganization and scan before/after.
根拠: high-level design defines sorted_region and append_region, with sorted lookup O(log N) and append write O(1).
repoで確認すべき点: Scan ordering guarantee and required reorganize timing.
過剰主張の注意: Scan が常に速いとは言わない。

## 2.4 mmap and the OS page cache

mmap can make a storage engine simpler by mapping file-backed bytes into the process address space. Instead of issuing explicit pread calls and managing a user-space buffer pool for value pages, the engine dereferences mapped addresses and lets the kernel handle page faults, cache residency, and writeback. This is particularly appealing when values are large, addressable by offsets, and not all resident in DRAM at once.

[日本語コメント]
狙い: mmap 採用の自然な動機を説明する。
対応する評価: E1 larger-than-memory behavior, E5 mmap-only microbaseline.
根拠: README states VMemKV delegates I/O management to OS virtual memory mechanisms including mmap/fork/mincore. high-level design also lists mmap/fork/mincore/madvise.
repoで確認すべき点: mincore/fork/madvise の実装済み範囲。
過剰主張の注意: mmap が user-space buffer pool を完全に置き換えるとは言わない。

The same delegation creates risks. Page faults can be expensive and hard to attribute to individual KVS operations. Kernel eviction may not match database-level priorities. Dirty page writeback can appear as latency spikes. TLB shootdowns and page-table management costs may become visible at high thread counts. Memory cgroups complicate fairness because process RSS does not fully capture page-cache pressure. These risks are central to the paper rather than incidental limitations.

[日本語コメント]
狙い: mmap criticism を最初から取り込んで robust な story にする。
対応する評価: E1 page faults/TLB shootdowns/cgroup memory, E5 mmap-only microbaseline, Section 9 limitations.
根拠: evaluation plan explicitly includes page faults, tail latency, SSD bandwidth, cycles/op, TLB shootdowns, cgroup memory, and mmap writeback policy.
repoで確認すべき点: page-fault, TLB shootdown, cgroup, and writeback instrumentation support.
過剰主張の注意: mmap-only microbaseline が速くても VMemKV の設計全体が不要とは即断しない。

## 2.5 Explicit buffer managers for larger-than-memory systems

Explicit buffer-manager systems keep the DBMS in control of page residency. LeanStore uses techniques such as pointer swizzling to reduce lookup overhead while retaining control over eviction and page management [LeanStore]. vmcache uses virtual memory mechanisms to help implement buffer management, but it still gives the DBMS explicit control over replacement and residency decisions [vmcache]. Such designs can provide predictability and database-specific eviction policies.

[日本語コメント]
狙い: explicit buffer manager を “予測可能性と制御” の代表として位置づける。
対応する評価: E7 responsibility table, Section 9 limitations.
根拠: contribution plan contrasts vmcache with VMemKV by noting vmcache uses VM in a DBMS-controlled buffer-manager design, while VMemKV delegates much value residency to the OS.
repoで確認すべき点: Related Work で LeanStore/vmcache の正確な引用情報を埋める。
過剰主張の注意: explicit buffer manager を不要・時代遅れとは書かない。

VMemKV chooses the opposite trade-off for the value layer: it gives up some DBMS-level predictability to reduce the amount of storage-engine code responsible for value residency. The hypothesis is not that this is universally better. It is that when T1 remains hot and compact, and T2 values can tolerate OS-managed residency, the resulting system may be simpler while still practical for a meaningful region of the workload space.

[日本語コメント]
狙い: 論文の “design point + characterization” framing を明確化する。
対応する評価: E1, E3, E5, E7.
根拠: evaluation plan says the goal is to clarify conditions where OS-delegated design works or does not work, not to show universal fastest performance.
repoで確認すべき点: T1 memory footprint and mlock/hugepage support under larger-than-memory conditions.
過剰主張の注意: “meaningful region” は評価後に具体化する。

---

# 3. Design Goals and Scope

## 3.1 Target

VMemKV targets a standalone key-value store with byte-oriented keys and values, exposed through Get, Insert, Update, Delete, and Scan. The target workloads include point reads, update-heavy workloads, read-mostly workloads, short scans, and larger-than-memory value sets. The evaluation plan uses YCSB-style macrobenchmarks and specialized microbenchmarks to separate raw mmap value-access cost from the costs and benefits of the T1/T2 architecture.

[日本語コメント]
狙い: target workload を API と workload の両方で定義する。
対応する評価: E1, E2, E3, E5.
根拠: evaluation plan lists Get/Insert/Update/Delete/Scan and larger-than-memory workloads as scope, and includes YCSB A/B/C/E/F.
repoで確認すべき点: 現行 benchmark は nanobench/custom throughput で、YCSB support は未確認。
過剰主張の注意: YCSB 結果が存在するように書かない。

## 3.2 Non-goals

VMemKV does not provide multi-operation transactions, SQL processing, phantom avoidance, secondary-index concurrency control, or replication. It does not attempt to be a complete distributed database system. Recovery in this paper is scoped to checkpoint plus WAL replay for the standalone KVS state, and the planned crash experiments are sanity checks rather than a complete formal proof of production-grade crash consistency.

[日本語コメント]
狙い: non-goals を明示して scope creep を避ける。
対応する評価: E6 recovery sanity check, E7 responsibility table.
根拠: evaluation plan excludes multi-operation transaction, SQL layer, phantom avoidance, LineairDB/Kamo concurrency control, and replication.
repoで確認すべき点: recovery implementation status; current code has checkpoint-free coordinator comment.
過剰主張の注意: crash consistency を完全保証済みと書かない。

## 3.3 Design principles

VMemKV follows four design principles. First, delegate value residency to the OS rather than implementing a user-space buffer pool for T2. Second, keep hot metadata, lookup control, scan control, and reachability decisions in T1. Third, avoid LSM-style multi-level compaction as the central mechanism, while still providing reorganization to repair fragmentation. Fourth, make simplicity auditable by comparing explicit storage-engine responsibilities across VMemKV, LSM engines, value-log engines, mmap-B-tree systems, and buffer-manager systems.

[日本語コメント]
狙い: design goals を paper の contribution と evaluation に直結させる。
対応する評価: E1, E3, E4, E7.
根拠: section writing plan and contributions identify these design principles and responsibility comparison.
repoで確認すべき点: responsibility table の各項目を実装コードと design docs で裏取りする。
過剰主張の注意: responsibility が少ないことを production-readiness と混同しない。

---

# 4. VMemKV Architecture Overview

## 4.1 Components

VMemKV consists of five conceptual components: **T1**, **T2**, **WAL**, **checkpoint**, and **background jobs**. T1 is the RAM-resident metadata/control layer. T2 is the mmap-backed value layer. The WAL records updates before they modify T1/T2 state. Checkpoints provide a durable base generation from which recovery can begin. Background jobs perform T1 reorganization, T2 reorganization, checkpoint reload, and memory/residency hinting. Current implementation status must be separated from the design: the design documents include WAL and checkpoint, while the current coordinator file is explicitly labeled “checkpoint-free” [repo fact to verify].

[日本語コメント]
狙い: architecture の全体像を component で整理する。
対応する評価: E4 reorganization/checkpoint, E6 recovery, E7 responsibility.
根拠: high-level design lists Tier 1, Tier 2, WAL, Checkpoint, Background Jobs. current implementation file is labeled checkpoint-free.
repoで確認すべき点: WAL/checkpoint の現行実装範囲。
過剰主張の注意: design component と implemented component を混同しない。

**Figure 1: VMemKV architecture.**
Placeholder: show client operations entering VMemKV; T1 as RAM-resident control plane containing sorted_region, append_region, payload bits, tombstones, optional append map/Bloom/SIMD/inline; T2 as mmap-backed file with value records; WAL on write path; checkpoint/reload and background reorganization path; OS VM managing T2 page residency.

[日本語コメント]
狙い: 実装図の指示を具体化する。
対応する評価: E3, E4, E5.
根拠: high-level design includes two-tier architecture and figures for two-region/two-tier/reorganization/live reload.
repoで確認すべき点: figure の map mode, WAL order, checkpoint boundary.
過剰主張の注意: 図で未実装機能を実装済みに見せない。

## 4.2 T1 as metadata/control plane

T1 stores fixed-size entries. Conceptually, each entry contains a key prefix, a hash of the full key, and payload bits. The payload is interpreted either as a T2 offset or as an inline 64-bit value, depending on payload mode and optional inline optimization. T1 has two regions: a sorted region for ordered lookup and scan control, and an append region that receives recent inserts. T1 is expected to fit in RAM and is the layer VMemKV explicitly controls.

[日本語コメント]
狙い: T1 の役割を “metadata/control plane” として明確化する。
対応する評価: E3 design breakdown, E4 T1 reorganization.
根拠: low-level design defines `IndexEntry` as 16-byte key prefix, hash, payload_bits, sorted_region, append_region, and payload mode. Current code uses `StoreKey` 16 bytes, hash, payload_bits and append/sorted regions.
repoで確認すべき点: actual slot size differs from conceptual `IndexEntry` because implementation uses atomic payload and published flags in slots.
過剰主張の注意: T1 “always fits” ではなく target/assumption として書く。

## 4.3 T2 as mmap-backed value/residency plane

T2 stores variable-length records in a file-backed mapped byte region. T1 payload bits identify a T2 record by byte offset. A T2 record stores a header, full key bytes, value bytes, and padding up to the allocated value length. Because T1 stores only a prefix and hash, VMemKV verifies the full key in T2 before returning a value. Page residency of T2 value bytes is delegated to the operating system; VMemKV can provide hints, but it does not implement a full user-space buffer pool for T2.

[日本語コメント]
狙い: T2 の record layout と full-key verification の必要性を説明する。
対応する評価: E1 OS residency, E3 T1/T2 split, E5 mmap-only baseline.
根拠: low-level design defines T2 record header and layout. Get path verifies full key in T2. Current T2 code maps a file and resolves records by offset.
repoで確認すべき点: MAP_PRIVATE vs MAP_SHARED semantics in current code and design.
過剰主張の注意: OS page cache fully replaces DB buffer manager とは言わない。

## 4.4 Core invariant

The central invariant is reachability: a T2 record is live if and only if it is reachable from a live T1 entry. T2 records that are not reachable from T1 are garbage. Deletes are represented by tombstoning the T1 payload rather than immediately modifying T2. Updates either modify a reachable T2 record in place or make the old record unreachable by appending a new record and changing the T1 offset. T2 reorganization reclaims unreachable records by copying only live records into a new T2 generation and updating T1 offsets.

[日本語コメント]
狙い: reorganization と recovery の基礎になる invariant を定義する。
対応する評価: E4 T2 unreachable bytes, copied/reclaimed bytes, scan before/after.
根拠: low-level design states live T2 records are reachable from live T1 entries, and unreachable records are garbage reclaimed by reorganize. T2 reorganize copies live records and skips tombstones/unreachable records.
repoで確認すべき点: duplicate key and tombstone handling during concurrent reorganization.
過剰主張の注意: invariant が crash consistency まで自動的に保証するとは書かない。

## 4.5 How VMemKV differs from adjacent designs

Compared with an LSM-tree engine, VMemKV does not organize all data around immutable sorted runs and multi-level compaction. Compared with WiscKey or BlobDB, VMemKV’s separated value layer is mmap-backed and intentionally delegates value residency to the OS, while using T1 as a RAM-resident control plane. Compared with Bitcask, T1 is not only a key directory; it maintains sorted and append regions to support scan control. Compared with LMDB, VMemKV does not mmap a B+tree as the main structure; it uses mmap primarily for the value/residency plane. Compared with explicit buffer-pool systems, VMemKV reduces DB-side residency management for values, while accepting OS-level unpredictability as a design risk.

[日本語コメント]
狙い: Related Work より前に architecture-level difference を簡潔に示す。
対応する評価: E2, E5, E7.
根拠: contribution plan provides these comparative axes.
repoで確認すべき点: LMDB/B+tree comparison wording and value-layer mmap scope.
過剰主張の注意: “VMemKV is simpler” を subjective にしない。

---

# 5. T1/T2 Data Model and Operation Paths

## 5.1 T1 data model

The conceptual T1 `IndexEntry` contains:

```c++
using StoreKeyPrefix = std::array<std::byte, 16>;

struct IndexEntry {
    StoreKeyPrefix key_prefix;
    uint64_t hash;          // hash(full_key)
    uint64_t payload_bits;  // T2 offset, tombstone, or inline value
};
```

The `key_prefix` is the primary sort key and allows compact comparison in T1. The `hash` disambiguates keys that share a prefix. The `payload_bits` field is interpreted as a T2 offset in `Offset64` mode, as an inline value in `Inline64` mode, or as a tombstone marker. T1 is split into `sorted_region` and `append_region`: the sorted region supports binary search and ordered scan control, while the append region supports simple insertion before later reorganization.

[日本語コメント]
狙い: reviewer が操作 path を追えるよう T1 layout を具体化する。
対応する評価: E3 AppendMap/BloomFilter/SimdScan/Inline64 breakdown.
根拠: low-level design defines the exact conceptual fields and payload modes. Current code defines `StoreKey`, hashing, inline metadata, and `STORE_NOT_FOUND`.
repoで確認すべき点: `TOMBSTONE_PAYLOAD`, `TOMBSTONE_OFFSET`, `STORE_NOT_FOUND` naming and exact semantics.
過剰主張の注意: code-level exactness requires verification because actual slots include atomics and optional structures.

T1’s append region is deliberately a temporary hot write region. In the base design, lookup scans the append region and then searches the sorted region. With the AppendMap optimization, append-region lookup can use an auxiliary hash table. With a Bloom filter on the sorted region, negative lookups can often avoid searching the sorted array. With SIMD scan, prefix comparisons can be vectorized. With memory hints, T1 can use OS mechanisms such as `mlock`, `MADV_HUGEPAGE`, and `MADV_SEQUENTIAL` to keep the control plane hot or prefetch scan paths [repo fact to verify].

[日本語コメント]
狙い: optional optimizations を correctness ではなく performance hooks として説明する。
対応する評価: E3 design breakdown.
根拠: config.hpp lists AppendMap, BloomFilter, SimdScan, MemoryHints, T1InlineValue. Low-level design marks opt-in optimizations as correct even when disabled. Current memory_hints code uses `mlock` and `madvise`.
repoで確認すべき点: each optimization’s production readiness and whether comments overstate “lock-free” or “uninterrupted throughput.”
過剰主張の注意: optimization effects are evaluation questions, not claims.

## 5.2 T2 data model

The conceptual T2 record format is:

```c++
struct ValueRecordHeader {
    uint32_t key_len;
    uint32_t value_len;
    uint32_t alloc_len;
    uint32_t flags;
    uint64_t version;
};

// Layout:
// [ValueRecordHeader][key bytes][value bytes][padding up to allocation]
```

`key_len` and `value_len` describe the logical record. `alloc_len` describes the physical allocation available for the value payload. Separating `value_len` from `alloc_len` allows in-place updates when the new value length is no larger than the existing allocation. `flags` are reserved, and `version` is available for debugging or validation in the design. Records are addressed by byte offsets from the beginning of the mapped T2 region.

[日本語コメント]
狙い: in-place update の前提である alloc_len を明示する。
対応する評価: E2 update-heavy workload, E4 storage fragmentation, E3 in-place/append breakdown.
根拠: low-level design and current T2 header both define key_len, value_len, alloc_len, flags, version.  Current append sets alloc_len to value size.
repoで確認すべき点: allocation policy beyond exact value length; current append appears to set `alloc_len = value.size()`, limiting growth slack.
過剰主張の注意: Do not imply all future larger values can update in place.

## 5.3 Get path

A Get computes the 16-byte key prefix and full-key hash, searches T1’s append region and sorted region, checks for a tombstone, and then resolves the T2 offset if the payload is not inline. The T2 record’s full key is compared with the requested full key before returning the value. This final verification is required because T1’s prefix and hash are compact filters, not the full logical key. If Inline64 or dynamic T1 inlining covers the value, the Get can return from T1 without touching T2 [repo fact to verify for exact inline mode].

[日本語コメント]
狙い: Get の correctness path と T2 access の必要性を説明する。
対応する評価: E1 page faults, E3 Inline64/AppendMap/BloomFilter breakdown, E5 mmap-only baseline.
根拠: low-level design describes Get steps including prefix/hash, T2 offset, and full-key comparison. Current get implementation resolves inline values or calls lookup_record, which verifies T2 record key equality.
repoで確認すべき点: collision behavior and hash width under inline metadata masking.
過剰主張の注意: hash+prefix does not replace full-key verification.

## 5.4 Insert path

An Insert first checks whether the key already exists. In the durability design, the operation appends an insert record to the WAL and persists it before modifying T1/T2 [repo fact to verify for current implementation]. For offset payloads, VMemKV appends a T2 record at `bytes_used`, obtains the offset, and publishes a T1 entry in the append region. For inline-eligible values, VMemKV can bypass T2 and store the value directly in T1 payload bits. Publishing T2 before T1 prevents readers from observing a T1 offset that points to an unwritten T2 record.

[日本語コメント]
狙い: insert の ordering と durability design を分離して説明する。
対応する評価: E2 load phase, E6 recovery, E3 Inline64.
根拠: low-level design specifies WAL before T1/T2 and T2 append before T1 entry. Current implementation writes T2 then T1 but current file is checkpoint-free and does not show WAL.
repoで確認すべき点: WAL append/fsync implementation and whether insert is currently durable.
過剰主張の注意: Do not claim current code has full insert durability unless WAL code is verified.

## 5.5 Delete path

A Delete locates the key through the Get/T1 lookup path. In the durability design, it first records the delete in the WAL. The in-memory update then marks the T1 payload as a tombstone or not-found marker. T2 is not modified during the delete. The old T2 record becomes unreachable and is reclaimed later by T2 reorganization.

[日本語コメント]
狙い: logical delete と physical reclamation を分ける。
対応する評価: E4 unreachable bytes/reclaimed bytes, E6 WAL replay.
根拠: high-level design says Delete writes WAL, sets T1 offset to tombstone, and does not access T2. Current implementation marks T1 payload as `STORE_NOT_FOUND`.
repoで確認すべき点: tombstone persistence and WAL replay semantics.
過剰主張の注意: Delete does not immediately free T2 space.

## 5.6 Scan path

A Scan uses T1 to identify candidate entries in the key range. It scans or searches T1’s append and sorted regions, removes tombstones, sorts and deduplicates candidates when needed, and then resolves each live payload through T2 unless the value is inline. Because T1 stores prefixes, T2 full-key comparison is still necessary for exact range membership. T1 reorganization improves scan control by absorbing the append region into the sorted region; T2 reorganization can improve value-access locality by rewriting live T2 records in T1 order.

[日本語コメント]
狙い: Scan の ordering fragmentation と reorganization の必要性をつなげる。
対応する評価: E2 Workload E, E4 scan before/during/after, E3 SimdScan.
根拠: low-level design describes Scan steps and full-key filtering. High-level design explains ordering and storage fragmentation and T1/T2 reorganization effects. Current scan implementation collects T1 candidates and resolves T2 records.
repoで確認すべき点: exact scan ordering guarantee before and after reorganize.
過剰主張の注意: Scan can suffer if T2 locality is poor or OS faults dominate.

## 5.7 Update semantics and in-place update

VMemKV’s update path is intentionally not purely append-only. After locating the key, VMemKV checks the existing T2 record allocation. If `new_value_len <= alloc_len`, it updates the value in place, adjusts `value_len`, and may increment a record version. If the new value exceeds the allocation, VMemKV appends a new T2 record and updates the corresponding T1 payload to the new offset. The old record then becomes garbage. This differs from value-log designs in which updates normally append new values and require later GC regardless of whether the value size is stable.

[日本語コメント]
狙い: in-place update の正確な条件と append fallback を明確化する。
対応する評価: E2 Workload A/F, E4 garbage/reorganization pressure.
根拠: high-level design and low-level design state exactly this condition.  Current T2 code returns false when value size exceeds `alloc_len`; otherwise it overwrites and updates `value_len` and `version`.
repoで確認すべき点: whether “all-region in-place update” is accurate terminology; current allocation policy.
過剰主張の注意: Do not claim arbitrary-size updates are in-place.

In-place update is most relevant for fixed-size or bounded-growth update-heavy workloads. When many updates fit the existing allocation, VMemKV can avoid creating a new T2 record, which can reduce unreachable bytes and reorganization pressure. When values grow beyond their allocation, VMemKV behaves like an append-and-update-offset system. Therefore, in-place update is an auxiliary benefit whose magnitude depends on workload value-size stability and allocation policy; it is not the central thesis of VMemKV.

[日本語コメント]
狙い: in-place update を強調しすぎないよう抑制する。
対応する評価: E2 update-heavy, E4 fragmentation, E3 breakdown.
根拠: planning files repeatedly state in-place update is important but not central.
repoで確認すべき点: benchmark currently has update tests, but YCSB update-heavy experiment remains planned.
過剰主張の注意: “updates do not create garbage” と書かない。

**Figure 2: T1/T2 layout.**
Placeholder: T1 sorted_region and append_region, each with key_prefix/hash/payload_bits; payload_bits point to T2 offsets; T2 records contain header/key/value/padding; inline entries bypass T2.

**Figure 3: Operation paths.**
Placeholder: Get, Insert, Update, Delete, Scan as separate arrows through T1, WAL, and T2.

**Figure 4: In-place update vs append update.**
Placeholder: left side shows value_len shrink or same-size update within alloc_len; right side shows larger value appended to T2 and T1 offset changed.

[日本語コメント]
狙い: 図の作成作業を後続 revision に渡しやすくする。
対応する評価: E3, E4.
根拠: user prompt required these figure placeholders; layout is grounded in low-level design.
repoで確認すべき点: figure labels should match final code names.
過剰主張の注意: 図で WAL/checkpoint を実装済みに見せる場合は注記が必要。

---

# 6. Reorganization, Checkpoint, and Recovery

## 6.1 Why fragmentation arises

VMemKV has two kinds of fragmentation. **Ordering fragmentation** arises because new T1 entries enter the append region and because T2 records are appended or updated in an order that may not match key order. As the append region grows, Get and Scan may spend more time reconciling unsorted entries. As T2 layout becomes less correlated with T1 order, scans may trigger less predictable page access. **Storage fragmentation** arises when deletes and append-updates leave T2 records unreachable from T1.

[日本語コメント]
狙い: reorganization の必要性を “LSM compaction の代わり” ではなく specific fragmentation repair として説明する。
対応する評価: E4 scan before/during/after, T2 unreachable bytes, reclaimed bytes.
根拠: high-level and low-level design define ordering fragmentation and storage fragmentation.
repoで確認すべき点: metrics for ordering fragmentation and storage fragmentation.
過剰主張の注意: reorganization cost が小さいとはまだ言わない。

In-place update reduces one source of storage fragmentation but does not remove the need for reorganization. Any delete creates unreachable T2 data. Any update that exceeds the allocation appends a new record. Even in fixed-size workloads, T1’s append region can grow and harm scan control. Therefore, VMemKV still requires background repair; it simply uses repair mechanisms tailored to T1/T2 reachability rather than LSM-style multi-level compaction.

[日本語コメント]
狙い: in-place update で GC/reorganization 不要という誤読を防ぐ。
対応する評価: E4 reorganization pressure.
根拠: contribution plan explicitly warns not to claim GC/compaction is eliminated.
repoで確認すべき点: reorganization trigger policy and thresholds.
過剰主張の注意: “compaction-free” という表現を避ける。

## 6.2 T1 reorganization

T1 reorganization merges the append region into the sorted region, removes tombstones, resolves duplicate logical keys, and resets the append region. It can be performed independently of T2 when T2 offsets remain valid. This allows VMemKV to repair lookup and scan-control degradation without rewriting value bytes. In the current implementation, T1 reorganization is designed to allow lock-free or optimistic readers using a sequence protocol, while writers are reconciled through version checks and retry/snapshot behavior [repo fact to verify for final concurrency contract].

[日本語コメント]
狙い: T1-only reorganization の軽さと独立性を説明する。
対応する評価: E4 T1 reorganization cost, E3 scan optimization.
根拠: low-level design says T1 reorganize is independent of T2 and removes tombstones. Current T1 implementation uses reorg sequence and publishes a rebuilt sorted region.
repoで確認すべき点: duplicate-key resolution correctness under concurrent writes.
過剰主張の注意: “lock-free” wording should be verified against actual locks and memory reclamation.

## 6.3 T2 reorganization

T2 reorganization rebuilds the value layer. It walks live T1 entries, copies the corresponding T2 records into a new T2 byte region, computes new offsets, and writes those offsets back into T1. Tombstones and unreachable T2 records are not copied. This repairs storage fragmentation and can improve scan locality if live records are copied in T1 order. Because T2 offsets change, T2 reorganization must be coordinated with T1 reorganization.

[日本語コメント]
狙い: T2 reorganization を value-log GC と似た live-copy operation として正確に説明する。
対応する評価: E4 copied bytes/reclaimed bytes/foreground p99.
根拠: low-level design specifies T2 reorganize input/output/procedure/effect. Current VMemKVImpl `reorganize()` writes live records to temp file and swaps T2 memory.
repoで確認すべき点: whether temp-file rename protocol is crash safe; current code maps temp with MAP_SHARED and renames after swap.
過剰主張の注意: T2 reorganization is not free and may interfere with foreground operations.

## 6.4 Checkpoint reload

The design couples T2 reorganization with checkpoint reload. Since rebuilding T2 can require a full scan and full copy, VMemKV avoids holding both generations entirely as ordinary in-memory structures. The design writes new T1 and T2 checkpoint files, uses `fork` to create a copy-on-write snapshot, reorganizes in the child, then briefly stops foreground operations to mmap the new checkpoint files, replay WAL records after the snapshot LSN, and publish the new generation. This design aims to bound the final stop-the-world interval, but the evaluation must measure fork time, memory high-water mark, replay time, and foreground interference.

[日本語コメント]
狙い: checkpoint reload の design flow を説明しつつ cost を評価対象にする。
対応する評価: E4 fork time, stop-the-world time, WAL replay time, memory high-water mark, TLB shootdowns.
根拠: high-level design and low-level design describe checkpoint reload steps with fork, snapshot LSN, mmap, replay, and WAL cleanup.
repoで確認すべき点: current implementation may not yet implement fork-based checkpoint reload.
過剰主張の注意: Do not claim stop-the-world time is small before measuring.

## 6.5 WAL replay and recovery envelope

The recovery design starts from the latest complete checkpoint and replays WAL records after the checkpoint’s replay boundary. The replay boundary must avoid reapplying records already included in the checkpoint snapshot and must include all records committed before the crash. The planned evaluation treats recovery as a sanity check: it crashes during load, update, checkpoint, and reorganization; measures recovery time as a function of WAL size and checkpoint age; checks replay bytes; and validates recovered key count and checksums. A stronger production-grade crash-consistency claim would require a precise protocol for checkpoint completion, incomplete checkpoint handling, WAL truncation, file fsync, atomic rename, and mmap writeback.

[日本語コメント]
狙い: recovery を system completeness として説明しつつ、証明済み claim にしない。
対応する評価: E6 recovery and crash-consistency sanity check.
根拠: evaluation plan includes crash during load/update/checkpoint/reorganization and recovery metrics. Design docs state checkpoint + WAL replay recovery and WAL rotation.
repoで確認すべき点: checkpoint completion protocol, incomplete checkpoint handling, replay start/end LSN, fsync policy.
過剰主張の注意: “durability guaranteed” should be conditional on verified implementation.

## 6.6 Why this is not LSM-style multi-level compaction

VMemKV still performs background repair, copying, and garbage reclamation. The distinction is the unit and purpose of repair. LSM compaction repeatedly merges sorted immutable runs across levels to control read, write, and space amplification. VMemKV reorganization instead uses T1 reachability to rebuild the T1 control plane and T2 value plane. It does not maintain a hierarchy of sorted SSTables as the central design. This difference matters because it changes the storage-engine responsibilities under comparison, but it does not make VMemKV free of maintenance work.

[日本語コメント]
狙い: “not compaction” の過剰表現を避ける。
対応する評価: E7 responsibility table, E4 reorganization cost.
根拠: contribution plan explicitly says reorganization is broadly GC/compaction-like and should not be described as eliminating compaction entirely.
repoで確認すべき点: terminology in final paper: “reorganization” vs “compaction” vs “GC.”
過剰主張の注意: “compaction-free” は使わない。

**Figure 5: Reorganization flow.**
Placeholder: T1 sorted/append merge; live T1 entries drive T2 record copy; unreachable T2 records skipped; new offsets written to T1.

**Figure 6: Checkpoint/reload flow.**
Placeholder: fork snapshot, child reorganizes to checkpoint files, parent continues foreground work, final stop-the-world, mmap new files, WAL replay, publish.

**Figure 7: Recovery path.**
Placeholder: latest complete checkpoint + WAL replay range + recovered T1/T2 state.

[日本語コメント]
狙い: system completeness 図の作成指示。
対応する評価: E4 and E6.
根拠: high-level design includes reorganization and live reload checkpoint figures and flow.
repoで確認すべき点: checkpoint/recovery diagrams should not imply unverified crash protocol.
過剰主張の注意: failure handling arrows should be marked design/prototype until verified.

---

# 7. Implementation

## 7.1 Repository structure and status

The implementation repository contains a C++ implementation under `implementation/`, with design documents, source code, tests, and benchmarks. The README describes VMemKV as a larger-than-memory KVS delegating I/O management to OS virtual memory mechanisms and points to high-level design, low-level design, and latency survey documents. The build uses CMake and C++20/C++23-compatible compilers, with optional RocksDB support for comparison. The current CMake target builds a static `vmemkv` library from `src/t2_flat_file/t2_flat_file.cpp`, while many components are header-based.

[日本語コメント]
狙い: repo 構造と build status を具体的に記述する。
対応する評価: E0 setup/fairness, E3 variants, E7 responsibility table.
根拠: README lists docs/build/tests/benchmarks.  CMake builds static library from T2 source and includes tests/benchmarks.
repoで確認すべき点: source files not listed in CMake may be header-only or incomplete.
過剰主張の注意: “current implementation complete” と書かない。

## 7.2 T1 implementation

The T1 implementation is a fixed-size two-region in-memory index with a 16-byte ordered key prefix, a masked FNV-style hash, and payload bits. It uses an append region with a fixed capacity and a sorted region stored behind an atomic shared pointer. Optional features include an append-region hash index, a Bloom filter on the sorted region, SIMD-assisted scans, memory hints, and T1 inline values. Current code includes optimistic reader behavior during reorganization using a sequence counter, plus write-version checks during rebuild [repo fact to verify].

[日本語コメント]
狙い: T1 implementation facts を design と結びつける。
対応する評価: E3 AppendMap/BloomFilter/SimdScan/MemoryHints/Inline64.
根拠: T1 code defines StoreKey, hash masking, inline metadata, append/sorted regions, append capacity, get/put/scan/reorganize.
repoで確認すべき点: exact concurrency correctness under simultaneous writer/reorganize.
過剰主張の注意: lock-free claim needs careful verification.

## 7.3 T2 implementation

The current T2 implementation, `T2FlatFile`, creates a fixed-capacity file, maps it, appends records by atomically reserving bytes, and resolves records by offset. It stores `ValueRecordHeader`, key bytes, value bytes, and padding. The implementation supports `update_value_at`: if the new value fits within `alloc_len`, it overwrites the value bytes in place, updates `value_len`, increments `version`, and returns true; otherwise it returns false so the caller can append a new record and update T1. The initial mapping path uses `mmap` with `MAP_PRIVATE`; the reorganization path maps a temporary file with `MAP_SHARED` before swapping memory [repo fact to verify for final intended mode].

[日本語コメント]
狙い: T2 current implementation を具体的に説明し、map mode mismatch を明示する。
対応する評価: E1 mmap behavior, E3 in-place update, E4 reorganization.
根拠: T2 code implements append, at, update_value_at, mmap mapping, ftruncate.   Reorganize maps temp file with MAP_SHARED.
repoで確認すべき点: MAP_PRIVATE write persistence semantics, msync/fsync policy, and whether current mode matches durability design.
過剰主張の注意: mmap-backed does not automatically mean persistent or crash-safe.

## 7.4 Coordinator and operation implementation

The current `VMemKVImpl` routes public operations across T1 and T2. Insert checks for an existing key, then writes to T2 or inline payload and publishes to T1. Get resolves inline payloads or fetches T2 records. Update tries T2 in-place update when possible and otherwise writes a new entry. Remove marks the T1 payload as not found. Scan gathers T1 candidates and resolves T2 values. The coordinator also exposes `reorganize()`, which writes live records to a temporary T2 file, rebuilds T1 offsets through an offset mapper, swaps the T2 mapping, and renames the temporary file. The file header labels this coordinator “checkpoint-free,” so WAL/checkpoint/recovery should be described as design or planned implementation until verified.

[日本語コメント]
狙い: 実装済み operation path と未確認 durability を分ける。
対応する評価: E3 operation paths, E4 reorganization, E6 recovery status.
根拠: current coordinator operations and reorganization are visible in code.   Current file says checkpoint-free.
repoで確認すべき点: WAL/checkpoint modules, if in another branch or unlisted directory.
過剰主張の注意: Do not state current implementation has WAL replay without code evidence.

## 7.5 OS interface usage

The design documents target `mmap`, `fork`, `mincore`, and `madvise` as OS mechanisms for value residency and checkpoint behavior. Current source evidence shows `mmap`, `munmap`, `ftruncate`, `open`, `write`, `truncate`, `rename`, `mlock`, and `madvise`. Memory hints apply `mlock` and `MADV_HUGEPAGE` to T1 regions and `MADV_SEQUENTIAL` before scans or reorganization. The low-level design also includes T2 prefetch with `MADV_WILLNEED`, but current implementation status should be verified.

[日本語コメント]
狙い: syscall usage を design と current code に分けて正確に書く。
対応する評価: E0 setup, E1 page faults/TLB, E4 fork/memory high-water mark.
根拠: high-level design lists mmap/fork/mincore/madvise. Current T2 and memory_hints code show mmap/munmap/mlock/madvise.  Low-level design lists Tier 2 prefetch.
repoで確認すべき点: `fork`, `mincore`, `msync`, `fsync`, `MADV_WILLNEED` actual use.
過剰主張の注意: Do not claim all targeted OS calls are implemented.

## 7.6 WAL and checkpoint implementation status

The design documents specify WAL-before-update ordering, checkpoint reload, replay from snapshot LSN, and WAL cleanup after checkpoint completion. They also discuss group commit, early lock release, and flush pipelining as possible optimizations. However, the inspected current coordinator is checkpoint-free, and the visible operation code does not show WAL append or fsync on Insert/Update/Delete. Therefore, this draft treats WAL/checkpoint/recovery as part of the VMemKV system design, with implementation status marked `[repo fact to verify]`.

[日本語コメント]
狙い: system completeness を論じつつ、未確認実装を断定しない。
対応する評価: E0 durability settings, E6 recovery.
根拠: low-level design specifies WAL fsync in Insert/Update/Delete and checkpoint replay boundaries.  Current coordinator is checkpoint-free.
repoで確認すべき点: WAL files/modules may exist outside inspected files or another branch.
過剰主張の注意: Full durability cannot be claimed without verified fsync/msync/checkpoint protocol.

## 7.7 Benchmarks and variants

The implementation includes a benchmark harness using nanobench for single-threaded operations and custom fixed-duration throughput tests for multi-threaded Get, Insert, and mixed 80R/20W workloads. It includes data-scale experiments for Get, negative Get, and Scan with uniform and Zipf distributions. The variant list includes baseline VMemKV, cumulative optimization configurations, ablations that remove individual optimizations, a fully optimized T1InlineValue configuration, and an optional RocksDB backend. This infrastructure is useful for early microbenchmarking and ablation, but it is not yet the full YCSB evaluation described in Section 8.

[日本語コメント]
狙い: 現行 benchmark と planned evaluation の差分を明確化する。
対応する評価: E3 variants, E2 planned YCSB.
根拠: benchmark file describes single-thread, multi-thread, and data-scale benchmarks. Variant list is in vmemkv.hpp. run_bench.sh documents benchmark script and options.
repoで確認すべき点: YCSB support, 1 KiB/16 KiB values, BlobDB, write amplification metrics.
過剰主張の注意: nanobench microbenchmarks are not macrobenchmark results.

## 7.8 Tests

The test suite instantiates correctness tests across VMemKV variants and optionally RocksDB. It covers empty gets, insert/get, duplicate insert, updates, deletes, reinsertion, scans, reorganization, long keys sharing 16-byte prefixes, large values, concurrent reads, storage-fragmentation reorganization, integral key scan ordering, and T1 inline value behavior. These tests provide useful sanity coverage of functional paths, but they do not replace crash-recovery validation, larger-than-memory evaluation, or durability testing.

[日本語コメント]
狙い: correctness coverage を具体的に示すが、評価結果として扱わない。
対応する評価: E6 recovery sanity, E3 operation correctness.
根拠: tests cover CRUD/scan/reorg/long keys/large values/concurrency/inlining.
repoで確認すべき点: crash tests and WAL replay tests.
過剰主張の注意: unit tests do not establish production crash consistency.

---

# 8. Evaluation

This draft intentionally does not include empirical results. The planned Evaluation section is organized around E0–E7: experimental setup and fairness; larger-than-memory behavior and thread scalability; YCSB-based comparison with RocksDB and RocksDB BlobDB; T1/T2 design breakdown; reorganization and checkpoint behavior; mmap-only microbaseline; recovery and crash-consistency sanity checks; and implementation-responsibility comparison. The evaluation is designed to characterize practicality, trade-offs, and limitations, not to claim that VMemKV is universally faster than existing KVSs.

[日本語コメント]
狙い: Evaluation section の placeholder と評価質問だけを残す。
対応する評価: 8.1–8.8 全体。
根拠: evaluation plan gives the exact structure and explicitly avoids universal fastest framing.
repoで確認すべき点: Planned evaluation scripts, instrumentation, YCSB integration, BlobDB support.
過剰主張の注意: No throughput/latency/write amplification/recovery numbers until measured.

---

# 9. Discussion and Limitations

## 9.1 When OS-delegated value residency may work

VMemKV is most plausible when T1 remains RAM-resident and frequently accessed metadata fits comfortably within the memory budget, while T2 values are large enough that delegating residency to the OS reduces engine complexity. Skewed workloads may benefit if hot values remain cached by the OS. Fixed-size or bounded-growth update-heavy workloads may benefit from in-place update because they create fewer unreachable T2 records. Scan workloads may benefit after reorganization if T2 records are laid out in T1 order. These are hypotheses for evaluation, not conclusions.

[日本語コメント]
狙い: 成功条件を仮説として列挙する。
対応する評価: E1 skew/uniform, E2 update/scan workloads, E4 scan after reorg.
根拠: evaluation plan includes access skew, value size, larger-than-memory ratios, and scan before/after reorganization.
repoで確認すべき点: T1 memory size under target dataset scale.
過剰主張の注意: “may benefit” として書き、結果を先取りしない。

## 9.2 When OS-delegated value residency may fail

VMemKV may struggle under uniform cold reads over a much larger-than-memory T2, especially on very fast NVMe devices where page-fault overhead becomes comparable to or larger than explicit I/O overhead. High thread counts may expose contention, TLB shootdowns, kernel page-table costs, or writeback interference. Dirty mmap writeback may create latency spikes if not controlled. Under cgroup memory pressure, the OS may evict pages in ways that do not match KVS-level priorities. These conditions are not edge cases; they are part of the evaluation target.

[日本語コメント]
狙い: negative results を想定して論文の robust story を保つ。
対応する評価: E1 uniform/cold reads, TLB shootdowns, cgroup pressure, bandwidth time-series; E5 mmap-only baseline.
根拠: evaluation plan includes page faults, tail latency, bandwidth, cycles/op, TLB shootdowns, and cgroup memory.  Latency survey frames storage-speed crossover as a concern.
repoで確認すべき点: Instrumentation for TLB shootdowns and cgroup page cache pressure.
過剰主張の注意: Do not hide mmap risks.

## 9.3 Interpreting RocksDB wins

If RocksDB wins on some workloads, that does not by itself invalidate VMemKV. RocksDB is a mature production engine with carefully engineered compaction, caching, Bloom filters, and write scheduling. A RocksDB win on scan-heavy, small-value, uniform-read, or high-write-amplification-sensitive workloads would clarify the boundary of the VMemKV design point. The relevant question is whether VMemKV provides a simpler and practical alternative for some larger-than-memory value workloads, not whether it dominates RocksDB everywhere.

[日本語コメント]
狙い: mixed evaluation outcomes に備える。
対応する評価: E2 YCSB comparison, E7 responsibility table.
根拠: evaluation plan explicitly says VMemKV need not win all workloads; it should clarify target and non-target conditions.
repoで確認すべき点: fairness settings: RocksDB memory, durability, compression, direct/buffered I/O, BlobDB.
過剰主張の注意: RocksDB に負けた結果を failure と単純化しない。

## 9.4 Interpreting a faster mmap-only microbaseline

The mmap-only microbaseline is not a durability-matched competitor. It exists to estimate the raw cost of mmap-backed value access with a simple in-memory map. If the mmap-only baseline is faster than VMemKV, the result may indicate overhead in T1 lookup, full-key verification, scan control, synchronization, or reorganization machinery. That would be useful: it would show where VMemKV pays for functionality beyond raw mmap dereference. If the baseline is slower, it may suggest that T1/T2 control provides value beyond direct mmap access. In either case, the microbaseline is diagnostic rather than a production competitor.

[日本語コメント]
狙い: mmap-only baseline の意味を正しく限定する。
対応する評価: E5 mmap-only microbaseline.
根拠: evaluation plan states the mmap-only microbaseline is used only to estimate raw mmap-backed value access cost and is not durability-matched.
repoで確認すべき点: mmap-only baseline implementation status.
過剰主張の注意: baseline comparison を KVS competition として解釈しない。

## 9.5 In-place update limitations

In-place update depends on allocation policy and value-size stability. If most updates increase values beyond their allocation, VMemKV will append new T2 records and generate garbage similarly to value-log systems. If the current allocation policy sets `alloc_len` exactly to the original value size, in-place update primarily helps same-size or shrinking updates. Over-allocating could improve future in-place updates but would increase space usage. This trade-off should be measured rather than assumed.

[日本語コメント]
狙い: in-place update の有効範囲を絞る。
対応する評価: E2 update-heavy, E4 storage fragmentation, E3 breakdown.
根拠: current append sets `alloc_len` to `value.size()`, and update succeeds only when new size is within alloc_len.
repoで確認すべき点: final allocation policy and whether slack allocation is planned.
過剰主張の注意: in-place update is not a universal update optimization.

## 9.6 Durability and production readiness

VMemKV’s design includes WAL, checkpoint reload, and WAL replay, but the inspected current implementation appears to be a prototype with checkpoint-free coordination. Production readiness would require verified WAL ordering, fsync/msync policy, checkpoint completion markers, incomplete-checkpoint handling, atomic generation switching, SIGBUS handling for mmap I/O errors, recovery tests across crash points, and operational controls for dirty writeback. These are engineering requirements, not optional polish.

[日本語コメント]
狙い: durability limitation を明示して信頼性を保つ。
対応する評価: E0 durability policy, E6 crash-consistency sanity.
根拠: high-level design mentions WAL/checkpoint durability, but current implementation is checkpoint-free.  High-level TODO mentions SIGBUS handler design.
repoで確認すべき点: all durability and crash-path details.
過剰主張の注意: Do not call the prototype production-grade.

## 9.7 Portability and OS dependence

VMemKV’s design is inherently OS-dependent. It relies on virtual memory behavior, mmap semantics, memory advice, page cache policy, and filesystem writeback behavior. The same workload may behave differently across Linux kernel versions, filesystems, cgroup settings, NUMA topologies, SSDs, and cloud block devices. This portability cost is the other side of delegating residency to the OS. A fair evaluation must report kernel version, filesystem, mount options, dirty page settings, cgroup limits, and storage hardware.

[日本語コメント]
狙い: OS dependency を limitation と reproducibility requirement の両方として扱う。
対応する評価: E0 setup and fairness.
根拠: evaluation plan lists OS kernel, filesystem, cgroup memory limit, dirty page settings, SSD, NUMA, and related setup details.
repoで確認すべき点: benchmark scripts collect environment metadata or not.
過剰主張の注意: OS behavior を portable constant とみなさない。

---

# 10. Related Work

## 10.1 LSM-tree systems

Bigtable, LevelDB, and RocksDB represent the LSM-tree lineage [Bigtable, LevelDB, RocksDB]. These systems buffer writes in memory, persist sorted immutable files, and use compaction to control read, write, and space amplification. RocksDB in particular is a mature production engine with a rich configuration surface for compaction, block cache, Bloom filters, compression, WAL policy, and I/O mode. VMemKV does not attempt to replace this general-purpose design. Instead, it asks whether a standalone KVS focused on larger-than-memory values can move some value-residency responsibility to the OS while keeping metadata explicitly controlled.

[日本語コメント]
狙い: RocksDB を main baseline としつつ、universal replacement を否定する。
対応する評価: E2, E7.
根拠: evaluation plan designates RocksDB as primary baseline and requires matched memory/durability settings.
repoで確認すべき点: RocksDB experience paper citation details.
過剰主張の注意: RocksDB に一般に勝つとは書かない。

## 10.2 Key-value separation

WiscKey separates keys from values by storing value pointers in the LSM tree and appending values to a separate log [WiscKey]. BlobDB integrates a related large-value separation strategy into RocksDB [BlobDB]. These designs reduce value movement through LSM compaction but introduce value-log garbage collection, pointer consistency, and scan-layout issues. VMemKV is closest to this family in its key/value separation, but differs in two ways: the value layer is mmap-backed and residency is delegated to OS virtual memory, while the T1 layer is a RAM-resident control plane rather than an LSM index.

[日本語コメント]
狙い: WiscKey に近いことを隠さず、差分を明確化する。
対応する評価: E2 BlobDB, E4 reorganization, E5 mmap baseline.
根拠: must_read_papers identifies WiscKey as highly related. contribution plan gives the T1/T2 distinction.
repoで確認すべき点: BlobDB citation and configuration.
過剰主張の注意: key-value separation 自体を VMemKV の新規性にしない。

## 10.3 Append-only log plus in-memory index

Bitcask stores an in-memory key directory pointing into append-only data files [Bitcask]. This approach provides simple point lookups and append-friendly writes, with merge operations to reclaim obsolete entries. VMemKV shares the use of memory-resident metadata pointing into value storage, but T1 is designed to support ordered scan control through sorted and append regions. VMemKV also differs by making T2 an mmap-backed residency plane rather than merely an append-only file read through explicit I/O.

[日本語コメント]
狙い: Bitcask との差分を scan control と mmap residency に置く。
対応する評価: E4 scan/reorganization, E7 responsibility table.
根拠: high-level design defines Reorganizing Two-Region and two-tier architecture.
repoで確認すべき点: Bitcask reference details.
過剰主張の注意: Bitcask の ordered scan 弱点を過度に一般化しない。

## 10.4 mmap-based KVSs and mmap criticism

LMDB is a prominent mmap-based embedded KVS that maps database pages into virtual memory [LMDB]. mmap can simplify data access and reduce explicit copying, but database research has emphasized risks including page-fault unpredictability, poor integration with DBMS-level eviction priorities, dirty writeback stalls, SIGBUS failure modes, and TLB/page-table overhead [AreYouSureMMAP]. VMemKV explicitly incorporates this criticism by limiting mmap’s central role to the value layer and keeping T1 metadata under engine control. The evaluation includes a mmap-only microbaseline and OS-level metrics because mmap behavior is a hypothesis to test, not an assumption to trust.

[日本語コメント]
狙い: LMDB と mmap 批判文献を対立軸として扱う。
対応する評価: E1, E5, Section 9.
根拠: high-level design explicitly positions VMemKV differently from LMDB via RAM-resident T1 and mmap-backed T2. latency survey cites the mmap criticism paper as relevant.
repoで確認すべき点: LMDB and mmap criticism bibliographic details.
過剰主張の注意: mmap criticism を無視しない。

## 10.5 Larger-than-memory buffer managers

LeanStore and vmcache represent systems that address larger-than-memory data with explicit or VM-assisted buffer management [LeanStore, vmcache]. They provide DBMS-level control over eviction and memory residency, often using sophisticated pointer translation or swizzling to reduce overhead. VMemKV chooses a narrower scope: it is a standalone KVS that keeps T1 metadata resident and delegates much T2 value residency to the OS. This reduces the number of DB-side responsibilities but also gives up some predictability and control.

[日本語コメント]
狙い: explicit buffer manager との trade-off を整理する。
対応する評価: E7 responsibility table, E1 limitations.
根拠: contribution plan contrasts VMemKV with vmcache and explicit buffer-pool systems.
repoで確認すべき点: LeanStore/vmcache citation details.
過剰主張の注意: explicit control の価値を否定しない。

## 10.6 Hybrid log systems

FASTER combines a hash index with a hybrid log to support high-performance concurrent key-value operations over memory and storage [FASTER]. Like VMemKV, it separates indexing from log-structured value storage to some extent, but its focus is a highly concurrent hybrid log architecture and associated recovery model. VMemKV’s focus is different: OS-delegated larger-than-memory value residency with a RAM-resident T1 control plane and mmap-backed T2 value layer.

[日本語コメント]
狙い: FASTER との類似性を認めつつ focus difference を出す。
対応する評価: Related Work positioning, E7 responsibility table.
根拠: section writing plan includes FASTER as hybrid log related work.
repoで確認すべき点: FASTER reference details and whether to mention recovery comparison.
過剰主張の注意: FASTER を単純な log-index system として矮小化しない。

---

# 11. Conclusion

VMemKV explores a design point for larger-than-memory key-value stores: delegate value residency to OS virtual memory while retaining explicit engine control over hot metadata. Its T1/T2 split separates a RAM-resident metadata/control plane from an mmap-backed value/residency plane. Reorganization, checkpoint reload, and WAL replay are designed around T1 reachability so that VMemKV can repair fragmentation and recover state without making LSM-style multi-level compaction the central mechanism. The planned evaluation will characterize where this design is practical, where mmap and OS delegation become liabilities, and how much implementation responsibility is shifted away from the storage engine. VMemKV’s contribution is a design point and characterization, not a claim of universal dominance over RocksDB, BlobDB, LMDB, or explicit buffer-manager systems.

[日本語コメント]
狙い: 結果なしでも論文の closing を成立させる。
対応する評価: 全体。
根拠: planning documents consistently frame the paper as OS delegation + T1/T2 split + conditions/limitations characterization.
repoで確認すべき点: final conclusion should incorporate actual evaluation results later.
過剰主張の注意: “VMemKV achieves…” のような結果断定を避ける。

---

# References

[Bigtable] Fay Chang, Jeffrey Dean, Sanjay Ghemawat, Wilson C. Hsieh, Deborah A. Wallach, Mike Burrows, Tushar Chandra, Andrew Fikes, and Robert E. Gruber. *Bigtable: A Distributed Storage System for Structured Data*. OSDI 2006.

[LevelDB] Google. *LevelDB*. System/software reference. Full bibliographic citation to verify.

[RocksDB] Facebook/Meta. *RocksDB*. System/software reference. Full bibliographic citation to verify.

[RocksDB-Experience] RocksDB experience / tuning / production paper. Exact title, authors, venue, and year to verify.

[WiscKey] Lanyue Lu, Thanumalayan Sankaranarayana Pillai, Hariharan Gopalakrishnan, Andrea C. Arpaci-Dusseau, and Remzi H. Arpaci-Dusseau. *WiscKey: Separating Keys from Values in SSD-conscious Storage*. FAST 2016.

[BlobDB] RocksDB BlobDB / integrated BlobDB reference. Exact paper or documentation citation to verify.

[Bitcask] Basho. *Bitcask: A Log-Structured Hash Table for Fast Key/Value Data*. Technical report / system documentation. Exact citation to verify.

[LMDB] Howard Chu. *Lightning Memory-Mapped Database Manager (LMDB)*. System/software reference. Exact citation to verify.

[AreYouSureMMAP] Andrew Crotty, Alex Galakatos, and Tim Kraska / CMU database group. *Are You Sure You Want to Use MMAP in Your Database Management System?* CIDR 2022. Exact author list to verify.

[LeanStore] Viktor Leis et al. *LeanStore: In-Memory Data Management Beyond Main Memory*. Exact title, venue, and year to verify.

[vmcache] Alfons Kemper / Thomas Neumann group. *The vmcache paper/system*. Exact title, authors, venue, and year to verify.

[FASTER] Badrish Chandramouli, Guna Prasaad, Donald Kossmann, Justin Levandoski, James Hunter, and Mike Barnett. *FASTER: A Concurrent Key-Value Store with In-Place Updates*. SIGMOD 2018. Exact citation to verify.

[YCSB] Brian F. Cooper, Adam Silberstein, Erwin Tam, Raghu Ramakrishnan, and Russell Sears. *Benchmarking Cloud Serving Systems with YCSB*. SoCC 2010.

[Aether] Aether group commit / early lock release / flush pipelining reference mentioned in low-level design. Exact citation to verify.

[PredictiveTranslation] Predictive Translation. Optional short mention; exact citation to verify.

[ScaleCache] ScaleCache. Optional short mention; exact citation to verify.

[LLFREE] LLFREE. Optional short mention; exact citation to verify.

[LIPaH] LIPaH. Optional short mention; exact citation to verify.

[HowToWriteToSSDs] Paper on SSD-conscious write behavior. Exact title and citation to verify.

[日本語コメント]
狙い: fake citation を避け、確実なもの以外は verify に回す。
対応する評価: Related Work and Evaluation setup.
根拠: user instruction requested placeholders if full bibliographic info cannot be resolved.
repoで確認すべき点: must_read_papers に今後 PDF が増える可能性、reference metadata.
過剰主張の注意: 不確かな venue/year を捏造しない。

---

# Open questions / facts to verify

1. **Current implementation vs design documents.** The design docs include WAL, checkpoint reload, fork snapshotting, and WAL replay, while the inspected coordinator is labeled checkpoint-free. Verify which branch/file represents the paper artifact.

2. **Exact T1 `IndexEntry` layout.** Design docs define `key_prefix`, `hash`, and `payload_bits`; current code uses `StoreKey`, `hash`, atomic payloads, sorted/append slots, and optional structures. Verify final layout and size.

3. **Exact T2 record header and allocation policy.** The header fields are clear, but current append appears to set `alloc_len = value.size()`. Verify whether future allocation slack is planned.

4. **Exact in-place update condition.** Current code supports in-place update when the new value size is no larger than `alloc_len`; verify whether this is final.

5. **Whether “all-region in-place update” is accurate terminology.** Current implementation updates T2 records in place when allocation permits and updates T1 payloads in place, but the term may overstate the feature.

6. **Tombstone terminology.** Design docs use `TOMBSTONE_PAYLOAD` / `TOMBSTONE_OFFSET`, while code uses `STORE_NOT_FOUND = ~0ULL`. Verify naming and semantics.

7. **WAL fsync policy.** Design docs say WAL append + `fsync` before updates, and low-level parameters include `WAL_SYNC_MODE`; current visible code does not show WAL.

8. **Checkpoint completion protocol.** Verify atomic rename, commit marker, file fsync, directory fsync, and old-generation cleanup.

9. **Incomplete checkpoint handling.** Verify how recovery ignores or repairs partial checkpoint files.

10. **Recovery replay boundary.** Design says replay begins after snapshot LSN and ends at latest committed LSN during stop-the-world; verify implementation.

11. **Use of `mmap`, `fork`, `mincore`, `madvise`, `msync`, `fsync`.** Design mentions `mmap`, `fork`, `mincore`, `madvise`; current visible code confirms `mmap`, `munmap`, `mlock`, and `madvise`, but not all listed calls.

12. **MAP_SHARED vs MAP_PRIVATE.** Design checkpoint reload says new checkpoint files are opened with `mmap(MAP_PRIVATE)`; current initial map uses `MAP_PRIVATE`, while reorganization maps the temp file with `MAP_SHARED`. Verify intended durability and visibility semantics.

13. **`msync` / dirty writeback policy.** Evaluation plan requires VMemKV `msync` policy and writeback settings, but current visible code does not show `msync`.

14. **SIGBUS handling.** High-level design TODO notes mmap I/O errors surface as SIGBUS and require signal-handler design.

15. **Benchmark variants currently implemented.** Current benchmark includes nanobench ST operations, MT Get/Insert/Mixed, scale Get/Negative Get/Scan, VMemKV variants, and optional RocksDB.

16. **YCSB support status.** Planned evaluation uses YCSB load and workloads A/B/C/E/F, but current visible benchmark is not YCSB.

17. **RocksDB support status.** Current implementation includes optional RocksDB build support and wrapper; verify fairness options.

18. **BlobDB support status.** Planned evaluation requires RocksDB BlobDB for 16 KiB values, but inspected code only shows a thin RocksDB wrapper, not BlobDB configuration.

19. **Instrumentation support.** Planned metrics include page faults, p99/p99.9, bandwidth time series, cycles/op, TLB shootdowns, write amplification, cgroup memory, replay time, and recovery time. Current benchmark output appears focused on nanobench latency/relative results and M ops/s. Verify instrumentation.

20. **T2 capacity behavior.** Design says capacity shortage can queue writes until checkpoint/reload, while current T2 append throws `T2 storage capacity exceeded`. Verify final behavior.

21. **Concurrency contract.** Low-level design allows online operations with retry/snapshot and final stop-the-world during checkpoint reload. Current code uses key-stripe mutexes, reorganize mutex, atomics, and optimistic T1 reads. Verify final guarantees.

22. **Scan ordering guarantee.** Tests call `reorganize()` to guarantee sorted scan results for integral keys. Verify whether scan before reorganize promises sorted output or only range membership.

23. **Inline value conditions.** Current dynamic inlining applies when key length is no more than 16 bytes and value length is 1–8 bytes; verify if this is final or only one implementation mode.

24. **T1 memory hints.** Current memory hints use `mlock`, `MADV_HUGEPAGE`, and `MADV_SEQUENTIAL`; design also mentions T2 `MADV_WILLNEED`. Verify implementation status for T2 prefetch.

25. **Reference metadata.** Verify full bibliographic information for LevelDB/RocksDB experience paper, BlobDB, Bitcask, LMDB, LeanStore, vmcache, Aether, Predictive Translation, ScaleCache, LLFREE, LIPaH, and SSD write guidance.
