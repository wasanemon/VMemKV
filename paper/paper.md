以下が、現時点の中心主張に合わせた **evaluation 抜きの章立て案**です。

中心主張はこれに固定します。

> **VMemKV is an in-place-first, two-tier key-value store over virtual memory. It keeps KV-specific metadata and update control in a compact mutable T1 index, while delegating larger-than-memory value residency to the OS through an mmap-backed T2 region.**

日本語では、

> **VMemKV は、virtual memory 上に構築された in-place-first な二層 KVS である。KV 固有の metadata と update control は compact / mutable な T1 index に保持し、larger-than-memory な value residency は mmap-backed T2 を通じて OS に委譲する。**

---

# Evaluation 抜きの章立て

## 0. Abstract

### 書くこと

VMemKV の問題設定、設計、貢献を短くまとめます。

Abstract では、以下の順番がよいです。

1. larger-than-memory KVS では、value residency / page replacement / compaction / update amplification が複雑になる。
2. LSM / value-log 系は update を append / new version に変換することで write path を単純化するが、compaction / GC / write amplification / tail latency を生む。
3. VMemKV は、in-place-first two-tier design を採る。
4. T1 は RAM-resident mutable index、T2 は mmap-backed value region。
5. T1/T2 により、DB 側は logical control を維持しつつ、value residency は OS に委譲する。
6. Reorganization は normal update path ではなく、ordering repair / garbage collection として使う。

### ここで主張すること

> VMemKV combines in-place-first updates with OS-delegated larger-than-memory value management.

### 注意点

Abstract では durability を強く言わない方がよいです。`pr-14` の TODO では crash recovery / MAP_PRIVATE durability inconsistency が未解決として明記されているため、現時点では “durable production engine” と主張しない方が安全です。

---

## 1. Introduction

### 1.1 Larger-than-memory KV stores are hard to keep simple

#### 書くこと

larger-than-memory KVS の難しさを導入します。

主に以下を問題として置きます。

* value が DRAM を超える。
* 明示的 buffer pool を持つと、page replacement / eviction / concurrency / prefetch / recovery が複雑になる。
* LSM-tree は write-friendly だが、update を append/new version として積む。
* value-log 系は value movement を減らすが、GC / pointer update / crash consistency が複雑になる。

ここでは、VMemKV が狙う問題を **“performance vs simplicity”** ではなく、より具体的に **“in-place update, LTM value residency, and implementation simplicity”** の三つ巴として置くのがよいです。

---

### 1.2 Existing designs force an update trade-off

#### 書くこと

既存設計の update path を整理します。

* LSM-tree:

  * update は新 version として append。
  * compaction が古い version を回収。
* Bitcask / WiscKey:

  * value は append log / value log に追記。
  * index は新 offset を指す。
  * 古い value は GC。
* B-tree / buffer-manager 系:

  * in-place update は可能だが、明示的 buffer manager や page lifecycle 管理が重い。
* mmap-only:

  * 実装は単純だが、DB 固有の ordering / fragmentation / update control が弱い。

ここで VMemKV の位置を出します。

> VMemKV asks whether a larger-than-memory KV store can keep update mutability without building a full user-space buffer manager or turning every update into an append.

---

### 1.3 VMemKV approach

#### 書くこと

VMemKV の解決策を提示します。

VMemKV は、repo の high-level design 上でも OS の仮想メモリ機構へ可能な限りデータ管理を委譲する larger-than-memory KVS と定義され、T1 は RAM 常駐 index、T2 は file-backed mmap の大容量データ層として設計されています。

Introduction では、以下の 3 点を明確に言います。

1. **T1 keeps logical control.**
   lookup, update, delete, scan order, offset, tombstone は T1 が管理する。

2. **T2 delegates physical residency.**
   value bytes は mmap-backed region に置き、page residency / eviction / fetch は OS に任せる。

3. **Updates are in-place-first.**
   T1 entry は既存 slot を直接更新し、T2 record も allocation が許す限り直接更新する。

---

### 1.4 Contributions

#### 書くこと

Contribution は 4 本にします。

#### Contribution 1: In-place-first two-tier architecture

