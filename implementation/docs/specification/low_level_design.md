# VMemKV Low Level Design

## 1. Overview

本書は [high_level_design.md](./high_level_design.md) を受けて、VMemKV の low-level なデータレイアウト、操作手順、バックグラウンド処理、opt-in 最適化、および主要パラメータを定義する。

## 2. Core Data Structures

### 2.1 Tier 1

Tier 1 は fixed-size の `IndexEntry` を格納するインデックス層である。
`sorted_region` と `append_region` はどちらも `IndexEntry` 配列として保持する。

```c++
using StoreKeyPrefix = std::array<std::byte, 16>; // 16-byte key prefix

struct IndexEntry {
    StoreKeyPrefix key_prefix;  // primary sort key
    uint64_t hash;              // hash(full_key)
    uint64_t payload_bits;      // offset or inline 64-bit value
};

struct T1Region {
    IndexEntry* data;
    uint64_t size;
    uint64_t capacity;
};

struct T1Index {
    T1Region sorted_region;
    T1Region append_region;
    T1PayloadMode payload_mode;
};

constexpr uint64_t TOMBSTONE_PAYLOAD = UINT64_MAX;

enum class T1PayloadMode : uint8_t {
    Offset64,
    Inline64,
};
```

**Fields**

| Field | Meaning |
| --- | --- |
| `IndexEntry.key_prefix` | 主ソートキー。Tier 1 の並び順を決める。 |
| `IndexEntry.hash` | フルキーのハッシュ値。prefix だけでは区別できない候補の絞り込みに使う。 |
| `IndexEntry.payload_bits` | index 全体の `payload_mode` に応じて解釈される 64-bit payload。`UINT64_MAX` は tombstone。 |
| `sorted_region` | `key_prefix` 順にソート済みの `IndexEntry` 配列。 |
| `append_region` | 直近の insert を受ける未整列の `IndexEntry` 配列。 |
| `payload_mode` | `payload_bits` を `offset` とみなすか、inline 64-bit value とみなすかを決める index 単位の設定。 |

**Invariants**

- `sorted_region` は `key_prefix` 昇順である。
- `append_region` は未整列である。
- Tier 1 の live entry は、`sorted_region` と `append_region` を合わせて logical key ごとに高々 1 個であることを期待する。
- `payload_bits == TOMBSTONE_OFFSET` の entry は delete 済みであり、Get / Scan の結果に含めない。

### 2.1.1 Dynamic Inline Optimizations

Tier 1 は 64-bit payload を保持するインデックスである。T1/T2ハイブリッド構成において、値のサイズが 64-bit（8バイト）以下のときにディスク（Tier 2）へのアペンド書き込みをバイパスして、T1 の payload 領域内に直接バリューをインライン格納する「動的インライン最適化」がサポートされている。これによって、小さなサイズの値に対してディスクI/Oや mmap デリファレンスを完全に省略し、インメモリKVS並みの極限の検索速度を実現できる。

（動的インライン最適化の具体的な実装アプローチとそれぞれのトレードオフについては、後述の **「7.5 Dynamic T1 Inline Optimization」** を参照。）


### 2.2 Tier 2

Tier 2 は可変長 key/value record を格納する value 層である。
Tier 2 は file-backed mmap 上の単一 byte array として保持し、Tier 1 の `payload_bits` に格納された byte offset から record を参照する。

```c++
struct ValueRecordHeader {
    uint32_t key_len;
    uint32_t value_len;
    uint32_t alloc_len;     // allocated bytes for value payload
    uint32_t flags;         // reserved
    uint64_t version;       // optional: debug / validation use
};

// Layout on disk / mmap:
// [ValueRecordHeader][key bytes][value bytes][padding up to alloc_len]

struct T2Store {
    std::byte* base;
    uint64_t bytes_used;      // next append offset
    uint64_t bytes_capacity;  // mapped byte length
};
```

**Offset Space**

- `base` の先頭オフセットを `0` とする。
- `bytes_used` は次に追記する record の offset を表す。
- record offset は `0 <= offset < bytes_used` を満たす必要がある。
- record の開始 offset は `alignof(ValueRecordHeader)` 境界にアラインされる。
- record offset を参照する際は、`base + offset` を `ValueRecordHeader*` として解釈し、header に続く key bytes / value bytes を読む。

