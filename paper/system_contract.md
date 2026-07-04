# 論文執筆用 VMemKV System Contract

この文書は、論文本文で VMemKV を説明するときに参照すべき、正規化された
system contract である。現行 prototype と、論文が前提とする final-state
design を明確に分ける。

## 状態語彙

- **Implemented**: 現在のブランチの `implementation/include/`,
  `implementation/src/`, `implementation/tests/` に存在するもの。
- **TODO-Final**: まだ実装されていないが、`implementation/TODO.md` で
  明示的に要求されているため final-state system に含めるもの。
- **Out-of-Scope**: VMemKV system または論文 claim の対象外であるもの。
- **Unresolved**: 現行実装、設計文書、TODO のいずれでも未確定のもの。
  決定前に論文 claim にしてはならない。

主要な根拠: `implementation/README.md`, `implementation/TODO.md`,
`implementation/docs/specification/high_level_design.md`,
`implementation/docs/specification/low_level_design.md`,
`implementation/include/`, `implementation/src/`, `implementation/tests/`,
`paper/evalutation/evaluation_plan.md`, `paper/others/contributions.md`。

## 1. Scope（対象範囲）

VMemKV は、standalone な larger-than-memory key-value store として、次を
対象にする。

- `Get`, `Insert`, `Update`, `Delete`, ordered `Scan`。
- key metadata、lookup、scan control、T2 payload reference を管理する
  RAM-resident Tier 1 index。
- 可変長 key/value record のための file-backed `MAP_PRIVATE` mmap Tier 2
  value layer。
- T2 value page の read-side residency management を OS に委譲すること。
- T1/T2 reorganization による fragmentation repair。
- final-state における WAL、checkpoint、recovery による durability。
- ablation に用いる opt-in T1/T2 optimization: AppendMap, BloomFilter,
  SimdScan, MemoryHints, T1InlineValue。

VMemKV は、次を対象にしない。

- 複数操作をまとめた transaction。
- phantom avoidance。
- SQL、relational query processing、secondary index。
- replication、distributed consensus、backup protocol。
- VMemKV を KVS component として使う可能性を超えた LineairDB/Kamo 側の
  concurrency control。
- OS page cache や VM が、すべての database system において DBMS buffer
  pool を置き換えられるという一般主張。

根拠: HLD sections 2, 4, 5, 8; LLD sections 2, 3, 7; evaluation plan
sections 1, 3; contributions C1-C4。

## 2. Source of Truth（仕様根拠）

この contract は、current branch の working tree を source of truth として
扱う。統合対象は次である。

- `implementation/` にある現行 prototype。
- `implementation/TODO.md` にある final-state TODO。
- `paper/evalutation/evaluation_plan.md` と `paper/others/contributions.md`
  にある、論文向けの評価・contribution 制約。

`paper/obsolete_drafts/` は仕様根拠ではない。歴史的リスクや削除すべき古い
claim を確認するためにのみ使ってよい。古い draft の表現を、現在の system
behavior として論文に流用してはならない。

`implementation/docs/specification/*.md` と `implementation/TODO.md` が矛盾
する場合は、TODO を final-state の方向性として扱い、その矛盾は deprecated
または unresolved として記録する。

根拠: `paper/README.md`, `implementation/TODO.md`。

## 3. Final-State Assumption（最終状態の前提）

論文執筆では、final-state VMemKV system を「現行 prototype + 明示的な
TODO-final item」として扱う。

- WAL と crash recovery。
- T1/T2 checkpointing と WAL truncation。
- no-fork, in-process checkpointing。
- insert-heavy run で append-region exhaustion を避けるために十分な
  background reorganization、またはそれに相当する非手動 reorganization。

現行 prototype と final-state design は別物である。