VMemKV は、T1/T2 の二層構造により、update を基本的に append/new version に変換せず、既存 T1 entry と、可能なら既存 T2 record を更新する。

#### Contribution 2: OS-delegated larger-than-memory value layer

VMemKV は value residency を OS virtual memory / page cache に委譲する。一方で、KV 固有の logical control は T1 に残す。

#### Contribution 3: Reorganizing Two-Region index

VMemKV は `sorted_region` と `append_region` を持つ T1 を使い、insert locality と scan order を両立する。HLD では、この構造が VMemKV の中核概念として説明されています。

#### Contribution 4: Reorganization as deferred repair

VMemKV の reorganize は normal update path ではなく、ordering fragmentation と storage fragmentation を後から修復する仕組みである。

---

## 2. Background and Motivation

この章は、一般的な関連研究紹介ではなく、**VMemKV の設計判断を理解させるための背景**に絞ります。

---

### 2.1 Update-as-append in LSM and value-log systems

#### 書くこと

LSM / value-log 系の基本的な update model を説明します。

主張は次です。

> Modern KV stores often make writes fast by avoiding in-place mutation, but this shifts complexity to compaction, GC, and read-side version resolution.

ここで Bigtable / RocksDB / WiscKey / Bitcask を使います。

* Bigtable / RocksDB:

  * MemTable, SSTable, WAL, compaction。
* WiscKey:

  * key-value separation。
  * LSM には key と pointer、value は value log。
* Bitcask:

  * append-only data file + in-memory keydir。

VMemKV との差分は、

> VMemKV does not make append the default update representation. Append is for insert and value-growth fallback; stable-size updates can mutate existing records.

---

### 2.2 Buffer managers and the cost of explicit residency control

#### 書くこと

OS に任せない larger-than-memory DB の複雑性を説明します。

* user-space buffer pool
* page table / page id mapping
* replacement policy
* dirty page management
* prefetch
* concurrency
* pointer swizzling 系の複雑性

LeanStore / vmcache / Predictive Translation はこの文脈で触れます。

ここで VMemKV の対比を出します。

> VMemKV narrows the DB-managed state to T1 metadata and T2 record layout, and lets the OS manage physical residency of value pages.

---

### 2.3 Why mmap alone is not enough

#### 書くこと

mmap 批判への先回りです。

ここは重要です。VMemKV は mmap を使いますが、**mmap-only DBMS ではない**と説明します。

書くべきこと:

* mmap は page cache / virtual memory / zero-copy read の利点を持つ。
* しかし DBMS で mmap を使うと、page fault latency, writeback control, SIGBUS, durability, error handling が問題になる。
* VMemKV は「DB の全責務を mmap に丸投げする」のではなく、T1 で logical control を保持する。
* T2 は value residency のために mmap を使う。

ここでは強く言いすぎない方がよいです。`pr-14` では durability と mmap write path に設計不整合が残っているため、Discussion で改めて limitation として扱います。

---

## 3. VMemKV Overview

この章は Design の前に、全体像を置く章です。

---

### 3.1 Design goals

#### 書くこと

VMemKV の design goals を明示します。

候補は以下です。

1. **Larger-than-memory value capacity**
2. **In-place-first updates for existing keys**
3. **Append-friendly inserts**
4. **Ordered scans**
5. **Simple storage-engine implementation**
6. **Deferred reorganization instead of update-time compaction**

ここで「やらないこと」も書きます。

* distributed KV ではない
* transaction engine ではない
* full DBMS ではない
* 現時点では crash recovery を main claim にしない

HLD でも、VMemKV の features と limitations として、Get / Insert / Update / Delete / Scan は対象だが、複数操作をまとめた transaction や phantom 回避は対象外とされています。

---

### 3.2 Two-tier architecture

#### 書くこと

T1/T2 の役割分担を書きます。

| Tier | 役割                                                                                             |
| ---- | ---------------------------------------------------------------------------------------------- |
| T1   | RAM-resident mutable index。key prefix, hash, payload を持つ。payload は T2 offset または inline value。 |
| T2   | mmap-backed variable-length value record region。実 key/value bytes を保持する。                       |