**Invariants**

- Tier 2 の live record は必ず Tier 1 のいずれかの live `IndexEntry.payload_bits` から到達可能である。
- Tier 1 から参照されていない Tier 2 record は garbage であり、`reorganize` で物理削除される。
- `update` は可能なら既存 record を in-place で上書きする。
- 新しい value が `alloc_len` を超える場合は `bytes_used` 位置に新 record を追加し、Tier 1 の `offset` を差し替える。
- 追記時の `bytes_used` の増分は、`ValueRecordHeader + key bytes + value bytes + padding（alloc_len まで）` に、次の record 開始位置を `alignof(ValueRecordHeader)` 境界に揃えるための調整を加えた合計で決まる。
- 常に `bytes_used <= bytes_capacity` を満たす。
- 追記要求で容量不足 (`bytes_used + required_bytes > bytes_capacity`) になった場合、その write request はキューに保持し、次回 checkpoint & reload で Tier 2 の再構築・拡張を待ってから書き込む。
- checkpoint & reload では Tier 2 がデフラグされて空き容量が増える場合があり、キューされた write request の実際の書き込み offset は enqueue 時点の `bytes_used` と一致するとは限らない。

### 2.3 VMemKV State

```c++
struct VMemKV {
    T1Index t1;
    T2Store t2;
    Wal wal;
};
```

本設計では、checkpoint reload 後の新世代も同じ `T1Index` / `T2Store` 形に再構築される。

## 3. Read / Write Operations

本節の手順は、特に断りがなければ `payload_mode == Offset64` を前提に記述する。`payload_mode == Inline64` の index では Tier 2 に関する手順を省略し、Tier 1 の `payload_bits` を直接読み書きする。

### 3.1 Get

入力は full key である。Tier 1 では `key_prefix` と `hash` で候補を絞り、Tier 2 で full key 一致を確認する。

**Procedure**

1. `key_prefix = prefix(full_key)` と `hash = hash(full_key)` を計算する。
2. `t1.append_region` に紐づくハッシュインデックス（`append_index`）を検索し、`key_prefix` および `hash` が一致する候補を探す。
3. append 側で見つからなければ、`t1.sorted_region` を `key_prefix` で二分探索し、候補 range を得る。
4. 候補 `IndexEntry` について `hash` を照合する。
5. `payload_bits == TOMBSTONE_OFFSET` なら not found。
6. `t2.at(payload_bits)` で Tier 2 record を取得する。
7. Tier 2 record の full key を比較し、一致すれば value を返す。違えば not found。

**Complexity**

- `O(1) + O(log |t1.sorted_region|)` expected

### 3.2 Insert

**Procedure**

1. `Get(full_key)` を実行し、既存 entry がなければ続行する。
2. WAL に insert record を append し、`fsync` する。
3. Tier 2 の `bytes_used` 位置に新しい `ValueRecord` を書き込み、論理 `offset` を得る。
4. `IndexEntry{key_prefix, hash, payload_bits}` を作り、Tier 1 `append_region` に追加する。

**Failure Rule**

- WAL 永続化前に Tier 1 / Tier 2 を更新してはならない。

### 3.3 Update

**Procedure**

1. `Get(full_key)` で対象 entry を特定する。見つからなければ not found。
2. WAL に update record を append し、`fsync` する。
3. Tier 2 の既存 record を見る。
4. `new_value_len <= alloc_len` なら Tier 2 record を in-place update する。
5. それ以外なら Tier 2 の `bytes_used` 位置に新 record を追加し、新しい `offset` を得る。
6. Tier 1 の該当 `IndexEntry.payload_bits` を新しい `offset` に書き換える。

**Notes**

- old Tier 2 record はその場では削除しない。
- old Tier 2 record は Tier 1 から到達不能になり、後続の `reorganize` で物理削除される。

### 3.4 Delete

**Procedure**

1. `Get(full_key)` で対象 entry を特定する。見つからなければ not found。
2. WAL に delete record を append し、`fsync` する。
3. Tier 1 の該当 `IndexEntry.payload_bits` を `TOMBSTONE_OFFSET` に書き換える。