| 項目 | 現行 prototype | Final-state system |
| --- | --- | --- |
| Write durability | WAL なし。recovery path なし。 | inline mutation を含むすべての write に対して WAL-before-update。 |
| T2 mapping | `MAP_PRIVATE | MAP_NORESERVE`、writable private pages。 | 同じ T2 mapping。T2 private dirty pages は durability path ではない。 |
| T2 update | `new_value_len <= alloc_len` なら in-place update。それ以外は append。 | 同じ logical update choice を WAL replay で保護する。 |
| Checkpoint | durable checkpoint としては未実装。 | stable T1 checkpoint と defragmented T2 checkpoint file。 |
| Recovery | 未実装。 | checkpoint を読み込み、WAL tail を replay し、active T1/T2 state を復元する。 |
| Checkpoint process model | HLD/LLD にはまだ `fork()` が書かれているが、コードは checkpoint-free。 | no-fork, thread-safe in-process background serialization/checkpointing。 |
| Reorganization | temp T2 file と memory swap による synchronous in-process T1/T2 rebuild。 | T1 reorganize は独立に実行できる。T2 reorganize は checkpoint/reload と generation switch に結び付く。 |
| Background execution | writer-triggered single-flight coordination は存在する。専用 background reorganization thread はない。 | background reorganize/checkpoint policy は TODO-final。 |

根拠: `implementation/TODO.md`; `src/vmemkv_impl.hpp`;
`src/core/reorganize_coordinator.hpp`; HLD section 6; LLD section 5。

## 4. Core Architecture（中核アーキテクチャ）

### Tier 1: RAM-Resident Index（RAM 常駐 index）

Tier 1 は hot な in-memory metadata layer である。実装済み index は次を持つ。

- 16-byte key prefix (`StoreKey`)。
- full key の 60-bit clean hash。`T1InlineValue` 使用時は上位 4 bit が
  inline metadata を encode しうる。
- T2 offset、inline value、または tombstone/not-found sentinel として解釈
  される 64-bit payload。
- sorted lookup/scan のための `sorted_region` と、recent write のための
  `append_region`。

sentinel は `STORE_NOT_FOUND == ~0ULL` である。設計文書では
`TOMBSTONE_PAYLOAD` または `TOMBSTONE_OFFSET` とも呼ばれている。論文本文で
は、コード名が必要な場合を除き、`tombstone/not-found sentinel` と呼ぶ。

実装済み T1 optimization:

- AppendMap: append-region slot に対する hash index。
- BloomFilter: sorted-region hash に対する negative lookup filter。
- SimdScan: サポートされる環境で append-region range filtering を SIMD で
  補助する。
- MemoryHints: 利用可能な場合に `mlock`, `MADV_HUGEPAGE`, sequential hint
  を使う。
- T1InlineValue: full key が 16 bytes 以下の場合、1-8 bytes の value を
  T1 に直接格納できる。

### Tier 2: `MAP_PRIVATE` mmap-Backed Value Layer（mmap-backed value layer）

Tier 2 は可変長 record を格納する。

```text
[ValueRecordHeader][key bytes][value bytes][padding]
```

現行 header は次である。

```text
key_len, value_len, alloc_len, flags, version
```

T2 は `MAP_PRIVATE | MAP_NORESERVE | PROT_READ | PROT_WRITE` で map される。
mapped T2 page への write は process-private であり、underlying file へ
writeback されない。read-side value residency は、clean file-backed page に
ついては OS VM/page-cache path に、private dirty page については通常の VM
management に委譲される。

T2 offset は mapped base からの byte offset である。live T2 record とは、
live T1 entry から到達可能な record である。T1 から到達不能な T2 record は
garbage であり、point delete ではなく reorganization/checkpoint によって
回収される。

### T1/T2 Reorganization（T1/T2 再編成）

T1 reorganization は `append_region` を `sorted_region` へ merge し、
tombstone を除去し、scan-friendly な ordering を回復する。T2
reorganization は、T1 から到達可能な live record のみを新しい T2 byte
array/file に copy し、T1 offset を書き換える。

final-state system では、T2 reorganization は checkpoint/reload の一部であ
る。現行 prototype では、`VMemKVImpl::reorganize()` が synchronous な
temp-file copy を実行し、temp file を private mapping し、T2 memory object
を swap し、rebuilt T1 を publish する。