HLD では、T1 は fixed-size index layer で、`IndexEntry` は key prefix, key hash, payload を持ち、payload は基本的に T2 offset とされています。 T2 は variable-size record を扱う file-backed mmap 上の byte array として定義されています。

---

### 3.3 Operation summary

#### 書くこと

Get / Insert / Update / Delete / Scan の流れを短くまとめます。

ここでは詳細説明ではなく、後続 Design のための鳥瞰図にします。

* Get:

  * T1 lookup
  * T2 offset resolve
  * full key check
* Insert:

  * T2 append
  * T1 append_region insert
* Update:

  * T1 lookup
  * T1 payload update
  * T2 in-place if allocation permits
  * otherwise T2 append + T1 offset swing
* Delete:

  * T1 tombstone
  * T2 physical deletion deferred
* Scan:

  * T1 range enumeration
  * T2 value fetch

HLD では update/delete/scan の流れがこの形で定義されています。

---

## 4. T1: Mutable Reorganizing Index

ここが論文の中心です。

---

### 4.1 Index entry layout

#### 書くこと

T1 の `IndexEntry` を説明します。

LLD では `IndexEntry` が `key_prefix`, `hash`, `payload_bits` を持つ構造として定義されています。

重要な説明:

* `key_prefix`:

  * ordering / range lookup に使う
* `hash`:

  * full-key disambiguation に使う
* `payload_bits`:

  * T2 offset
  * tombstone
  * inline value

この章のキモは、

> ordering fields are immutable, while payload bits are mutable.

です。

これにより、sorted index なのに update は並び替え不要になります。

---

### 4.2 Sorted region and append region

#### 書くこと

Reorganizing Two-Region を説明します。

* `sorted_region`:

  * key prefix 順
  * lookup / scan に強い
* `append_region`:

  * insert を受ける
  * unordered
  * append-friendly
* read は両方を見る
* update/delete は既存 entry を直接更新
* reorganize が append を sorted に吸収

HLD では、insert は append、read は両 region、update/delete は対象 entry が見つかれば基本的に in-place、reorganize は append を sorted に統合すると説明されています。

---

### 4.3 In-place payload mutation

#### 書くこと

T1 の in-place update を明確に主張します。

実装上、T1 `put()` は既存 slot が見つかると `slot.store(value)` で payload を更新し、見つからない場合のみ append slot を reserve / publish します。

この節の主張:

> T1 supports in-place mutation across the entire keyspace because both append and sorted entries expose mutable payload bits.

ここで注意すること:

* key 自体の update は対象外。
* key が変わる場合は delete + insert 扱い。
* in-place なのは payload / offset / tombstone の mutation。

---

### 4.4 Lookup and scan path

#### 書くこと

lookup と scan の設計を説明します。

* Get:

  * append_region
  * sorted_region
  * hash check
  * T2 full key check
* Scan:

  * T1 で range candidate を得る
  * T2 で full key/value resolve

ここでは append lookup index, Bloom filter, SIMD scan は opt-in optimization として軽く触れるだけでよいです。詳細は Implementation 章に回します。

---

### 4.5 T1 reorganization

#### 書くこと

T1 reorganize の役割を書きます。

* append_region の肥大化は lookup / scan の ordering fragmentation を生む。
* T1 reorganize は sorted + append を merge / sort / dedup し、append を空にする。
* tombstone entry を落とす。
* T2 とは独立に実行可能。

LLD では、T1 reorganize は sorted_region と append_region を読み、tombstone を除外し、key_prefix 順に sort し、新しい sorted_region を構築して append_region を空にすると説明されています。

---

## 5. T2: mmap-backed Value Region

この章は、VMemKV の OS-delegated LTM 管理の中心です。

---

### 5.1 T2 record layout

#### 書くこと

T2 の record layout を説明します。

LLD では `ValueRecordHeader` が key_len, value_len, alloc_len, flags, version を持ち、その後に key bytes と value bytes が続く layout として定義されています。

重要点:

* T2 は variable-length record layer。
* T1 payload offset から T2 record を参照する。
* full key は T2 record 内で確認する。
* `alloc_len` が in-place update 可否を決める。

---