**Notes**

- Tier 2 の record は delete 時には触らない。
- delete 済み record は Tier 1 から到達不能になり、`reorganize` で物理削除される。

### 3.5 Scan

**Procedure**

1. Tier 1 `append_region` の全体を走査し、範囲内の `IndexEntry` を候補に集める。
2. Tier 1 `sorted_region` を `key_prefix` で二分探索し、範囲内候補を列挙する。
3. 候補ごとに `payload_bits != TOMBSTONE_OFFSET` を確認する。
4. `payload_bits` から Tier 2 record を取得する。
5. full key を比較し、範囲内の record だけを返す。

**Notes**

- `key_prefix` はあくまで coarse filter である。
- full key 比較は Tier 2 で行う。

## 4. Reorganize

### 4.1 Purpose

`reorganize` は次の 2 種類の断片化を解消する。

- Ordering Fragmentation:
  Tier 1 `append_region` の肥大化により候補探索・確認コストが増え、Get / Scan が遅くなる。加えて、Tier 2 の肥大化や局所性低下、append-update による参照先の散在により、主に Scan が遅くなり、Get でもページフォルト増加などで間接的に悪化しうる。
- Storage Fragmentation:
  delete や append-update の結果、Tier 1 から参照されない古い Tier 2 record が蓄積する。

### 4.2 T1 Reorganize

T1 `reorganize` は T2 と独立に実行できる。

**Input**

- 現在の `t1.sorted_region`
- 現在の `t1.append_region`

**Output**

- 新しい `t1.sorted_region`
- 空の `t1.append_region`

**Procedure**

1. `sorted_region` と `append_region` の全 `IndexEntry` を読み出す。
2. `payload_bits == TOMBSTONE_OFFSET` の entry を除外する。
3. `key_prefix` 順にソートする。
4. 必要なら重複 key を解消する。
5. 新しい `sorted_region` を構築する。
6. `append_region` を空にする。
7. `append_region` に紐づくハッシュインデックス（`append_index`）を完全にクリア（空に初期化）する。

**Effect**

- Ordering Fragmentation を解消する。
- delete 済み entry を Tier 1 から取り除く。
- `payload_mode == Inline64` の index では、この処理だけで完結する。

### 4.3 T2 Reorganize

T2 `reorganize` は Tier 1 の offset を更新する必要があるため、`payload_mode == Offset64` の index に対してのみ T1 `reorganize` とセットで実行する。

**Input**

- T1 の `reorganize` 後 `sorted_region`
- T1 の現 `append_region`
- 現在の Tier 2 全 record

**Output**

- 新しい `T2Store`
- 新しい offset を反映した Tier 1

**Procedure**

1. T1 `sorted_region` の順に各 live `IndexEntry` を走査する。
2. 各 `IndexEntry.payload_bits` から Tier 2 record を取得し、新しい `T2Store` の `bytes_used` 位置にコピーする。
3. コピー先 offset を計算し、対応する T1 `IndexEntry.payload_bits` を新 offset に書き換える。
4. 次に T1 `append_region` の各 live `IndexEntry` について同様に Tier 2 record をコピーする。
5. Tombstone 化されている entry と、Tier 1 から参照されない Tier 2 record はコピーしない。

**Effect**

- Tier 2 の Storage Fragmentation を解消する。
- Tier 1 の offset を新世代の Tier 2 へ張り直す。

## 5. Checkpoint Reload

### 5.1 Motivation

T2 `reorganize` は full scan + full copy を伴うため、in-memory で同時に旧世代と新世代を持つとメモリ圧迫が大きい。
そのため、T2 `reorganize` は必ず checkpoint file を介して行い、完成したファイルを `mmap` で読み込む。


> [!NOTE]
> 新世代の T2 ファイル構築時、`mmap(MAP_SHARED)` でマッピングしてアペンド追記する設計ではなく、通常の `write()` システムコールを用いたシーケンシャル書き出しを採用します。
> ファイルを拡張しながら `mmap` でアペンド書き込みを行うと、ページ境界を越えるたびにカーネル側でマイナーページフォルト（Page Fault）が発生し、メモリ割り当てとページテーブル更新による CPU サイクル浪費が発生します。
> これに対し、通常の `write()` はカーネルページキャッシュのバッファリングが高度に効き、ページフォルトを伴わずにシーケンシャルデータを高速に流し込めます。また、一時的な mmap 状態を管理する必要がなくなるため、リソース管理のバグが排除され堅牢性が向上します。書き出し完了した新ファイルを、最後に `MAP_PRIVATE | MAP_NORESERVE` で再オープン（mmap）します。