根拠: HLD sections 3, 4, 6; LLD sections 2, 4, 5, 7;
`src/t1_index/t1_index.hpp`; `src/t2_flat_file/*`; `src/vmemkv_impl.hpp`。

## 5. Operation Semantics（操作 semantics）

### Get

1. user key を serialize する。
2. prefix と hash により、T1 の `append_region` と `sorted_region` を検索
   する。
3. payload が tombstone/not-found sentinel なら not found を返す。
4. T1InlineValue が entry を inline と mark している場合は、T1 payload を
   直接 decode する。
5. それ以外の場合は、T2 offset を解決し、格納されている full key と要求さ
   れた full key を比較する。
6. full-key match なら value を返し、それ以外は not found を返す。

### Insert

Final-state semantics:

1. duplicate live logical key を reject する。
2. T1/T2 を変更する前に insert WAL record を append し、永続化する。
3. T1InlineValue が適用可能なら、inline payload を T1 に書く。
4. それ以外の場合は、新しい T2 record を append し、その offset を T1
   `append_region` に書く。

現行 prototype との差分: WAL は存在しない。実装は T2 または T1 inline state
へ直接 write し、その後 T1 を update する。

### Update

Final-state semantics:

1. key が存在しない場合は false を返す。
2. T1/T2 を変更する前に update WAL record を append し、永続化する。
3. 既存 entry が inline で、新しい value も inline 可能な場合は、T1 inline
   payload を update する。
4. 既存 T2 record の `alloc_len` が十分大きい場合は、value を in-place
   update し、`value_len`/`version` を調整する。
5. value が収まらない場合は、active value-placement policy に従って新しい
   T2 record または inline payload を作り、その後 T1 を update する。
6. 古い T2 record は unreachable garbage になる。

現行 prototype との差分: in-place update と append update は存在するが、WAL
では保護されていない。非 inline の T2 entry について、prototype は placement
を切り替える前に T2 in-place update を試みるため、shrink 時の T2-to-inline
conversion は final paper claim ではない。

### Delete

Final-state semantics:

1. key が存在しない場合は false を返す。
2. T1 を変更する前に delete WAL record を append し、永続化する。
3. T1 payload に tombstone/not-found sentinel を書く。
4. foreground delete path では T2 record に触れない。

削除された T2 record は unreachable garbage になり、後で回収される。

### Scan

Scan は T1 を使って serialized-key order の candidate entry を特定し、
tombstone を除外し、新しい entry が古い entry を上書きするように deduplicate
し、必要に応じて T2 record を解決し、full-key range membership を検証する。
integer key は lexicographic scan のために big-endian order で serialize さ
れる。

Scan は multi-operation transaction ではない。実装は、optimistic sequence
check と retry によって、concurrent T1 reorganization 中に torn intermediate
state を観測しないことを目指す。

根拠: HLD section 5; LLD section 3; `src/api/*`; `src/vmemkv_impl.hpp`;
`src/t1_index/t1_index.hpp`; `tests/test_kv_store.cpp`。

## 6. Durability and Recovery Contract（永続化と recovery の契約）

Final-state durability は、T2 mmap writeback ではなく、WAL + checkpoint +
recovery によって提供される。

final-state で要求される rule:

- WAL-before-update: 対応する WAL record が durable になる前に、T1、T2、
  inline mutation を visible にしてはならない。
- inline mutation を含むすべての `insert`, `update`, `delete` operation は
  log しなければならない。
- commit policy は、後続の明示的な group-commit policy が同等の commit
  visibility を定義しない限り、client commit 前の WAL append + `fsync` で
  ある。
- Recovery は、最新の valid checkpoint を読み込み、checkpoint LSN 以降の WAL
  record を replay することで active T1/T2 state を再構成する。
- private T2 in-place update 中の crash は、WAL を replay して consistent な
  recovered state に戻すことで処理する。
- checkpoint が commit された後、checkpoint LSN より前の WAL は truncate し
  てよい。

