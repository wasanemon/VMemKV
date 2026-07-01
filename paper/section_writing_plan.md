# VMemKV 論文: 各 Section の執筆計画

このファイルは、VMemKV 論文の evaluation 以外の各 section について、目的、書くべき内容、避けるべき過剰主張、使うべき先行研究、repo で確認すべき実装事実、後で evaluation に接続する主張を整理するためのメモである。

評価結果はまだないため、本文草稿では結果を断定しない。評価に接続する箇所では、`We show that ...` ではなく、`We evaluate whether ...`, `We investigate ...`, `Our evaluation is designed to answer ...` という書き方にする。

---

## 全体方針

VMemKV 論文の中心主張は、以下の 2 点である。

1. OS 仮想メモリ機構に larger-than-memory な value residency 管理をできるだけ委譲すること。
2. RAM-resident な T1 index と mmap-backed な T2 value layer の責務分離によって、単純な実装と実用的な性能の両立を狙うこと。

in-place update は重要だが、中心主張ではなく update-heavy workload における補助的な強みとして扱う。特に、既存 allocation に収まる更新で T2 garbage generation、storage usage、reorganization pressure を抑えうる点を説明する。ただし、任意サイズの更新が常に in-place 可能であるとは断定しない。

---

## Section-by-section writing plan

