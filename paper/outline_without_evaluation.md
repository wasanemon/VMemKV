# VMemKV 論文: Evaluation を除いた章立て

このファイルは、`paper/evaluation_plan.md` の E0〜E7 に自然に接続する形で、Evaluation section 以外の論文構成を整理するためのメモである。

本文は最終的には英語で書くが、本メモは草稿化前の設計資料として日本語で記述する。

---

## 論文全体の流れ

VMemKV 論文は、以下の論理で進めるのがよい。

1. larger-than-memory な value 管理は、KVS 実装において大きな設計負担になる。
2. LSM-tree、key-value separation、append-only log + in-memory index、mmap-based KVS、explicit buffer manager は、それぞれ異なる trade-off を持つ。
3. VMemKV は、large value の residency 管理を OS に委譲しつつ、key metadata と lookup / scan control は RAM-resident T1 index に保持する。
4. これにより、VMemKV は user-space buffer pool や LSM-style multi-level compaction を中心機構にせず、単純な実装と実用的な性能の両立を狙う。
5. 評価では、この設計がどの条件で成立し、どの条件で崩れるかを E0〜E7 で確認する。

重要なのは、VMemKV を「RocksDB より常に速い KVS」として書かないことである。主張は、OS 委譲型 larger-than-memory 管理と T1/T2 責務分離が成立する条件を明らかにすることに置く。

---

## 章立て案

```text
Abstract

1. Introduction
   1.1 Larger-than-memory values as a storage-engine design problem
   1.2 Why not simply use LSM, value-log, mmap, or buffer-pool engines?
   1.3 VMemKV thesis: T1/T2 split + OS-delegated value residency
   1.4 Contributions and evaluation questions

2. Background and Motivation
   2.1 LSM-tree KVSs and compaction
   2.2 Key-value separation and value logs
   2.3 Append-only log + in-memory index designs
   2.4 mmap and OS page cache: benefits and known risks
   2.5 Explicit buffer managers for larger-than-memory systems

3. Design Goals and Scope
   3.1 Target workloads and assumptions
   3.2 Non-goals: transactions, SQL layer, phantom avoidance, replication
   3.3 Design principles: delegate value residency, retain metadata control
   3.4 Claims that the paper will and will not make

4. VMemKV Architecture Overview
   4.1 Component overview: T1, T2, WAL, checkpoint, background jobs
   4.2 Core invariant: T1 controls reachability, T2 stores values
   4.3 Why the split is necessary for larger-than-memory mmap values
   4.4 Lifecycle overview: load, read, update, scan, reorganize, recover

5. T1/T2 Data Model and Operation Paths
   5.1 T1 index layout and responsibilities
   5.2 T2 mmap-backed value region
   5.3 Get / Insert / Update / Delete paths
   5.4 Scan path and ordering fragmentation
   5.5 Optional T1 optimizations: AppendMap, BloomFilter, SimdScan, MemoryHints, Inline64

6. Reorganization, Checkpoint, and Recovery
   6.1 Why fragmentation arises
   6.2 T1-only reorganization
   6.3 T2 reorganization and checkpoint reload
   6.4 WAL replay and crash-recovery envelope
   6.5 Foreground interference and stop-the-world boundary

7. Implementation
   7.1 Repository structure and implementation status
   7.2 OS interface usage: mmap, mincore, madvise, fork, msync/fsync policy
   7.3 Durability configuration and fairness knobs
   7.4 Benchmark variants and instrumentation hooks
   7.5 Implementation responsibility table preview

8. Evaluation
   [別担当 / paper/evaluation_plan.md に従うため、この草稿対象からは除外]

9. Discussion and Limitations
   9.1 When OS-delegated value residency is expected to work
   9.2 When it may fail: page faults, writeback, TLB shootdowns, cgroups, fast SSDs
   9.3 Interpreting negative results
   9.4 What VMemKV intentionally does not solve

10. Related Work
   10.1 LSM-tree systems: Bigtable, LevelDB/RocksDB
   10.2 Key-value separation: WiscKey, BlobDB
   10.3 Append-only log + in-memory index: Bitcask
   10.4 mmap-based KVSs: LMDB and mmap critiques
   10.5 Larger-than-memory buffer managers: LeanStore, vmcache
   10.6 Hybrid log systems: FASTER

11. Conclusion
```

---

## 各章の役割

### Abstract

論文全体の主張を 1 段落で提示する。