明示的な non-rule:

- `MAP_PRIVATE` T2 dirty page は durable state ではない。
- `msync`, `MAP_SHARED`, T2 file writeback は VMemKV の durability path では
  ない。
- runtime T2 disk synchronization I/O は commit path の一部ではない。

未確定の durability detail:

- 正確な WAL record format、checksum、LSN encoding、replay idempotence。
- group commit が評価対象の final system に含まれるかどうか。
- checkpoint file の `fsync` policy。
- directory `fsync` policy。
- atomic rename と commit marker のどちらを使うか、または両方を使うか。
- incomplete checkpoint file をどう検出し、破棄するか。
- no-fork checkpoint における正確な replay start/end LSN rule。

これらの detail が実装・検証されるまで、crash consistency を claim してはな
らない。

根拠: `implementation/TODO.md`; HLD section 7; LLD sections 3.2-3.4, 5.3,
7.2; evaluation plan E0, E6。

## 7. Checkpoint / Reorganization Contract（checkpoint / reorganization の契約）

final-state checkpoint model は no-fork, in-process checkpointing である。

final-state で要求される behavior:

- `fork()` checkpointing は deprecated であり、current ではない。
- T1 checkpoint serialization と T2 defragmented checkpoint output は、
  thread-safe protocol により in-process で実行される。
- T2 checkpoint output は sequential に write され、その後
  `MAP_PRIVATE | MAP_NORESERVE` で reopen される。
- 最終的な generation switch のために、短い foreground pause は許容される。
- foreground pause 中は、新しい client operation を block する。
- WAL replay は、新世代を checkpoint LSN から switch に含まれる最新 durable
  LSN まで進める。
- T1-only reorganization は、full T2 reorganization/checkpoint より高頻度に
  実行してよい。
- T2 reorganization は offset を変更するため、T1 を consistent に update し
  なければならない。

現行 prototype behavior:

- `VMemKVImpl::reorganize()` は synchronous かつ checkpoint-free である。
- live record を `t2_flat.tmp` に write し、新しい file を private mapping
  し、T2 memory pointer を swap し、temp file を rename する。
- `VMemKV reorganize resolves storage fragmentation` test が示すように、
  unreachable T2 record を回収できる。
- restart recovery や durable checkpoint metadata は提供しない。
- `ReorganizeCoordinator` は writer-triggered single-flight soft/hard
  reorganization を提供するが、専用 background reorganization thread は存在
  しない。

checkpointing が pause-free であると claim してはならない。論文では、設計
が pause を generation switch + WAL replay に bound することを目指す、と主
張してよいが、実装後に実際の foreground cost を評価しなければならない。

根拠: `implementation/TODO.md`; HLD section 6; LLD sections 4, 5, 6;
`src/vmemkv_impl.hpp`; `src/core/reorganize_coordinator.hpp`;
`tests/test_kv_store.cpp`。

## 8. Implemented / TODO-Final / Out-of-Scope / Unresolved（状態別整理）

### Implemented（実装済み）

- `StoreAdapter` 経由の public C++ API: `insert`, `get`, `update`,
  `remove`, `scan`, `get_bytes`。
- integral key の big-endian serialization、および value の raw/little-endian
  serialization。
- sorted region と append region を持つ T1 two-region index。
- T1 prefix/hash lookup、tombstone sentinel、scan、deduplication、
  reorganization。
- T1 AppendMap, BloomFilter, SimdScan, MemoryHints, T1InlineValue variant。
- key/value bytes、allocation length、8-byte alignment を持つ T2 record
  layout。
- T2 append、および new value が `alloc_len` に収まる場合の in-place update。
- T2 `MAP_PRIVATE | MAP_NORESERVE` mapping。
- live T2 record を copy し、unreachable record を回収する synchronous
  T1/T2 reorganization。
- 基本的な thread-safety mechanism: `VMemKVImpl` の striped write lock、T1
  optimistic read/reorganization protocol、atomic append publication、map
  lifetime を守る T2 shared memory handle。
