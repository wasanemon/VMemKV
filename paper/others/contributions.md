# VMemKV 論文: Contribution 案

このファイルは、VMemKV 論文の評価以外の本文を書く前に、主張を 3〜4 個の contribution に絞るための整理メモである。

前提として、本論文は VMemKV がすべての workload で最速であることを主張しない。中心は以下の 2 点である。

1. OS 仮想メモリ機構に larger-than-memory な read-side value residency 管理をできるだけ委譲すること。
2. RAM-resident な T1 index と mmap-backed な T2 value layer の責務分離によって、単純な実装と実用的な性能の両立を狙うこと。

in-place update は重要な特徴だが、中心主張ではなく update-heavy workload における補助的な強みとして扱う。

---

## Contribution 1: OS-delegated larger-than-memory value residency

### 何を主張するか

VMemKV は、large value の read-side residency / eviction の多くを OS の仮想メモリ機構に委譲する standalone KVS である。

従来の storage engine が持つ user-space buffer pool、page replacement policy、multi-level compaction を VMemKV の中心機構には置かず、`mmap`, `madvise` などの OS 機構を活用して larger-than-memory な value 領域を扱う設計を検討する。durability は T2 file writeback ではなく、WAL と checkpoint / recovery によって担保する。

ただし、これは「OS page cache が常に DBMS buffer manager を置き換えられる」という主張ではない。正確には、VMemKV は value layer に対象を絞ることで、どの条件で OS 委譲型設計が成立するかを評価する。

### どの section で説明するか

- Introduction
- Background and Motivation
- Design Goals and Scope
- VMemKV Architecture Overview
- T2 Value Layer
- Discussion and Limitations

### 対応する evaluation question / experiment

- Q1: OS 委譲型 larger-than-memory 管理は機能するか？
- E0: Experimental setup and fairness
- E1: Larger-than-memory behavior and thread scalability
- E5: Simple mmap KVS upper-bound baseline
- E7: Implementation responsibility table

### 先行研究との差分

- RocksDB / LevelDB / Bigtable 系の LSM-tree は、MemTable, SSTable, WAL, compaction によってデータを管理する。
- WiscKey / BlobDB は key-value separation により large value を LSM compaction から切り離すが、value log と GC を storage engine 側で管理する。
- LMDB は mmap を使う代表的な KVS だが、VMemKV は RAM-resident T1 と mmap-backed T2 の責務分離を中心に置く。
- vmcache は仮想メモリを buffer manager に活用するが、page fault / eviction / replacement policy は DBMS 側で制御する。VMemKV は read-side value residency の多くを OS 側に委譲する点で異なる。

### 過剰主張になりそうな点

避けるべき表現:

- 「mmap は DBMS において常に高速である」
- 「OS page cache は user-space buffer pool を完全に置き換えられる」
- 「VMemKV は RocksDB より常に高速である」
- 「VMemKV は larger-than-memory KVS の一般解である」

安全な表現:

- 「VMemKV investigates whether ...」
- 「VMemKV evaluates the conditions under which ...」
- 「VMemKV delegates read-side value residency, while keeping key metadata under explicit engine control.」

---

## Contribution 2: T1/T2 responsibility split

### 何を主張するか

VMemKV の中核は、RAM-resident な T1 index と mmap-backed な T2 value layer の責務分離である。

T1 は key metadata、lookup、scan control、T2 offset、最適化 hook を管理する。T2 は large value record を file-backed `MAP_PRIVATE` mmap 上の byte region として保持し、clean read-side pages の residency は OS に委譲する。

この分離により、VMemKV は単なる mmap file access ではなく、hot metadata は engine が制御し、clean read-side value pages は OS が管理する control/data split として設計される。

### どの section で説明するか

- VMemKV Architecture Overview
- T1/T2 Data Model
- Operation Paths
- Implementation

### 対応する evaluation question / experiment

- Q3: T1/T2 の責務分離は効いているか？
- E1: Larger-than-memory behavior and thread scalability
- E2: RocksDB comparison
- E3: T1/T2 design breakdown
- E5: Simple mmap KVS upper-bound baseline

### 先行研究との差分

- WiscKey は LSM-tree に key と value pointer を持ち、value を vLog に分離する。VMemKV も key-value separation に近いが、T2 を mmap-backed value region とし、OS-managed residency を中心に置く。
- Bitcask は in-memory keydir と append-only data file を持つ単純な設計だが、ordered scan には向きにくい。VMemKV は T1 側で scan control を担う設計として位置づける。
- LMDB は mmap-based KVS の代表だが、VMemKV は mmap を主に value layer に使い、index metadata を T1 として分離する。

### 過剰主張になりそうな点

避けるべき表現:

- 「key-value separation 自体が新しい」
- 「T1/T2 分離だけで常に高性能になる」
- 「T2 の mmap access は always near memory speed」

安全な表現:

- 「The novelty is not key-value separation alone, but its combination with OS-managed read-side value residency.」
- 「T1/T2 split is evaluated against a simple mmap baseline.」
- 「T1/T2 split is intended to separate metadata control from read-side value residency management.」

### repo で要確認

- T1 の `IndexEntry` / `payload_bits` / `sorted_region` / `append_region` の最終仕様。
- T1 が full key を持つのか、prefix / hash / StoreKey を持つのか。
- T2 offset, record header, `alloc_len`, tombstone/delete の形式。
- T1/T2 reorganization の実装範囲。
- AppendMap, BloomFilter, SimdScan, MemoryHints, Inline64 の実装完成度。

---

## Contribution 3: Fragmentation repair and recovery without LSM-style multi-level compaction as the central mechanism