| Section | 目的 | 書くべき内容 | 避けるべき過剰主張 | 使うべき先行研究 | repo で確認すべき実装事実 | 後で evaluation に接続する主張 |
|---|---|---|---|---|---|---|
| Abstract | 論文全体の主張を 1 段落で固定する | VMemKV が standalone KVS であること。T1/T2 split。OS VM delegation。user-space buffer pool や LSM-style multi-level compaction を中心機構にしないこと。評価は成立条件と限界を調べるものだと書く。 | `We show that VMemKV outperforms RocksDB` のような結果断定。`VMemKV solves larger-than-memory KVS` のような一般解主張。 | RocksDB, WiscKey, Bitcask, mmap criticism, vmcache | `mmap`, `fork`, `mincore`, `madvise` の実際の使用範囲。WAL/checkpoint/recovery の実装範囲。standalone API の範囲。 | E1, E2, E3, E5, E7 に自然に接続する。結果ではなく evaluation questions を提示する。 |
| 1. Introduction | 査読者に「なぜ VMemKV が必要か」を納得させる | larger-than-memory values が storage engine に要求する責務。LSM/value-log/mmap/buffer-manager の trade-off。VMemKV の T1/T2 split。in-place update は補助的強みとして短く触れる。contribution。評価で答える問い。 | 「VMemKV は全 workload で最速」。`mmap` が常に良い。key-value separation 自体が新規。in-place update が中心 contribution であるかのような書き方。 | Bigtable/RocksDB は LSM の代表。WiscKey/BlobDB は key-value separation。Bitcask は simple log-indexed KVS。mmap criticism は反対意見。vmcache は VM-assisted buffer manager。 | API が Get/Insert/Update/Delete/Scan を本当に含むか。transaction, SQL, replication を除外してよいか。T1/T2/WAL/checkpoint の名称と責務。in-place update の条件。 | E1: OS 委譲型 larger-than-memory behavior。E2: YCSB-based comparison with RocksDB。E3: T1/T2 breakdown。E5: mmap-only microbaseline。 |
| 2. Background and Motivation | VMemKV が入る設計空間を作る | LSM-tree KVS, key-value separation, append-only log + in-memory index, mmap-based KVS, explicit buffer manager の比較軸を整理する。 | Related Work の網羅にしない。文献紹介だけで終わらせない。VMemKV がなぜ必要かに接続する。 | Bigtable, LevelDB/RocksDB, WiscKey, BlobDB, Bitcask, LMDB, Are You Sure You Want to Use MMAP?, vmcache, LeanStore, FASTER | 文献の正確な引用情報。BlobDB/LMDB/FASTER/LeanStore をどこまで本文に残すか。 | E2 の YCSB-based RocksDB/BlobDB 比較、E5 の mmap-only microbaseline、E7 の responsibility table の理由づけ。 |
| 3. Design Goals and Scope | 主張の境界を明確化する | Target workload。standalone KVS。Get/Insert/Update/Delete/Scan。larger-than-memory workload。T1/T2 separation。reorganization。checkpoint reload。WAL replay。Non-goals: transaction, SQL layer, phantom avoidance, LineairDB/Kamo concurrency control, replication。 | DBMS 全体の提案に見せない。transactional storage engine と誤解させない。in-place update を中心主張にしない。 | RocksDB は production KVS の広さを示す比較対象。Bitcask は scope を絞った local KVS の例。 | `paper/evaluation_plan.md` の scope と実装/API が一致するか。Delete/Scan/WAL/recovery の実装状態。 | E0 の fairness。E6 の recovery sanity。E7 の responsibility table。 |
| 4. VMemKV Architecture Overview | 読者に全体像と不変条件を与える | T1, T2, WAL, Checkpoint, Background Jobs。T1 は reachability / metadata / lookup / scan control。T2 は mmap-backed value bytes。T1 から到達可能な T2 record が live であるという invariant。 | T1/T2 を単なる cache/file と弱く見せない。逆に完全に新しい storage architecture と盛りすぎない。 | WiscKey の LSM+vLog。Bitcask の keydir+log。vmcache の VM-assisted buffer manager。 | T1 の live entry と T2 live record の到達可能性 invariant。T2 garbage 判定。T1/T2 reorganization の実装状態。 | E3 で T1/T2 split の効果を見る。E4 で reorganization/checkpoint。E5 で mmap-only microbaseline と比較する。 |
| 5. T1/T2 Data Model and Operation Paths | 設計を具体化する | T1 `IndexEntry`, `sorted_region`, `append_region`, payload/offset。T2 variable-size record, offset, alloc_len。Get/Insert/Update/Delete/Scan の path。Update semantics と in-place update。Optional T1 optimizations: AppendMap, BloomFilter, SimdScan, MemoryHints, Inline64。 | Optional optimization の効果を断定しない。scan が常に速いと言わない。in-place update を主 contribution にしない。任意サイズの更新が常に in-place 可能と書かない。 | WiscKey は pointer-to-value。Bitcask は keydir。RocksDB は MemTable/SSTable/Bloom/filter/compaction。 | `T1Config` の flags。AppendMap/BloomFilter/SimdScan/MemoryHints/Inline64 の完成度。Delete/tombstone の扱い。Scan ordering の保証。full key の扱い。in-place update と append update の正確な条件。 | E1 の DRAM-resident vs larger-than-memory。E2 の YCSB read/update/scan。E3 の variant breakdown。E5 の mmap-only microbaseline。 |
| 6. Reorganization, Checkpoint, and Recovery | system completeness を示す | fragmentation の種類。in-place update が garbage generation を抑えうるが不要化しないこと。T1-only reorganization。T2 reorganization。checkpoint reload。WAL replay。foreground interference。stop-the-world boundary。 | 「compaction 不要」と書かない。正確には LSM-style multi-level compaction を中心機構にしない。reorganization cost が小さいと事前に断定しない。crash consistency を完全証明済みのように書かない。in-place update で GC が不要になるとは書かない。 | LSM compaction。WiscKey vLog GC。Bitcask merge。mmap writeback criticism。 | checkpoint completion protocol。atomic rename / commit marker。WAL replay start/end LSN。incomplete checkpoint の扱い。fork 後の CoW cost。stop-the-world 範囲。 | E4 で reorganization/checkpoint cost。E6 で recovery sanity。E2 で write amplification / storage usage。 |
| 7. Implementation | 設計と実装の対応を説明する | repo structure。T1/T2/WAL/checkpoint/background job modules。OS calls。MAP_SHARED / MAP_PRIVATE。msync/fsync policy。durability knobs。benchmark variants。instrumentation hooks。 | 「実装が簡単」を主観で書かない。LoC だけを強調しない。未実装機能を断定しない。 | RocksDB 実運用論文。vmcache/LeanStore explicit buffer manager。mmap criticism。 | 実際の syscall 使用箇所。fsync/msync policy。WAL sync policy。cgroup measurement support。perf counters。benchmark CSV/JSON output。 | E0 の setup/fairness。E1/E2 の metrics。E3 の variants。E7 の responsibility table。 |
| 9. Discussion and Limitations | 評価結果が mixed でも論文 story を保てるようにする | mmap が効く条件と効かない条件。page faults, TLB shootdowns, dirty writeback, cgroup pressure, fast SSD scaling。RocksDB に負けた場合の解釈。mmap-only microbaseline が速かった場合の解釈。in-place update の効果が限定的だった場合の解釈。 | 弱点を隠さない。mmap 批判を無視しない。negative result を失敗としてだけ扱わない。 | Are You Sure You Want to Use MMAP?, vmcache, RocksDB, WiscKey | 評価結果後に具体化する。現時点では risk として記述する。 | E1/E5 が negative でも「成立条件の characterization」として接続する。 |
| 10. Related Work | VMemKV の位置づけを整理する | LSM-tree systems, key-value separation, append-only log + in-memory index, mmap-based KVS, larger-than-memory buffer managers, hybrid log systems を比較軸で整理する。 | 文献網羅を目的にしない。各文献を VMemKV の主張に必要な軸だけで使う。 | Bigtable, RocksDB, WiscKey, BlobDB, Bitcask, LMDB, mmap criticism, vmcache, LeanStore, FASTER | BlobDB/LMDB/FASTER/LeanStore の引用情報。本文に残す文献と短い言及に回す文献の判断。 | E2, E5, E7 の比較対象が Related Work と整合していること。 |
| 11. Conclusion | 評価結果に応じて主張を締める | OS delegation + T1/T2 split + responsibility reduction を再掲する。最終稿では評価結果を短くまとめる。 | 結果がない段階で `VMemKV achieves ...` と書かない。RocksDB に常勝したように書かない。 | 最重要比較だけ短く触れる。 | なし。評価結果後に更新する。 | 結果が強ければ target workload で practical performance、mixed なら成立条件と限界の characterization として締める。 |