- CRUD、duplicate insert、delete/reinsert、scan、同じ prefix を共有する long
  key、large value、concurrent read、inline value、reorganization による
  space reclamation の correctness test。
- RocksDB 有効時の benchmark comparison 用 RocksDB adapter variant。

### TODO-Final（final-state に入る TODO）

- WAL class、WAL file format、durable append/flush、write pipeline
  integration。
- recovery parser と active T1/T2 state への replay。
- T1 checkpoint serialization (`t1_index.chk` または後継 format)。
- defragmented T2 checkpoint file generation。
- checkpoint LSN metadata と WAL truncation。
- no-fork in-process checkpointing protocol。
- insert-heavy workload が append-region limit に到達しないための background
  reorganization、または同等の scheduling/backpressure。
- `fork()` を current checkpoint design から外す LLD revision。

### Out-of-Scope（対象外）

- multi-operation transaction と phantom avoidance。
- SQL layer と relational query processing。
- replication、distributed recovery、consensus。
- 明示的に仕様化されるまでの secondary-index design。
- general-purpose DBMS buffer-pool replacement claim。
- central mechanism としての `mincore`。
- durability mechanism としての `MAP_SHARED`, `msync`, T2 file writeback。
- 完全網羅的な crash testing。現行 evaluation plan は recovery evaluation を
  sanity check として scope している。

### Unresolved（未解決）

- checkpoint commit protocol: atomic rename、commit marker、file `fsync`、
  directory `fsync`、incomplete-checkpoint handling。
- WAL format、checksum policy、LSN layout、replay idempotence、group commit。
- no-fork checkpoint generation switch における正確な foreground pause
  boundary。
- background reorganization scheduling、trigger threshold、checkpoint cadence
  との相互作用。
- capacity overflow behavior: LLD は checkpoint reload まで write を queue
  すると述べるが、現行 T2 code は capacity exhaustion で throw する。
- checkpoint/reorganization output 中に中断された場合の crash semantics。
- T1/T2 checkpoint file が、inline mode、optimization variant、capacity、
  hash metadata を十分に含むかどうか。
- `SIGBUS` / mmap I/O error handling。
- 同じ 16-byte prefix と同じ clean hash を共有する key に対する full-key hash
  collision semantics。
- 現行 `StoreKey` layout における SimdScan activation/effectiveness。
- temp path と persistent path に関する prototype file-lifecycle detail。
- durability-matched RocksDB configuration、および final evaluation における
  正確な VMemKV WAL/checkpoint sync setting。

## 9. Deprecated or Non-Current Claims（deprecated / 非 current な claim）

論文では、次を current VMemKV claim として提示してはならない。

- `fork()` checkpoint が current checkpoint mechanism である。
- `mincore` が VMemKV の central mechanism である。
- T2 durability が `MAP_SHARED`, `msync`, 通常の mmap file writeback から来
  る。
- `MAP_PRIVATE` dirty page が crash または restart 後も残る。
- OS page cache が一般に DBMS buffer pool を置き換える。
- VMemKV には GC/compaction-like work が存在しない。Reorganization は
  fragmentation repair mechanism である。
- Checkpoint/reorganization に foreground impact がない。
- 現行 prototype が crash-consistent である。
- VMemKV は常に RocksDB より高速である、またはすべての larger-than-memory
  KVS workload に対する一般解である。

歴史的表現として許されるのは、これらを rejected alternative、deprecated
design note、related-risk discussion として扱う場合だけである。

根拠: `paper/README.md`; `implementation/TODO.md`; HLD/LLD conflict;
contributions の overclaim note。

## 10. Allowed Paper Claims（論文で許される claim）

### Claims allowed now（現時点で許される claim）

- VMemKV は、larger-than-memory value layer の read-side value residency を
  OS virtual memory mechanism に委譲する standalone KVS design を検討する。
- VMemKV は、RAM-resident な key metadata と scan control を T1 に置き、
  mmap-backed な variable-length value を T2 に置くことで両者を分離する。