> [!NOTE]
> T2 の mmap には `MAP_PRIVATE | MAP_NORESERVE | PROT_READ | PROT_WRITE` を使用します。
> `MAP_PRIVATE` により書き込みはプロセス内の COW ページに留まり、ファイルには反映されません（再起動時揮発）。
> `MAP_NORESERVE` により、カーネルが mmap 時点でスワップ領域を一括予約するのを回避します。これにより、物理 RAM を大幅に超える仮想アドレス空間（例: 64 GB）を確保しても ENOMEM が発生しません。物理ページはアクセス時にオンデマンドで割り当てられ、通常通りスワップアウトされます。
> この設計により、mprotect による書き込みページ保護の往復（RO → RW → RO）が不要になり、書き込みパスが大幅に簡潔かつ高速になります。


### 5.2 Flow

`checkpoint reload` は次の手順で行う。

1. 新しい checkpoint file を T1, T2 用に作成する。
2. `fork` で CoW スナップショットを作る。
3. child 側で `fork` 直後の snapshot LSN を記録する。
4. child 側で T1 `reorganize` を実行し、新しい T1 in-memory state を作る。
5. child 側で T2 `reorganize` を実行し、新しい T2 checkpoint file を構築する。
6. T2 の新 offset を反映済みの T1 を T1 checkpoint file に書き出す。
7. checkpoint file 完成後、親側で短時間 stop-the-world に入る。
8. 親側で新 checkpoint file を `mmap(MAP_PRIVATE | MAP_NORESERVE)` して、新しい T1 / T2 を作る。
  T2 は `PROT_READ | PROT_WRITE` で mmap し、書き込みパスに mprotect は使用しない。
9. T1 file は `mlock` する。
10. child 側 snapshot LSN の次の WAL record から、親側で stop-the-world 中に確定した最新 LSN までを replay する。
11. replay 結果を新しい T1 / T2 に反映する。
12. 新世代へ切り替える。
13. stop-the-world を解除する。
14. 不要になった WAL と旧 checkpoint を削除する。

**Hash Index Lifecycle in Checkpoint**

- `reorganize` 直後は `append_region` が空 (size = 0) となるため、T1 checkpoint file には `sorted_region` のデータのみがシーケンシャルに書き出され、`append_index` の状態自体はファイルへシリアライズ（永続化）しない。
- `reload` 時、親プロセス側で新しくマッピングされた T1 インスタンスは、`append_index` を空に初期化した状態で起動し、その後の WAL リプレイおよび新規クライアント書き込み時に適宜ハッシュインデックスへの登録・更新を行う。

### 5.3 Correctness Rule

- WAL replay は snapshot LSN の次の record から始める。
- replay の終点は stop-the-world 中に親側で確定した最新 LSN である。
- child 側 snapshot に含まれている更新を replay してはならない。
- stop-the-world 開始後に新たなクライアント操作を入れてはならない。

## 6. Concurrency Contract

### 6.1 Base Assumption

point operation は通常時にオンラインで進める。
一方、checkpoint reload の最終切り替えだけは短時間の stop-the-world を許容する。

この節では具体的な同期機構そのものではなく、各操作がどの操作と競合し、競合時に何を保証すべきかを定義する。

### 6.2 Required Guarantees

- Get / Scan は通常時にオンラインで実行できる。
- Insert / Update / Delete は WAL 永続化後に T1 / T2 を更新する。
- T1-only `reorganize` は T2 と独立して高頻度に走らせてよい。
- T2 `reorganize` は checkpoint reload の一部としてのみ実行する。
- checkpoint reload の最終段階だけは全クライアント操作を止める。
- stop-the-world 開始前に開始された操作については、retry・snapshot・serialization のいずれかで整合を保つ。

### 6.3 Operation Concurrency Matrix