---

## Section ごとの詳細メモ

### Abstract

Abstract は最後に書くべきである。現時点では、評価結果がないため、結果断定を避けた provisional abstract に留める。

入れるべき要素:

- VMemKV is a standalone key-value store.
- VMemKV separates a RAM-resident T1 index from an mmap-backed T2 value layer.
- VMemKV delegates larger-than-memory value residency to OS virtual memory mechanisms.
- VMemKV avoids a user-space buffer pool and LSM-style multi-level compaction as central mechanisms.
- The evaluation is designed to characterize practicality, target workloads, and limitations.

避ける表現:

- `We show that VMemKV is faster than RocksDB.`
- `VMemKV eliminates compaction.`
- `VMemKV solves larger-than-memory storage.`

---

### 1. Introduction

Introduction は、以下の段落構成が自然である。

1. Larger-than-memory value management problem
2. Existing approaches and their trade-offs
3. VMemKV design intuition
4. Scope and non-goals
5. Contributions
6. Evaluation questions preview

Introduction の最後では、以下のように evaluation を予告する。

> Our evaluation is designed to answer whether OS-delegated value residency can provide practical performance under larger-than-memory conditions, whether the T1/T2 split matters beyond raw mmap-backed value access, and how much implementation responsibility VMemKV avoids compared with LSM-tree and buffer-manager-based designs.

この文は結果断定を避けつつ、E1/E3/E5/E7 に接続できる。

---

### 2. Background and Motivation

Background は Related Work と重複しやすいので、設計動機に必要な範囲に絞る。

推奨する順序:

1. LSM-tree KVSs
2. Key-value separation
3. Append-only log + in-memory index
4. mmap and OS page cache
5. Explicit buffer managers

ここでの主張は、「どの既存方式も間違っている」ではなく、「それぞれ別の責務と trade-off を持つ」である。

---

### 3. Design Goals and Scope

この章は査読者の要求を制御するために重要である。

明確に書くべき non-goals:

- multi-operation transactions
- SQL layer
- phantom avoidance
- replication
- LineairDB / Kamo 側の concurrency control

VMemKV は standalone KVS として書く。DBMS 全体や transactional storage engine として書くと、査読者の期待が大きくなりすぎる。

---

### 4. VMemKV Architecture Overview

この章には必ず概念図を置くべきである。

図の候補:

```text
Client API
  | Get / Put / Update / Delete / Scan
  v
T1: RAM-resident index and metadata layer
  | key -> T2 offset / inline payload
  v
T2: mmap-backed value region
  | OS page cache / page faults / writeback
  v
SSD / filesystem

Side components:
  - WAL
  - Checkpoint
  - Reorganization jobs
```

この図では、T1 を control plane、T2 を data / residency plane として見せる。

---

### 5. T1/T2 Data Model and Operation Paths

この章では、読者が E3 の ablation を理解できるだけの detail を入れる。

最低限必要な説明:

- T1 entry が key metadata と T2 pointer を持つ。
- T1 は RAM-resident である。
- T2 は mmap-backed byte region である。
- T2 record は variable-size value を持つ。
- 更新時には in-place update できる場合と append になる場合がある。
- Delete は T1/T2 上でどのように表現されるか。repoで要確認。
- Scan は T1 の ordering と T2 の value fetch の組み合わせで実行される。

#### Update semantics and in-place update