### 5.2 OS-delegated larger-than-memory residency

#### 書くこと

T2 がなぜ mmap-backed なのかを書きます。

主張:

> T2 is a byte-addressable value region whose physical residency is managed by the OS.

書くべき点:

* DB 側は page replacement を実装しない。
* T2 record は offset で参照する。
* OS page cache / virtual memory が fault / eviction / fetch を担う。
* T1 は RAM 常駐なので、value が cold でも lookup metadata は hot に保てる。

ここでは mmap の利点だけでなく、「T1 があるから mmap を使っても KV の logical control を失わない」と書くべきです。

---

### 5.3 In-place value update and append fallback

#### 書くこと

T2 の update path を説明します。

LLD では、`new_value_len <= alloc_len` なら T2 record を in-place update し、それ以外なら T2 末尾に新 record を append して T1 offset を差し替えると定義されています。

実装上も `T2FlatFile::update_value_at()` は、value が `alloc_len` を超えない場合に mmap 上の value payload を `memcpy` し、`value_len` と `version` を更新しています。

この節では、次の表現を使うのが安全です。

> VMemKV is not physically in-place for every update. It is in-place-first: stable-size updates mutate existing records, while value-growth updates fall back to append-and-pointer-swing.

---

### 5.4 Delete and unreachable records

#### 書くこと

delete は T2 を即時変更しないことを説明します。

* T1 payload を tombstone にする。
* T2 record は unreachable になる。
* physical deletion は reorganize に遅延。

LLD では delete は T1 payload を tombstone にし、T2 record は触らず、後続 reorganize で物理削除されると定義されています。

ここでは、

> deletion is an in-place reachability update in T1.

と書くとよいです。

---

## 6. Reorganization: Repair, Ordering, and Garbage Collection

この章は、VMemKV が compaction ではなく reorganize をどう使うかを説明します。

---

### 6.1 Why reorganization is needed

#### 書くこと

fragmentation を 2 種類に分けます。

* Ordering fragmentation:

  * append_region が肥大化し、scan / lookup が悪化
* Storage fragmentation:

  * delete / value-growth update により T2 に unreachable record が残る

LLD でもこの 2 種類の fragmentation が定義されています。

---

### 6.2 T1 reorganization

#### 書くこと

T1 の repair を説明します。

* append を sorted に吸収
* tombstone 削除
* sorted scan locality 回復
* T2 とは独立に実行可能

これは 4.5 と少し重複しますが、この章では **system-level repair mechanism** として位置づけます。

---

### 6.3 T2 reorganization

#### 書くこと

T2 の garbage collection を説明します。

LLD では、T2 reorganize は T1 reorganize 後の sorted_region と append_region の live entries を入力にし、T2 live record だけを新 T2Store にコピーし、新 offset を T1 に反映すると定義されています。

主張:

> T2 reorganization follows T1 reachability.

つまり、T2 側が独自に liveness を判断するのではなく、T1 が live set を決める。

---

### 6.4 Reorganization versus LSM compaction

#### 書くこと

ここは Related Work ではなく、Design Discussion として書きます。

対比:

| LSM compaction                             | VMemKV reorganization                         |
| ------------------------------------------ | --------------------------------------------- |
| update visibility と version cleanup に深く関わる | update visibility は T1/T2 in-place path で即時反映 |
| multiple sorted runs を merge               | append/sorted T1 と T2 live records を repair   |
| normal write path の副作用として継続的に必要            | deferred repair / GC として必要時に実行                |
| value movement が大きくなりうる                    | stable-size update では value movement なし       |

ここは論文上かなり重要です。

---

## 7. Implementation

この章は、実装の詳細を全部説明するのではなく、**論文主張に必要な実装事実**だけに絞ります。

---

### 7.1 Code organization and public API

#### 書くこと

`VMemKVImpl`, `T1Index`, `T2FlatFile`, `StoreAdapter`, `Config` の関係を説明します。

`VMemKVImpl` は T1Index と T2FlatFile を束ね、get / insert / update / delete / scan / reorganize をルーティングする coordinator として実装されています。

---

### 7.2 Concurrency model

#### 書くこと