以下の表は、各操作の並行実行可否と必要な扱いを定義する。

- `Allowed`: 特別な停止なしに並行実行してよい
- `Allowed with retry/snapshot`: 並行実行してよいが、snapshot 読みまたは retry が必要
- `Serialized`: 同一 key に対しては直列化が必要
- `Blocked in final STW`: checkpoint reload の最終 stop-the-world では停止する
- `Single-flight`: 並行呼び出し時、実行者（leader）を 1 スレッドだけ選出する。follower の挙動は経路で異なり、soft 経路では待機せず継続、hard 経路では leader 完了まで待機する（アトミックフラグで制御）。

| Operation A / B | Get | Scan | Insert | Update/Delete | T1 reorganize | Checkpoint reload |
| --- | --- | --- | --- | --- | --- | --- |
| `Get` | Allowed | Allowed | Allowed | Allowed | Allowed with retry/snapshot | Blocked in final STW |
| `Scan` | Allowed | Allowed | Allowed | Allowed | Allowed with retry/snapshot | Blocked in final STW |
| `Insert` (distinct key) | Allowed | Allowed | Allowed | Allowed | Allowed with retry/snapshot | Blocked in final STW |
| `Insert` (same key) | Allowed | Allowed | Serialized | Serialized | Allowed with retry/snapshot | Blocked in final STW |
| `Update/Delete` (distinct key) | Allowed | Allowed | Allowed | Allowed | Allowed with retry/snapshot | Blocked in final STW |
| `Update/Delete` (same key) | Allowed | Allowed | Serialized | Serialized | Allowed with retry/snapshot | Blocked in final STW |
| `T1 reorganize` | Allowed with retry/snapshot | Allowed with retry/snapshot | Allowed with retry/snapshot | Allowed with retry/snapshot | Single-flight | Blocked in final STW |
| `Checkpoint reload` | Blocked in final STW | Blocked in final STW | Blocked in final STW | Blocked in final STW | Blocked in final STW | Single-flight |

### 6.4 Conflict Resolution Rules

#### Point Operations vs Point Operations

- 同一 key に対する Insert / Update / Delete は直列化しなければならない。
- 異なる key に対する point operation は独立に進めてよい。
- Get / Scan は進行中の Insert / Update / Delete と競合してもよいが、観測結果は「旧状態」または「新状態」のいずれかでなければならず、破損した中間状態を見てはならない。

#### T1 Reorganize vs Readers

- Get / Scan は T1 `reorganize` と並行してよい。
- ただし、reader は
  - 旧 `sorted_region` / `append_region` の snapshot を読み切る
  - または切り替えを検出して retry する
  のどちらかで整合を保たなければならない。

#### T1 Reorganize vs Writers

- Insert / Update / Delete は T1 `reorganize` と並行してよい。
- ただし、writer は
  - 旧世代に対して成功し、その後 merge 境界以降の差分として扱われる
  - または切り替えを検出して retry する
  のどちらかで、更新を失わないことが必要である。

#### Checkpoint Reload vs All Operations

- child 側での `reorganize` / checkpoint file 構築中は、親側の Get / Scan / Insert / Update / Delete を継続してよい。
- 最終段階では stop-the-world に入り、新しい操作開始を止める。
- stop-the-world 中に、新世代の T1 / T2 を `mmap` し、WAL replay により snapshot 以降の差分を反映する。
- 新世代への切り替えが完了するまでは、クライアントへ新世代を公開してはならない。

### 6.5 Implementation Candidates

本書は mutex-free 実装を必須とはしない。
上記の並行性契約を満たすなら、次のいずれも許容する。

- coarse-grained lock
- per-region lock
- per-entry seqlock
- pointer swap + epoch based reclamation
- reader snapshot + writer retry

`seqlock` は有力な実装候補だが、設計仕様として必須ではない。
low-level design が要求するのは同期機構の名前ではなく、6.2 から 6.4 に記した concurrency contract である。

## 7. Opt-in Optimizations

本節の最適化はすべて opt-in であり、無効でも正しく動作する。

### 7.1 Tier 1 Memory Hints

- `mlock(t1, size)`:
  Tier 1 を物理 RAM に固定し、Tier 2 圧迫下でもスワップアウトを防ぐ。