この段階では評価結果がまだないため、`We show that ...` ではなく、`We evaluate whether ...`, `We investigate ...`, `Our evaluation is designed to answer ...` という表現にする。

Abstract では次を含める。

- VMemKV が standalone KVS であること。
- RAM-resident T1 index と mmap-backed T2 value layer を分離すること。
- OS 仮想メモリ機構に value residency 管理を委譲すること。
- user-space buffer pool や LSM-style multi-level compaction を中心機構にしないこと。
- 評価では成立条件と限界を明らかにすること。

### 1. Introduction

読者に「なぜ VMemKV という設計が必要なのか」を納得させる章である。

Introduction では、いきなり mmap の話から始めるより、larger-than-memory values が storage engine に要求する責務から始める方がよい。

書くべき流れ:

1. KVS が扱う value が RAM を超えると、buffering、eviction、write amplification、fragmentation repair、recovery が問題になる。
2. LSM-tree は実用的で強力だが、compaction と設定複雑性を持つ。
3. WiscKey / BlobDB は value を LSM から分離するが、value-log GC と crash consistency が新たな責務になる。
4. mmap-based KVS は OS に委譲できるが、page fault / eviction / writeback / TLB shootdown のリスクがある。
5. VMemKV は、metadata control は T1 に残し、large value residency は T2 + OS に委譲する設計を取る。
6. contribution と evaluation questions を提示する。

Evaluation への接続:

- E1: larger-than-memory behavior
- E2: RocksDB comparison
- E3: T1/T2 design breakdown
- E5: simple mmap baseline
- E7: implementation responsibility table

### 2. Background and Motivation

この章は文献の網羅ではなく、VMemKV の設計空間を作るための比較である。

扱うべき比較軸:

- LSM-tree systems: compaction, write amplification, scan support, production maturity
- Key-value separation: value write amplification reduction, value-log GC, range scan trade-off
- Append-only log + in-memory index: simplicity, point lookup, RAM-resident key directory, weak ordered scan
- mmap / OS page cache: copy reduction and implementation simplicity, but unpredictable eviction / writeback / TLB costs
- Explicit buffer manager: predictability and control, but implementation complexity

Evaluation への接続:

- E2 で RocksDB / BlobDB と比較する理由を導入する。
- E5 で simple mmap baseline を置く理由を導入する。
- E7 で implementation responsibility table を置く理由を導入する。

### 3. Design Goals and Scope

この章では、論文の scope を明確に限定する。

対象に含めるもの:

- standalone KVS
- Get / Insert / Update / Delete / Scan
- larger-than-memory workload
- T1/T2 separation
- reorganization
- checkpoint reload
- WAL replay recovery

対象に含めないもの:

- multi-operation transactions
- SQL layer
- phantom avoidance
- LineairDB / Kamo 側の concurrency control
- replication

ここで scope を明示しておかないと、査読者が「transactional consistency はどうするのか」「SQL layer はどうするのか」「replication はどうするのか」と要求してくる可能性が高い。

Evaluation への接続:

- E0 の fairness 設定。
- E6 の recovery sanity check。
- E7 の responsibility table。

### 4. VMemKV Architecture Overview

この章では、VMemKV の全体像を図とともに説明する。

中心となる説明:

- T1 は RAM-resident な index / metadata layer。
- T2 は file-backed mmap 上の value layer。
- WAL は更新の durability を担う。
- Checkpoint は T1/T2 state の復旧基点を作る。
- Background jobs は reorganization / checkpoint reload などを担う。

重要な invariant:

- T2 の live record は T1 の live entry から到達可能である。
- T1 から参照されない T2 record は garbage であり、reorganization の対象になる。

repoで要確認:

- 上記 invariant が現在の設計・実装で本当に成立しているか。
- T1/T2 record の final format。
- checkpoint / WAL の順序関係。

Evaluation への接続:

- E3: T1/T2 design breakdown
- E4: reorganization and checkpoint behavior
- E5: simple mmap baseline

### 5. T1/T2 Data Model and Operation Paths

この章では、Architecture Overview より一段具体的に、T1/T2 のデータ構造と操作経路を書く。

書くべき内容:

- T1 `IndexEntry` の役割。
- `sorted_region` と `append_region` の役割。
- T1 payload が T2 offset または inline value を表す可能性。
- T2 が variable-size record を mmap-backed byte array に置くこと。
- insert / update のとき、T2 への append または in-place update が起きること。
- Get / Insert / Update / Delete / Scan の path。
- Optional optimizations: AppendMap, BloomFilter, SimdScan, MemoryHints, Inline64。