現実装でどこまで thread-safe かを説明します。

注意点:

* あまり強い concurrency 論文にしない。
* “sufficient implementation for evaluation” として説明する。
* T1 read path / reorganize / writer serialization を簡潔に書く。

実装コメントでは、T1Index は read operations が thread-safe、write operations は StoreImpl serialization に依存し、VMemKVStore は StoreImpl で thread-safe に調整されると説明されています。

---

### 7.3 T1 implementation details

#### 書くこと

論文主張に必要な T1 実装事実を書く。

* atomic payload bits
* append region
* sorted region
* append map
* Bloom filter
* SIMD scan
* memory hints
* seqlock / optimistic read around reorganize

T1 read path は `reorg_seq_` を使った optimistic retry protocol としてコメントされています。

---

### 7.4 T2 implementation details

#### 書くこと

T2 実装の要点を書く。

* file creation
* mmap
* append
* record resolution
* in-place update
* swap memory for reorganize

T2FlatFile は file を map し、record offset から `ValueRecordHeader` と key/value span を解決します。 append は `bytes_used` を atomic に進め、record header/key/value/padding を書きます。

---

### 7.5 Configuration and ablation support

#### 書くこと

Config tag による最適化切り替えを説明します。

`config.hpp` では AppendMap, BloomFilter, SimdScan, MemoryHints, InlineShort, Inline8B が tag として定義され、`Config<Tags...>` で切り替えられるようになっています。 また public header では、Baseline, cumulative variants, ablation variants, inline variants, RocksDB rival が `AllPossibleTypes` として列挙されています。

ここは evaluation への布石ですが、結果は書かない。

---

## 8. Discussion and Limitations

この章は必須です。VMemKV は野心的なので、弱点を先に制御した方がよいです。

---

### 8.1 What in-place-first does and does not mean

#### 書くこと

過剰主張を防ぎます。

明確に書くこと:

* すべての physical update が in-place ではない。
* T1 は既存 key の payload update が in-place。
* T2 は allocation-preserving update のみ in-place。
* value growth は append + pointer swing。
* insert は append。
* delete は T1 reachability update であり、T2 reclaim は deferred。

この節は査読対策として重要です。

---

### 8.2 Durability and recovery scope

#### 書くこと

現時点では durability を main claim にしないことを明記します。

`pr-14` TODO では、inline value recovery paradox、MAP_PRIVATE durability inconsistency、write path を mmap write ではなく write/fsync に寄せる設計変更が必要であることが書かれています。

この章では、

> This paper focuses on the architecture and update/storage layout. A full crash-consistent implementation requires finalizing the write/fsync recovery path.

のように書くのが安全です。

---

### 8.3 When OS-delegated LTM management helps

#### 書くこと

OS page cache / mmap の適用条件を慎重に述べます。

* hot set が RAM にある程度収まる
* storage latency が page fault cost と同程度か大きい
* long-running steady state
* cloud VM / slower SSD では有利になりやすい
* extremely fast NVMe / cold uniform access では不利になりうる

ここで既存 microbenchmark を “motivation” として軽く触れてもよいですが、evaluation section ではないので結果を詳述しない方がよいです。

---

### 8.4 Implementation simplicity versus control

#### 書くこと

実装容易性を貢献として整理します。

言い方:

> VMemKV simplifies the storage engine by reducing DB-side responsibility, not by eliminating storage management entirely.

VMemKV は以下を持たない:

* explicit buffer pool
* page replacement policy
* multi-level LSM compaction
* full value-log GC machinery

代わりに持つ:

* T1 mutable index
* T2 flat record layout
* reorganize
* OS paging hints

---

## 9. Related Work

Related Work は、最後に置く方がよいです。Introduction / Background で必要な比較は先に使い、Related Work では体系化します。

---

### 9.1 LSM-tree storage engines

#### 扱うもの

* Bigtable
* LevelDB
* RocksDB

#### 書くこと

* write-as-append
* immutable runs
* compaction
* read/write amplification
* scan support

VMemKV との差分:

> VMemKV preserves ordered access through T1 but does not make every update a new immutable version.

---

### 9.2 Key-value separation and value logs

#### 扱うもの