### 何を主張するか

VMemKV は、LSM-tree の multi-level compaction を中心機構にせず、T1/T2 reorganization、in-process checkpoint / reload、WAL replay によって fragmentation repair と recovery を扱う。

更新や削除により T2 上には T1 から参照されない garbage record が発生する。VMemKV はこれを reorganization によって回収する。また、checkpoint と WAL replay により crash 後の復旧を行う設計を持つ。

ただし、reorganization は広い意味では GC / compaction に相当する処理である。そのため「VMemKV は compaction を一切持たない」とは書かず、「LSM-style multi-level compaction を中心機構にしない」と限定して書く。

### どの section で説明するか

- Reorganization, Checkpoint, and Recovery
- Implementation
- Discussion and Limitations

### 対応する evaluation question / experiment

- Q4: Reorganization / checkpoint reload は有効か？
- Q5: Durability / recovery は成立するか？
- E2: RocksDB comparison, especially write amplification and storage usage
- E4: Reorganization and checkpoint behavior
- E6: Recovery and crash-consistency sanity check

### 先行研究との差分

- LSM-tree systems は compaction により古い version や tombstone を整理する。
- WiscKey / BlobDB は value log GC により obsolete value を回収する。
- Bitcask は merge により古い entry / tombstone を回収する。
- VMemKV は T1 から到達可能な T2 record を live とみなし、T1/T2 reorganization と in-process checkpoint / reload を中心に fragmentation を修復する設計として位置づける。

### 過剰主張になりそうな点

避けるべき表現:

- 「VMemKV は GC が不要である」
- 「VMemKV は compaction cost を完全に排除する」
- 「checkpoint は foreground に影響しない」
- 「crash consistency は完全に証明済みである」

安全な表現:

- 「VMemKV replaces LSM-style multi-level compaction with reorganization tailored to the T1/T2 split.」
- 「The evaluation measures foreground interference during reorganization and in-process checkpoint / reload.」
- 「Recovery is evaluated as a sanity check rather than exhaustive crash testing.」

### repo で要確認

- T1-only reorganization と T2 reorganization の区別。
- T2 reorganization が checkpoint / reload の一部としてのみ実行されるか。
- checkpoint file の完成判定。
- atomic rename / commit marker の有無。
- WAL replay の開始 LSN / 終了 LSN。
- incomplete checkpoint の扱い。
- checkpoint serialization / generation switch / foreground pause の範囲。

---

## Contribution 4: Objective implementation-responsibility comparison

### 何を主張するか

VMemKV の「実装が単純」という主張は、主観的な印象ではなく、storage engine が担う責務の比較として示す。

比較軸は、user-space buffer pool、page replacement policy、multi-level compaction、value-log GC、mmap-backed value storage、OS-managed read-side value residency、ordered scan support、background repair / reorganization、in-place update、recovery mechanism などである。

この contribution は主技術ではなく、C1〜C3 の主張を査読者に納得させるための補助的 contribution として扱う。

### どの section で説明するか

- Design Goals and Scope
- Implementation
- Discussion and Limitations
- Related Work

### 対応する evaluation question / experiment

- Q6: 実装責務は本当に少ないか？
- E7: Implementation responsibility table
- E0: Experimental setup and fairness, because simplicity and fairness must not be confused

### 先行研究との差分

- RocksDB は production-grade LSM engine として多機能・高設定性・運用上の複雑性を持つ。
- WiscKey / BlobDB は value-log GC と LSM index の両方を扱う。
- LeanStore / vmcache は explicit buffer management を持つ larger-than-memory system として位置づけられる。
- VMemKV は standalone KVS に scope を絞り、buffer pool や multi-level compaction を自前実装しない設計として位置づける。

### 過剰主張になりそうな点

避けるべき表現:

- 「コードが短いので優れている」
- 「実装が簡単なので性能も良い」
- 「責務が少ないので production-ready である」

安全な表現:

- 「VMemKV reduces the number of storage-engine responsibilities by delegating read-side value residency to the OS.」
- 「This simplicity comes with risks, which are evaluated and discussed.」
- 「The responsibility table is used to make the simplicity claim auditable.」

### repo で要確認

- LoC を出す場合は、対象範囲を明確にする。
- T1/T2/WAL/checkpoint/reorganization/benchmark の各 module の責務。
- RocksDB や baseline との比較表に載せる項目が実装実態と一致しているか。
- VMemKV が実際に持っている background jobs の数と種類。

---

## 最終的な Contribution 配置案

論文本文では、以下の順番で contribution を提示するのがよい。

1. OS-delegated larger-than-memory read-side value residency
2. T1/T2 responsibility split
3. Reorganization, in-process checkpoint, and recovery tailored to the T1/T2 split
4. Responsibility-based simplicity analysis

C1 と C2 が主 contribution である。C3 は system completeness を示す contribution、C4 は simplicity claim を客観化するための補助 contribution として扱う。

---

## Contribution paragraph draft in Japanese

本稿の contribution は以下の 4 点である。第一に、VMemKV は large value の read-side residency 管理を user-space buffer pool ではなく OS 仮想メモリ機構に委譲する larger-than-memory KVS 設計を提示する。第二に、RAM-resident な T1 index と mmap-backed な T2 value layer の責務分離により、hot metadata は engine が制御し、clean read-side value pages は OS が管理する構成を取る。第三に、VMemKV は LSM-style multi-level compaction ではなく、T1/T2 reorganization、in-process checkpoint / reload、WAL replay によって fragmentation repair と recovery を扱う。第四に、VMemKV の単純性を主観的なコード印象ではなく、storage engine が担う実装責務の比較表として整理する。