in-place update は補助的な強みとして、以下の範囲で説明する。

- New value が既存 record allocation に収まる場合、VMemKV は mmap-backed T2 region 上で in-place update できる。
- New value が既存 allocation を超える場合、VMemKV は新しい T2 record を append し、T1 の offset を差し替える。
- この性質は、pure append-only value-log design と比べて fixed-size / bounded-growth update-heavy workload で T2 garbage generation と reorganization pressure を抑えうる。
- ただし、任意の update が常に in-place で処理できるわけではない。

---

### 6. Reorganization, Checkpoint, and Recovery

この章では、VMemKV が長時間運用で避けられない fragmentation と recovery をどう扱うかを書く。

重要な区別:

- T1-only reorganization: index layout / ordering / append region の整理。
- T2 reorganization: T1 から参照されない garbage record を除去し、live value を再配置する。
- Checkpoint reload: checkpoint と WAL replay によって復旧状態を作る。

in-place update との関係:

- in-place update は T2 garbage generation を減らしうる。
- しかし delete、value-growth update、allocation を超える update では garbage record が発生する。
- したがって、in-place update は reorganization を不要にする機構ではなく、reorganization pressure を下げる補助的機構として説明する。

repoで要確認:

- T2 reorganization が checkpoint reload の一部としてのみ実行されるか。
- stop-the-world の範囲。
- WAL 永続化後に T1/T2 更新する順序。
- checkpoint 中に fork を使う場合の parent/child の役割。

---

### 7. Implementation

Implementation では、設計を実装詳細に落とす。ただし、評価結果や性能値は書かない。

書くべき観点:

- repository structure
- core API
- T1 implementation
- T2 implementation
- WAL/checkpoint implementation
- benchmark variants
- metrics instrumentation

Implementation の最後に、E7 の responsibility table に接続する前振りを置く。

例:

> To make the simplicity claim auditable, our evaluation includes an implementation-responsibility comparison rather than relying only on lines of code or subjective complexity.

---

### 9. Discussion and Limitations

この章は、mmap 批判と negative result に耐えるために重要である。

必ず入れるべき論点:

- mmap は OS に制御を委ねるため、tail latency と predictability に弱点がある。
- cgroup memory limit 下では RSS だけで memory budget を比較できない。
- fast SSD / many-core 環境では page fault, eviction, TLB shootdown が支配的になりうる。
- RocksDB に負けた場合でも、VMemKV の設計価値は成立条件の characterization と responsibility reduction に残る。
- mmap-only microbaseline が速い場合でも、VMemKV は T1/T2 organization、durability、reorganization、recovery、scan control の cost を測る対象として意味がある。
- in-place update の効果が限定的でも、中心主張は OS-delegated larger-than-memory management と T1/T2 responsibility split にあるため崩れない。

---

### 10. Related Work

Related Work は以下の比較軸で整理する。

#### LSM-tree systems

本文に残す:

- Bigtable
- LevelDB / RocksDB

主に書くこと:

- MemTable, SSTable, WAL, compaction
- write amplification / space amplification / read amplification
- production maturity and configurability

#### Key-value separation

本文に残す:

- WiscKey
- BlobDB, if evaluation includes RocksDB BlobDB

主に書くこと:

- LSM tree stores keys and value pointers
- values are stored in a separate value log
- value-log GC and range-scan trade-off
- VMemKV との近さと差分
- VMemKV は append-only value-log だけではなく、allocation が許す範囲で mmap-backed T2 region を in-place update できる点を補助的差分として述べる

#### Append-only log + in-memory index

本文に残す:

- Bitcask

主に書くこと:

- keydir
- append-only data files
- simple point lookup
- RAM-resident key directory
- merge / garbage collection
- ordered scan の弱さ

#### mmap-based KVS and mmap criticism

本文に残す:

- LMDB
- Are You Sure You Want to Use MMAP?

主に書くこと:

- mmap の利点: copy reduction, simple implementation, OS page cache
- mmap のリスク: page faults, eviction, dirty writeback, TLB shootdown, predictability
- VMemKV は mmap 批判を無視せず、value layer に対象を絞る

#### Larger-than-memory buffer managers

本文に残す:

- LeanStore
- vmcache

主に書くこと:

- explicit buffer management
- pointer swizzling / VM-assisted translation
- DBMS-controlled eviction
- VMemKV は buffer manager を持たず OS に委譲する点で異なる

#### Hybrid log systems

本文に残すか短い言及:

- FASTER

主に書くこと:

- hash index + hybrid log
- in-memory / on-disk log の階層
- VMemKV とは T1/T2 split と mmap value residency の焦点が異なる

---

## Figure / Table 配置案

Evaluation 前に置くべき図表は以下。

| 図表 | 置く section | 目的 | Evaluation との接続 |
|---|---|---|---|
| Figure 1: VMemKV architecture overview | Section 4 | T1/T2/WAL/checkpoint/background jobs の全体像を示す | E1, E3, E4, E6 |
| Figure 2: T1/T2 responsibility split | Section 4 or 5 | T1 が metadata/control、T2 が value/residency を担うことを示す | E3, E5, E7 |
| Figure 3: Read / write / update / scan paths | Section 5 | 各 operation が T1/T2/WAL をどう通るかを示す | E1, E2, E3 |
| Figure 4: Update path: in-place vs append update | Section 5 | 既存 allocation に収まる更新と append に落ちる更新を区別する | E2, E4 |
| Figure 5: Reorganization / checkpoint flow | Section 6 | foreground operation, fork/checkpoint, WAL replay, stop-the-world boundary を示す | E4, E6 |
| Table 1: Design responsibility comparison | Section 7 or Related Work | buffer pool, replacement, compaction, value-log GC, recovery などの責務を比較する | E7 |
| Table 2: Related work positioning | Related Work | LSM, value separation, mmap, buffer manager, hybrid log の位置づけを整理する | E2, E5, E7 |

---

## 草稿化前のリスクチェック

| リスク | 査読者からの見え方 | 本文での対処 | Evaluation での支え方 |
|---|---|---|---|
| 新規性が弱く見える | Bitcask + mmap、WiscKey の value log を mmap にしただけ、LMDB の変種では、と見られる | key-value separation 単体ではなく、T1/T2 control/data split + OS-managed value residency の組み合わせとして書く | E3, E5, E7 |
| mmap 批判との衝突 | mmap は DB には危険という既存批判があるのに、なぜ採用するのかと問われる | 批判を Discussion で正面から扱う。VMemKV は mmap を value layer に限定して使うと説明する | E1, E5 |
| RocksDB に負けた場合 | 性能で負けるなら価値がないと見られる | 全 workload で勝つ主張を避け、成立条件と苦手条件の characterization として書く | E2 |
| mmap-only microbaseline が速かった場合 | VMemKV の工夫が不要に見える | mmap-only microbaseline は durability / recovery / reorganization のない raw mmap-backed value access の参照点と位置づける | E5, E4, E6 |
| in-place update の効果が限定的な場合 | update-heavy workload での補助的強みが弱く見える | in-place update は中心主張ではなく、fixed-size / bounded-growth update で garbage generation を抑えうる補助機構として書く | E2, E4 |
| 実装が簡単という主張が主観的 | 著者の感想に見える | responsibility table で客観化する | E7 |
| T1/T2 分離が実装都合に見える | index と values を分けただけに見える | T1 を control plane、T2 を OS-managed residency plane として説明する | E3, E5 |
| recovery / durability 主張が強すぎる | production-grade crash consistency を主張しているように見える | durability scope を明示し、E6 は sanity check と書く | E0, E6 |
| reorganization が compaction と同じに見える | compaction なしと言いながら GC していると突かれる | LSM-style multi-level compaction ではない、と限定する | E4 |

---

## Open questions / facts to verify

本文草稿を書く前に、以下を repo で確認する必要がある。

- 対象ブランチの design docs と実装が一致しているか。
- T1 `IndexEntry` の final layout。
- T1 が full key を持つか、prefix/hash/StoreKey を持つか。
- T1 `sorted_region` / `append_region` の現在の意味。
- AppendMap / BloomFilter / SimdScan / MemoryHints / Inline64 の実装完成度。
- T2 record header, offset, `alloc_len`, tombstone/delete, checksum の有無。
- in-place update と append update の正確な条件。
- 「全領域 in-place update 可能」と言う場合の正確な意味。特に、既存 allocation に収まる場合だけなのか、任意サイズ更新も含むのか。
- WAL 永続化と T1/T2 更新の順序。
- checkpoint completion protocol。
- incomplete checkpoint の扱い。
- recovery replay の開始 LSN / 終了 LSN。
- `mmap`, `fork`, `mincore`, `madvise`, `msync`, `fsync` の実使用箇所。
- MAP_SHARED / MAP_PRIVATE の使い分け。
- benchmark variants と metrics output。
- cgroup memory limit / page fault / TLB shootdown / device bytes written を測定できるか。