- VMemKV は、user-space buffer pool と LSM-style multi-level compaction を
  value layer の central mechanism にしない。
- VMemKV は、T1 ordering fragmentation と T2 storage fragmentation を修復す
  るために reorganization を使う。
- in-place update は、既存 T2 allocation に収まる value に対する実装済み
  optimization である。ただし、論文の central contribution ではない。
- final-state design では、durability は T2 mmap writeback ではなく WAL、
  checkpoint、recovery によって扱われる。
- VMemKV は、OS-delegated design が成立する条件と成立しない条件によって評価
  すべきであり、universal win を claim すべきではない。

### Claims to avoid（避けるべき claim）

- "mmap is always fast for DBMS workloads."
- "OS page cache fully replaces a DBMS buffer manager."
- "VMemKV eliminates compaction/GC cost."
- "VMemKV checkpointing has no pause or memory spike."
- "VMemKV is production-ready durable in the current prototype."
- "Key-value separation itself is novel."
- "T2 mmap access is near-memory speed under cold uniform larger-than-memory
  access."

### Claims that require evaluation（評価が必要な claim）

- controlled memory limit 下での larger-than-memory throughput と tail
  latency。
- simple mmap-only baseline を超える T1/T2 split の効果。
- matched memory and durability setting 下での RocksDB/BlobDB に対する競争力。
- reorganization が scan performance、reclaimed bytes、copied bytes、
  foreground tail latency に与える影響。
- WAL size と checkpoint age の関数としての checkpoint/recovery time。
- WAL/checkpoint/reorganization output と T2 private dirty bytes を分離した
  write amplification。
- LSM、value-log、Bitcask-like、LMDB、buffer-pool design と比較した
  implementation responsibility の削減。

根拠: evaluation plan sections 1, 4-10; contributions C1-C4。

## 11. Evaluation Gates（評価 gate）

### Possible with the current prototype（現行 prototype で可能）

- CRUD、scan、inline value、large value、reorganization transparency に関す
  る API correctness と regression testing。
- AppendMap, BloomFilter, SimdScan, MemoryHints, InlineValue に対する T1/T2
  design breakdown。
- non-durable prototype design point としての T2 `MAP_PRIVATE` mmap behavior。
- evaluation が durability/recovery を match していないことを明示する場合の
  larger-than-memory read-side behavior、page fault、thread scalability。
- prototype における storage-fragmentation repair としての reorganization:
  `bytes_used` before/after、copied live records、reclaimed unreachable
  records、scan before/after。
- raw value-access reference としての mmap-only microbaseline。durability を
  match した competitor ではない。

### Requires WAL/checkpoint/recovery first（先に WAL/checkpoint/recovery が必要）

- crash consistency と recovery claim。
- durability-matched RocksDB comparison。
- checkpoint foreground pause、generation switch time、WAL replay time、
  checkpoint memory high-water mark。
- WAL と checkpoint output を含む engine/device write amplification。
- WAL size/checkpoint age に対する recovery time。
- update、checkpoint、reorganization 中の crash。

### Durability-Matched RocksDB Comparison Preconditions（durability-matched RocksDB 比較の前提）

durability-matched RocksDB result を提示する前に、論文では次を明記しなけれ
ばならない。

- VMemKV WAL sync policy と checkpoint file sync policy。
- RocksDB WAL/sync policy、block cache size、compression、I/O mode。
- 共有された memory budget。可能なら cgroup または同等の total-memory
  control を使う。
- VMemKV T2 mapping mode: `MAP_PRIVATE`。
- 16 KiB value などの large-value comparison に RocksDB BlobDB を含めるかど
  うか。
- logical bytes、WAL bytes、engine-written bytes、device-written bytes、
  engine write amplification、device write amplification の定義。
- VMemKV T2 private dirty bytes を、T2 file writeback ではなく memory/private
  state として扱うこと。

根拠: evaluation plan E0-E7; `implementation/TODO.md`;
`paper/others/contributions.md`。