過剰主張を避ける点:

- Optional optimization の効果は評価前に断定しない。
- in-place update を中心主張にしない。
- scan が常に速いとは言わない。

Evaluation への接続:

- E1: DRAM-resident vs larger-than-memory behavior
- E2: read-heavy / update-heavy / scan-heavy workload
- E3: variant breakdown
- E5: simple mmap baseline

### 6. Reorganization, Checkpoint, and Recovery

この章では、VMemKV が単なる mmap file access ではなく、長時間稼働する KVS として必要な background repair と recovery を持つことを説明する。

書くべき内容:

- update / delete によって ordering fragmentation と storage fragmentation が発生すること。
- T1-only reorganization と T2 reorganization の違い。
- T2 reorganization が live record を再配置し、garbage bytes を回収すること。
- checkpoint reload がどの段階で foreground operation を止めるか。
- WAL replay による recovery の範囲。

注意点:

- 「compaction がない」と書くのではなく、「LSM-style multi-level compaction を中心機構にしない」と書く。
- recovery は exhaustive crash testing ではなく sanity check として扱う。
- checkpoint の foreground interference は評価で測る対象であり、事前に小さいと断定しない。

Evaluation への接続:

- E4: reorganization and checkpoint behavior
- E6: recovery and crash-consistency sanity check
- E2: write amplification / storage usage

### 7. Implementation

この章では、設計が実装としてどのように現れているかを書く。

書くべき内容:

- repository structure
- T1/T2/WAL/checkpoint/background job の module 構成
- OS interface usage: `mmap`, `mincore`, `madvise`, `fork`, `msync`, `fsync`
- durability configuration
- MAP_SHARED / MAP_PRIVATE の扱い
- benchmark variants
- instrumentation hooks
- implementation responsibility table の前振り

repoで要確認:

- 実際に使っている syscall と、設計上想定しているだけの syscall の区別。
- `msync` / `fsync` policy。
- WAL sync policy。
- benchmark output が CSV / JSON で出せるか。
- page faults / RSS / cgroup memory / perf counters / TLB shootdowns を測れるか。

Evaluation への接続:

- E0: setup and fairness
- E1/E2: performance counters and OS metrics
- E3: variants
- E7: responsibility table

### 9. Discussion and Limitations

Evaluation の後に置く章だが、evaluation 以外の草稿を書く段階でも骨格を作っておくべき章である。

書くべき内容:

- VMemKV がうまくいくと予想される条件。
- VMemKV が苦手になりうる条件。
- mmap 批判との関係。
- RocksDB に負けた場合の解釈。
- simple mmap baseline が速かった場合の解釈。
- production KVS として残る課題。

重要な点:

この章を用意しておくことで、評価結果が強く出なかった場合でも、論文の主張を「成立条件の characterization」として維持できる。

### 10. Related Work

Related Work は網羅ではなく、VMemKV の主張に必要な比較軸だけを残す。

本文に残すべきもの:

- Bigtable / RocksDB / LevelDB: LSM-tree baseline
- WiscKey / BlobDB: key-value separation
- Bitcask: append-only log + in-memory index
- LMDB: mmap-based KVS
- mmap criticism: mmap の既知リスク
- vmcache / LeanStore: larger-than-memory buffer management
- FASTER: hybrid log 系の比較対象

Appendix / 短い言及に回してよいもの:

- LevelDB 単体の詳細
- FASTER の細部
- Predictive Translation
- ScaleCache
- LLFREE
- LIPaH
- How to Write to SSDs の細部。ただし write amplification / SSD-conscious design の文脈では本文に一部入れてもよい。

### 11. Conclusion

Conclusion では、最終的な評価結果に応じて strong / modest な結論を選ぶ。

評価結果が良い場合:

- target workload で practical performance を示した、と書ける。
- T1/T2 split が simple mmap baseline との差分を作った、と書ける。

評価結果が mixed の場合:

- OS-delegated design の成立条件と限界を明らかにした、と書く。
- RocksDB に対して常に勝つのではなく、特定条件で競争力がある、と書く。

評価結果が弱い場合:

- mmap-based larger-than-memory KVS の限界を T1/T2 split とともに実験的に characterization した、と書く。
- ただし、この場合は system paper としての主張を慎重に再構成する必要がある。