- `madvise(t1, size, MADV_HUGEPAGE)`:
  TLB 圧力を下げる。
- `MADV_SEQUENTIAL`:
  T1 full scan の直前に適用し、先読みを促す。

### 7.2 Group Commit / Early Lock Release / Flush Pipelining

詳細は [Aether](https://dl.acm.org/doi/10.14778/1920841.1920928) を参照。

- WAL をグループで `fsync` する。
- エントリロック解放を WAL flush 完了前に前倒しする。
- 読み取りも waiter を介して未 flush データの整合を取る。

### 7.3 SIMD Tier 1 Scan

`IndexEntry` は fixed-size かつ連続配置なので、Tier 1 の append scan や range scan は SIMD 最最適化しやすい。

### 7.4 Index-Level Covering

index 単位で `payload_mode == Inline64` を選び、Tier 1 payload を 64-bit value として解釈する。

- small fixed-size value workload では Tier 2 アクセスを完全に省略できる
- WAL / checkpoint では index の payload mode をメタデータとして永続化する

### 7.5 Entry-Level Adaptive Covering (Dynamic T1 Inline Optimization)

エントリーごとに T2 オフセットとインライン 64-bit 値を動的に切り替える最適化である。値が 8バイト（64ビット）以下のときに Tier 2 への書き出しをバイパスして Tier 1 インデックスの payload 領域内に直接バリューをインライン格納する。

本最適化は `vmemkv::Config` のテンプレート引数タグ（`T1InlineValue`）を介して制御される。

#### 7.5.1 メタデータとハッシュのエンコーディング
T1のインデックススロットに十分な空きビット領域がないため、64ビットのハッシュフィールド（`hash`）の最上位4ビットをメタデータ領域として再利用し、フラグとサイズ情報を格納する。

- **Bit 63 (`is_inline`)**: `1` の場合はインラインデータ、`0` の場合は T2オフセットを表す。
- **Bits 62-60 (`inline_size`)**: インラインデータのバイトサイズ（1〜8バイト）を表す。サイズ `8` は `0` としてエンコードされる。
- **Bits 59-0 (`clean_hash`)**: 実際の60ビットFnvハッシュキー。インデックスの検索、Bloom filterの登録・判定、SIMDスキャン等のハッシュ比較時には、上位4ビットをマスクしてこの60ビット部分のみを比較する。

#### 7.5.2 インライン化の動作
- **T2 オフセットとの識別 (判定)**:
  - 読み出し時、T1から取得したスロットハッシュの最上位ビット（Bit 63）を確認するだけで、T2をフェッチせずにインラインかオフセットかを100%確実に識別できる。
- **値が 1〜8 バイトの場合**:
  - `payload_bits` に対するビットシフトやビットの埋め込みは行わず、64ビットのビットパターンをそのまま無加工で格納し、デコード時は `inline_size` に従いバイトコピーを行う。これにより、`double`、`time`、連番のサロゲートキー（偶数・奇数を問わず）など、あらゆる64ビット以内のデータ型を完全にインライン化できる。

### 7.6 Sorted Bloom Filter

`sorted_region` 全体に Bloom filter を付与し、negative lookup を高速化できる。

### 7.7 Tier 2 Prefetch

Scan 時に Tier 1 から得た offset 群に対して `madvise(MADV_WILLNEED)` を出し、Tier 2 を先読みする。

## 8. Parameters

| Parameter | Meaning |
| --- | --- |
| `T1_MAX_INDEX_SIZE` | Tier 1 最大 entry 数 |
| `T1_REORGANIZE_THRESHOLD` | T1 `append_region` が肥大化したときの reorganize 閾値 |
| `T2_MAX_VIRTUAL_MEMORY_SIZE` | Tier 2 最大仮想アドレス空間 |
| `T2_CHECKPOINT_INTERVAL_SEC` | checkpoint reload 実行周期 |
| `T2_STORAGE_FRAGMENTATION_THRESHOLD` | Tier 2 storage fragmentation の閾値 |
| `T2_ORDERING_FRAGMENTATION_THRESHOLD` | Tier 2 ordering fragmentation の閾値 |
| `WAL_SYNC_MODE` | WAL `fsync` ポリシー |