* WiscKey
* Bitcask

#### 書くこと

* key/index と value separation
* value log
* pointer update
* GC
* range scan の難しさ

VMemKV との差分:

> VMemKV resembles key-value separation, but its T1 is mutable and ordered, and its T2 permits allocation-preserving in-place updates rather than treating the value area as purely append-only.

---

### 9.3 Larger-than-memory engines and buffer managers

#### 扱うもの

* LeanStore
* FASTER
* F2
* vmcache
* Predictive Translation

#### 書くこと

* explicit buffer management
* hybrid log
* pointer swizzling
* virtual-memory-assisted buffer management
* larger-than-memory workload

VMemKV との差分:

> VMemKV is not a general-purpose buffer manager. It narrows the problem to KV metadata in T1 and value residency in T2.

---

### 9.4 mmap and OS-managed storage

#### 扱うもの

* mmap 批判系
* LMDB / mmap-based systems
* vmcache

#### 書くこと

* mmap の利点
* mmap の問題
* VMemKV の立場

VMemKV の立場:

> VMemKV does not claim mmap alone is sufficient for DBMS design. It uses mmap for T2 residency while retaining logical control in T1.

---

## 10. Conclusion

### 書くこと

短くまとめます。

含めるべき点:

1. VMemKV は in-place-first two-tier KVS。
2. T1 は mutable / reorganizing / RAM-resident。
3. T2 は mmap-backed larger-than-memory value region。
4. OS に value residency を委譲しつつ、KV logical control は DB 側に残す。
5. Reorganization は update の主経路ではなく、repair / GC。
6. 今後は evaluation と crash-consistent write/recovery path を完成させる。

Conclusion は過剰に強くしない方がよいです。

---

# 推奨する最終章構成

論文としては、以下の構成を推します。

```text
Abstract

1. Introduction
   1.1 Larger-than-memory KV stores are hard to keep simple
   1.2 Existing designs force an update trade-off
   1.3 VMemKV approach
   1.4 Contributions

2. Background and Motivation
   2.1 Update-as-append in LSM and value-log systems
   2.2 Buffer managers and explicit residency control
   2.3 Why mmap alone is not enough

3. VMemKV Overview
   3.1 Design goals and non-goals
   3.2 Two-tier architecture
   3.3 Operation summary

4. T1: Mutable Reorganizing Index
   4.1 Index entry layout
   4.2 Sorted region and append region
   4.3 In-place payload mutation
   4.4 Lookup and scan path
   4.5 T1 reorganization

5. T2: mmap-backed Value Region
   5.1 T2 record layout
   5.2 OS-delegated larger-than-memory residency
   5.3 In-place value update and append fallback
   5.4 Delete and unreachable records

6. Reorganization: Repair, Ordering, and Garbage Collection
   6.1 Why reorganization is needed
   6.2 T1 reorganization
   6.3 T2 reorganization
   6.4 Reorganization versus LSM compaction

7. Implementation
   7.1 Code organization and public API
   7.2 Concurrency model
   7.3 T1 implementation details
   7.4 T2 implementation details
   7.5 Configuration and ablation support

8. Discussion and Limitations
   8.1 What in-place-first does and does not mean
   8.2 Durability and recovery scope
   8.3 When OS-delegated LTM management helps
   8.4 Implementation simplicity versus control

9. Related Work
   9.1 LSM-tree storage engines
   9.2 Key-value separation and value logs
   9.3 Larger-than-memory engines and buffer managers
   9.4 mmap and OS-managed storage

10. Conclusion
```

---

# この章立てのポイント

この章立てでは、VMemKV を **mmap の論文**としてではなく、次のような論文として見せます。

> **in-place-first update を larger-than-memory KVS で成立させるために、mutable T1 index と OS-managed T2 value layer を分離した設計の論文**

これが一番強いです。

特に重要なのは、Design の中心を **T1 → T2 → Reorganization** の順に置くことです。

mmap を最初に出しすぎると mmap 批判に巻き込まれます。
in-place を最初に出し、T1/T2 の責務分離を説明し、その後で OS-delegated LTM management を出す方が、論文の主張が通りやすいです。
