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
    uint64_t payload_bits;      // Tier 2 offset, or an entry-level inlined value (2.1.1 節)
};

struct T1Region {
    IndexEntry* data;
    uint64_t size;
    uint64_t capacity;
};

struct T1Index {
    T1Region sorted_region;
    T1Region append_region;
};

constexpr uint64_t TOMBSTONE_PAYLOAD = UINT64_MAX;
```

**Fields**

| Field | Meaning |
| --- | --- |
| `IndexEntry.key_prefix` | 主ソートキー。Tier 1 の並び順を決める。 |
| `IndexEntry.hash` | フルキーのハッシュ値。prefix だけでは区別できない候補の絞り込みに使う。 |
| `IndexEntry.payload_bits` | 通常は Tier 2 の byte offset。entry 単位でインライン化されている場合は値そのもの(2.1.1 節)。`UINT64_MAX` は tombstone。 |
| `sorted_region` | `key_prefix` 順にソート済みの `IndexEntry` 配列。 |
| `append_region` | 直近の insert を受ける未整列の `IndexEntry` 配列。 |

**Invariants**

- `sorted_region` は `key_prefix` 昇順である。
- `append_region` は未整列である。
- Tier 1 の live entry は、`sorted_region` と `append_region` を合わせて logical key ごとに高々 1 個であることを期待する。
- `payload_bits == TOMBSTONE_OFFSET` の entry は delete 済みであり、Get / Scan の結果に含めない。

### 2.1.1 Dynamic Inline Optimizations

Tier 1 は 64-bit payload を保持するインデックスである。T1/T2ハイブリッド構成において、値のサイズが 64-bit（8バイト）以下のときにディスク（Tier 2）へのアペンド書き込みをバイパスして、T1 の payload 領域内に直接バリューをインライン格納する「動的インライン最適化」がサポートされている。これによって、小さなサイズの値に対してディスクI/Oや mmap デリファレンスを完全に省略し、インメモリKVS並みの極限の検索速度を実現できる。

（動的インライン最適化の具体的な実装アプローチとそれぞれのトレードオフについては、後述の **「7.3 Entry-Level Adaptive Covering (Dynamic T1 Inline Optimization)」** を参照。）


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

**Payload Bit Layout**

Tier 1 の `payload_bits` が Tier 2 offset を表す場合(2.1 節)、T2 へのランダムアクセスなしで $O(1)$ に `T1_Live_Bytes` を集計できるよう、未使用の上位ビットにレコードサイズを埋め込む。opt-in ではなく、offset payload では常時この形式を用いる。

* **Bits 0-47 (48-bit):** `T2 Offset`（物理ファイル内のレコードのオフセットポインタ。最大 256 TiB の仮想アドレス空間をマップ可能）。
* **Bits 48-63 (16-bit):** `Embedded Block Count`（16バイトアライメントされたレコード占有サイズブロック数 `aligned_len / 16`）。最大表現サイズ: $(2^{16} - 1) \times 16 \text{ バイト} \approx \mathbf{1.04 \text{ MB}}$。

デコード: 物理オフセットは `payload_bits & 0xFFFFFFFFFFFFULL`、レコードサイズは `(payload_bits >> 48) << 4`。

**Invariants**

- Tier 2 の live record は必ず Tier 1 のいずれかの live `IndexEntry.payload_bits` から到達可能である。
- Tier 1 から参照されていない Tier 2 record は garbage であり、`reorganize` で物理削除される。
- `update` は可能なら既存 record を in-place で上書きする。
- 新しい value が `alloc_len` を超える場合は `bytes_used` 位置に新 record を追加し、Tier 1 の `offset` を差し替える。
- 追記時の `bytes_used` の増分は、`ValueRecordHeader + key bytes + value bytes + padding（alloc_len まで）` に、次の record 開始位置を `alignof(ValueRecordHeader)` 境界に揃えるための調整を加えた合計で決まる。
- 常に `bytes_used <= bytes_capacity` を満たす。
- 追記要求で容量不足 (`bytes_used + required_bytes > bytes_capacity`) になった場合、その write request は失敗として扱う(キューイングや自動リトライは行わない)。
- reorganize(4 章)は delete 済み・参照が切れた record を除去して Tier 2 を defrag できるが、これは容量不足への自動対応ではなく、checkpoint & reload とは独立した既存の機構である。

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

本節の手順は、特に断りがなければ `payload_bits` が Tier 2 offset である通常の entry を前提に記述する。entry 単位でインライン化されている entry(2.1.1 節)では Tier 2 に関する手順を省略し、Tier 1 の `payload_bits` を直接読み書きする。

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
2. Tier 2 の `bytes_used` 位置に新しい `ValueRecord` を書き込み、論理 `offset` を得る。
3. `IndexEntry{key_prefix, hash, payload_bits}` を作り、Tier 1 `append_region` に追加する。
4. WAL に insert record を append し、`fsync` する。呼び出し元への成功応答はこの `fsync` 完了後にのみ返す。

**Failure Rule**

- WAL append は、Tier 1 / Tier 2 への適用(2.〜3.)が成功したことが確定した後にのみ行う。例外・失敗で完了しなかった操作を WAL に記録してはならない。
- 理由: Tier 1 / Tier 2 は volatile であり (5.1 節)、それ自体がディスクへ書き戻される経路を持たないため、古典的 WAL が要求する「ログ永続化 ≺ データページのディスクへの書き戻し」という制約はここでは構造的に自明に満たされる。したがって WAL を先に書く必然性はなく、むしろ適用失敗時に WAL へ記録が残ると、次回起動時の replay で同じ失敗が再現し続け、リカバリ自体が永久に失敗する("poison pill")リスクがある。
- 2. で容量不足 (`bytes_used + required_bytes > bytes_capacity`) となった場合、この操作は失敗として扱い、WAL には何も記録しない(2.2 節)。

### 3.3 Update

**Procedure**

1. `Get(full_key)` で対象 entry を特定する。見つからなければ not found。
2. Tier 2 の既存 record を見る。
3. `new_value_len <= alloc_len` なら Tier 2 record を in-place update する。
4. それ以外なら Tier 2 の `bytes_used` 位置に新 record を追加し、新しい `offset` を得たうえで、Tier 1 の該当 `IndexEntry.payload_bits` を書き換える。
5. WAL に update record を append し、`fsync` する。呼び出し元への成功応答はこの `fsync` 完了後にのみ返す。

**Notes**

- old Tier 2 record はその場では削除しない。
- old Tier 2 record は Tier 1 から到達不能になり、後続の `reorganize` で物理削除される。
- Failure Rule は 3.2 節と同様: 2.〜4. が失敗した操作を WAL に記録してはならない。

### 3.4 Delete

**Procedure**

1. `Get(full_key)` で対象 entry を特定する。見つからなければ not found。
2. Tier 1 の該当 `IndexEntry.payload_bits` を `TOMBSTONE_OFFSET` に書き換える。
3. WAL に delete record を append し、`fsync` する。呼び出し元への成功応答はこの `fsync` 完了後にのみ返す。

**Notes**

- Tier 2 の record は delete 時には触らない。
- delete 済み record は Tier 1 から到達不能になり、`reorganize` で物理削除される。
- Failure Rule は 3.2 節と同様: 2. が失敗した操作を WAL に記録してはならない。

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

### 4.3 T2 Reorganize

T2 `reorganize` は Tier 1 の offset を更新する処理であり、常に T1 `reorganize` とセットで実行する。entry 単位でインライン化されている entry(2.1.1 節、7.3 節)は Tier 2 に一切アクセスしないため、この処理の対象から外れる。

### 4.4 Reorganize Trigger & Promotion (空間増幅率と自動トリガーの制御)

`reorganize` が実行される際、空間効率（Space Efficiency）を保証しつつ無駄なディスク I/O を最小化するため、空間増幅率（Space Amplification Ratio） $A$ に基づく動的プロモーション（昇格）を定義する。

#### 空間増幅率 $A$ の定義
空間増幅率 $A$ は、有効（Live）なデータの実サイズ `T1_Live_Bytes` に対する現在の T2 物理ファイルサイズ `T2_Used_Bytes` の比として定義される。
$$A = \frac{\text{T2\_Used\_Bytes}}{\text{T1\_Live\_Bytes}}$$

#### T2 Reorganize (GC / Checkpoint) への昇格判定式

マージ実行時、以下のいずれかの条件を満たす場合のみ、ディスクの物理デフラグ・GC と checkpoint(5 章)を伴う T2 Reorganize を実行し、それ以外は T1-only (インメモリ並列マージソート)で処理を完了する。T2 Reorganize は必ず checkpoint reload の一部として実行されるため(6.2 節)、この式は checkpoint のトリガー条件でもある。

$$\text{T2\_Reorganize\_Trigger} = A \ge A_{\text{limit}} \lor \text{WAL\_Bytes\_Since\_Checkpoint} \ge \text{WAL\_MAX\_BYTES\_SINCE\_CHECKPOINT}$$

* **$A_{\text{limit}}$ (空間増幅率上限):** `1.3` (Storage Fragmentation $\ge 30\%$ 相当。RocksDB 等の標準である空間増幅率上限に基づき定義される)。
* **`WAL_MAX_BYTES_SINCE_CHECKPOINT`**: 直前 checkpoint の checkpoint LSN 以降に WAL へ append されたバイト数の上限。Checkpoint はこのサイズベースのトリガーのみで判定し、書き込みレートに関わらず起動時の WAL replay 時間を有界に保つ。
* **初期挿入 (Insert-only) ワークロード:** ゴミデータが発生しないため $A$ は常に $1.0 < 1.3$ に保たれ、`WAL_MAX_BYTES_SINCE_CHECKPOINT` 到達だけが昇格条件になる。

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

### 4.5 T1 Reorganize Auto-Trigger (ワークロード適応型 L2 キャッシュサイズ制限と Soft/Hard しきい値)

T1 `reorganize` は、`append_region` のサイズに応じて自動的にバックグラウンド実行がトリガーされる。この制御には、ライトバーストを吸収するための容量確保と、並行スキャン（Scan）のレイテンシ低減を両立させるため、**ワークロード適応型（Workload-Adaptive）自動トリガー機構**を導入する。

#### 1. しきい値の定義 (Soft Limit と Hard Limit)
*   **Soft Limit ($T_{\text{soft}}$):** バックグラウンド Reorg スレッドの起動を促すソフトしきい値。
*   **Hard Limit ($T_{\text{hard}}$):** アペンドバッファの完全枯渇とメモリ破綻を防ぐため、新規の書き込み操作（Insert/Update）を一時的にブロッキング（Stall）させるハードしきい値。常に `APPEND_CAP` の 90% 〜 95% の高レベルに固定し、バースト書き込み耐性を最大化する。

#### 2. ワークロード適応型 Soft Limit 計算式 (Workload-Adaptive Thresholding)
スキャン（Scan）操作の有無に応じて、ソフトしきい値 $T_{\text{soft}}$ を動的に切り替える。

*   **スキャン非アクティブ（Pure-Insert ワークロード）:**
    スキャンが実行されていない場合、未ソート領域の走査コストを考慮する必要がないため、書き込み効率と CPU 効率を優先してしきい値を引き上げる。
    $$T_{\text{soft}} = \text{APPEND\_CAP} \times \frac{\text{SoftThresholdPercent}}{100}$$

*   **スキャンアクティブ（Scan-Heavy / Mixed ワークロード）:**
    スキャンが実行されている場合、未ソートのアペンド領域に対する $O(N)$ 線形走査がボトルネックとなる。スキャンの走査データを各 CPU コアの **L2 キャッシュ（プライベートキャッシュ）** に完全に収め、キャッシュライン無効化やメモリバス帯域の競合を防ぐため、L2 キャッシュ容量に基づいた絶対件数へしきい値を縮小する。
    $$T_{\text{soft}} = \min \left( \frac{\text{L2\_Cache\_Bytes}}{\text{sizeof(AppendSlot)}}, \; \text{APPEND\_CAP} \times \frac{\text{SoftThresholdPercent}}{100} \right)$$
    
    *   $\text{L2\_Cache\_Bytes} = 1 \text{ MB}$ （現代の一般的なコアあたり L2 キャッシュ容量）
    *   $\text{sizeof(AppendSlot)} = 40 \text{ バイト}$
    *   スキャンアクティブ時の絶対上限件数: **26,214 件**

#### 3. スキャンアクティブ状態の検出 (Read-only Fast Path)
マルチスレッド並行スキャンにおいてフラグ書き込みによるキャッシュラインの奪い合い（Cache Bouncing）を回避するため、**Read-Check-Write (TEST and SET) パターン**による軽量なアトミックフラグ `scan_active_` を用いる。
1. `scan()` の開始時に `scan_active_` が `false` の場合のみ `true` を書き込む。すでに `true` の場合は読み取り（Read-only）でバイパスし、無駄なキャッシュ無効化を防ぐ。
2. `reorganize()` のマージ完了時に、`scan_active_` を `false` にリセットする。

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


### 5.2 Flow (No-Fork)

`checkpoint reload` は次の手順で行う。`fork` は使用しない(TODO item 2 により廃止)。

1. `reorganize` トリガー(4.4 節の空間増幅率、および WAL サイズ上限のいずれか)成立時、**先に** `checkpoint_lsn = wal.next_lsn() - 1` を読む。その直後に T1 `reorganize()` を呼び出す。呼び出しの中で旧 `append_region` が `append_immutable_` として凍結され、新規の空 `append_region` が atomic pointer swap で公開される。スワップ完了直後、再編成スレッドは一段目のエポック同期バリア（`wait_until_epoch`）を実行し、旧世代領域への書き込み権を保持していたすべての concurrent writers の完了（publish）を待機する。これ以降の Insert / Update / Delete はすべて新世代へのみ書き込まれる(5.3 節 Correctness Rule 参照)。
2. 凍結され、かつ書き込みが完全に静止した `sorted_region` + `append_immutable_` をマージし、新しい `sorted_region` を in-memory に構築する(まだ未公開)。
3. 手順 2 の新しい `sorted_region` を T1 checkpoint の一時ファイルへ書き出す。
4. `offset_mapper` を通じて T2 の live record を新しい T2 の一時ファイルへ順次書き出し、新 offset を手順 2 の `sorted_region` に反映する(既存の T2 reorganize と同一手順)。
5. T1 checkpoint ファイル・T2 ファイルの両方が完成したら、両者の世代情報と checkpoint LSN を記す manifest 一時ファイルを書き、`fsync` する。
6. manifest を `rename()` でアトミックに正式パス(例: `checkpoint.manifest`)へ差し替える。この瞬間に新世代が公式に有効化される。
7. in-memory の新 `sorted_region` と新しい T2 mmap を、既存の reorganize と同じアトミックポインタスワップで公開する(旧世代は epoch based reclamation で安全に解放する)。
   T2 は `mmap(MAP_PRIVATE | MAP_NORESERVE | PROT_READ | PROT_WRITE)` で読み込み、書き込みパスに mprotect は使用しない。
8. WAL をローテーションする(5.5 節 WAL Rotation 参照)。
9. 不要になった旧世代の T1 checkpoint ファイル・T2 ファイルを削除する。

このフローに stop-the-world は存在しない。手順 1 の凍結(atomic pointer swap)が新旧世代を切り分ける唯一の同期点であり、それ以降の新規書き込みは自動的に新世代(次の checkpoint サイクルの対象)に入るため、追いつくための特別な同期処理は不要である。

**Hash Index Lifecycle in Checkpoint**

- `reorganize` 直後は `append_region` が空 (size = 0) となるため、T1 checkpoint file には `sorted_region` のデータのみがシーケンシャルに書き出され、`append_index` の状態自体はファイルへシリアライズ（永続化）しない。
- `reload` 時、新しくマッピングされた T1 インスタンスは、`append_index` を空に初期化した状態で起動し、その後の WAL リプレイおよび新規クライアント書き込み時に適宜ハッシュインデックスへの登録・更新を行う。

### 5.3 Correctness Rule

- checkpoint LSN の決定方法: `t1.reorganize()` (手順 1 の凍結)を呼び出す**前**に `checkpoint_lsn = wal.next_lsn() - 1` を 1 回読む。Insert / Update / Delete は T1 / T2 への適用完了後に WAL append する(3.2 節)ため、この読み取り時点で既に WAL LSN が採番済みの操作は、その T1 適用も凍結前に完了していることが保証され、必ず凍結対象(旧世代)に含まれる。
- checkpoint LSN は、手順 1 の凍結(atomic pointer swap)が完了した時点で WAL へ fsync 済みであることが確定している LSN の最大値以下でなければならない。
- checkpoint LSN を実際より低く見積もる(conservative に倒す)ことは許容される: 起動時の WAL tail replay が checkpoint に既に含まれるエントリを再度適用しても、`T1Index::put()` の overwrite-in-place 意味論により副作用がなく安全である(冪等)。
- checkpoint LSN を実際より高く見積もってはならない: それを行うと、checkpoint に反映されていない有効な更新を tail replay がスキップしてしまい、データロストになる。
- 新しい manifest が `rename()` で正式パスへ反映されるまで、旧世代の T1 checkpoint ファイル・T2 ファイル・WAL は削除してはならない。
- 起動時は、manifest が指す世代の T1 checkpoint ファイルと T2 ファイルを読み込み、その後 checkpoint LSN の次の record から WAL の末尾までを replay する。manifest が存在しない、または読み込みに失敗する場合は、T2 を破棄し WAL 全体を LSN 1 から replay する(item 1 で実装済みのフォールバック挙動)。

### 5.4 T1 Checkpoint File Format (`t1_index.chk`)

T1 checkpoint ファイルは、Tier 2 と同じく「`mmap` して即座に使う」ことを前提とした、パース不要のフラット配列フォーマットである。

**File Layout**

```
[T1ChkFileHeader]
[IndexEntry] * entry_count   -- key_prefix 昇順にソート済み
```

```c++
struct T1ChkFileHeader {
    uint32_t magic;          // フォーマット識別子
    uint8_t  format_version;
    uint8_t  reserved[3];
    uint64_t entry_count;    // 後続する IndexEntry の個数
    uint64_t checksum;       // ヘッダ(checksum フィールドを除く) + 全 IndexEntry 配列に対する FNV-1a64
};
// IndexEntry は 2.1 節と同一レイアウト、32B 固定長
```

**Design Rationale**

- **per-record ヘッダが不要な理由**: Tier 2 の record は可変長 (key_len / value_len が個体ごとに異なる) なので `ValueRecordHeader` が各 record に必要だが、`IndexEntry` は既に固定長 32B (2.1 節、AVX 命令の都合による制約) であるため、ファイル全体で 1 個のヘッダのみで足りる。
- **per-record checksum / torn-tail 検出が不要な理由**: T1 chk は WAL と異なり、生きているプロセスの中で継続的に追記されるファイルではない。1 回の checkpoint 処理でシーケンシャルに書き切り、完成後にのみ manifest から参照される(5.3 節)。したがって「書き込み途中でクラッシュした半端なファイル」が観測されることは、manifest の `rename` が完了しない限り起こらない。ファイル全体に対する 1 個の checksum は、書き込み完了後の bit rot 検出のためだけに存在する。
- **ロード手順**: 起動時、manifest が指す世代の T1 chk ファイルを `mmap(MAP_PRIVATE)` し、header の magic / format_version / checksum を検証したら、`IndexEntry` 配列部分をそのまま `sorted_region` として解釈する。`payload_bits` を `std::atomic_ref<uint64_t>` 経由でアトミックに扱うのは、Tier 2 の `ValueRecordHeader::version` に対する SeqLock 実装(既存)と同じパターンである。パースや個別のハッシュテーブル挿入ループは一切不要であり、これによって "instantaneous" な Fast Boot を実現する。
- **`append_index` はシリアライズしない**: 5.2 節の Hash Index Lifecycle の通り、checkpoint 直後は `append_region` が空であるため、ハッシュインデックス自体は永続化不要で、起動時に空の状態から再構築される。
- **runtime 表現との関係**: プロセス内で通常の(checkpoint を伴わない)T1-only reorganize が作る `sorted_region` は、従来通り heap 上の配列のままでよい。checkpoint 時にのみ、この配列の内容を上記フォーマットでファイルへコピーする。次回起動時の T1 chk 読み込みだけがこのファイルを直接 `mmap` して使う。同一プロセス内で checkpoint 直後にランタイム表現自体を mmap 領域へ切り替えるゼロコピー最適化は、今回はスコープ外とする。

### 5.5 WAL Rotation

WAL は先頭からの truncate を行わない(可変長レコード列の先頭を削るのは高コストなため)。代わりに、checkpoint 完了後にレコードを新しいファイルへ移し替える「ローテーション」を行う。

**Procedure**

1. checkpoint の manifest が正式パスへ `rename` された直後、`Wal` の `append_mutex_` を取得する(新規 append を短時間ブロックする)。
2. ロック保持中に、現在の WAL ファイルから `lsn > checkpoint_lsn` のレコードのみを新しい一時 WAL ファイルへ `pread` でコピーする。
3. 新しい WAL ファイルを `fsync` し、`rename()` で正式な WAL パスへアトミックに差し替える。`Wal` の内部 `fd_` を新ファイルへ向け直す。
4. ロックを解放する。以降の新規 append は新しい WAL ファイルへ入る。

**Notes**

- ロック保持時間はコピーするレコード量に比例し、`WAL_MAX_BYTES_SINCE_CHECKPOINT`(4.4 節)が上界を与える。この停止は WAL への新規 append のみに影響し、Get / Scan、および進行中の Insert / Update / Delete の T1 / T2 側の処理には影響しない。
- コピーされる tail レコードは元の `lsn` をヘッダに保持したまま新ファイルへ入るため、`Wal` のコンストラクタが既に持つ「末尾の有効 record から `next_lsn_` を復元する」ロジックがそのまま機能する。
- 例外として、tail が空(checkpoint_lsn が採番済みの最大 LSN と一致する)の場合は、新しい WAL ファイルにレコードが 1 件も無いため、通常のコンストラクタでは `next_lsn_` が `1` にリセットされてしまう。これを避けるため、`Wal` は開始 LSN を明示指定して空ファイルを作成する専用コンストラクタ(または factory)を持つ。
- ローテーション前の WAL ファイルは、新ファイルへの `rename` が完了した時点で置き換えられるため、明示的な削除は不要である。

## 6. Concurrency Contract

### 6.1 Base Assumption

point operation は通常時にオンラインで進める。
checkpoint reload は T1 `reorganize()` がトリガーする atomic pointer swap 機構で実現され、専用の stop-the-world は必要としない。

この節では具体的な同期機構そのものではなく、各操作がどの操作と競合し、競合時に何を保証すべきかを定義する。

### 6.2 Required Guarantees

- Get / Scan は通常時にオンラインで実行できる。
- Insert / Update / Delete は WAL 永続化後に T1 / T2 を更新する。
- T1-only `reorganize` は T2 と独立して高頻度に走らせてよい。
- T2 `reorganize` は checkpoint reload の一部としてのみ実行する。
- checkpoint reload は T1 `reorganize()` 内の atomic pointer swap が完了した時点で有効化される。専用の stop-the-world は発生しない。
- pointer swap の前後をまたいで進行する操作については、retry・snapshot・serialization のいずれかで整合を保つ。

### 6.3 Operation Concurrency Matrix

以下の表は、各操作の並行実行可否と必要な扱いを定義する。

- `Allowed`: 特別な停止なしに並行実行してよい
- `Allowed with retry/snapshot`: 並行実行してよいが、snapshot 読みまたは retry が必要
- `Serialized`: 同一 key に対しては直列化が必要
- `Single-flight`: 並行呼び出し時、実行者（leader）を 1 スレッドだけ選出する。follower の挙動は経路で異なり、soft 経路では待機せず継続、hard 経路では leader 完了まで待機する（アトミックフラグで制御）。

`Checkpoint reload` は `T1 reorganize` (4.4 節のトリガー成立時)の中でトリガーされ、同一の `reorg_running_` single-flight ロックを共有する。すなわち両者は同じ実行スロットを取り合う、実質的に同一操作のバリエーションである。

| Operation A / B | Get | Scan | Insert | Update/Delete | T1 reorganize | Checkpoint reload |
| --- | --- | --- | --- | --- | --- | --- |
| `Get` | Allowed | Allowed | Allowed | Allowed | Allowed with retry/snapshot | Allowed with retry/snapshot |
| `Scan` | Allowed | Allowed | Allowed | Allowed | Allowed with retry/snapshot | Allowed with retry/snapshot |
| `Insert` (distinct key) | Allowed | Allowed | Allowed | Allowed | Allowed with retry/snapshot | Allowed with retry/snapshot |
| `Insert` (same key) | Allowed | Allowed | Serialized | Serialized | Allowed with retry/snapshot | Allowed with retry/snapshot |
| `Update/Delete` (distinct key) | Allowed | Allowed | Allowed | Allowed | Allowed with retry/snapshot | Allowed with retry/snapshot |
| `Update/Delete` (same key) | Allowed | Allowed | Serialized | Serialized | Allowed with retry/snapshot | Allowed with retry/snapshot |
| `T1 reorganize` | Allowed with retry/snapshot | Allowed with retry/snapshot | Allowed with retry/snapshot | Allowed with retry/snapshot | Single-flight | Single-flight (同一実行スロット) |
| `Checkpoint reload` | Allowed with retry/snapshot | Allowed with retry/snapshot | Allowed with retry/snapshot | Allowed with retry/snapshot | Single-flight (同一実行スロット) | Single-flight |

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
- ただし、writer が旧世代バッファに対して行う書き込み（reserve & publish）は、再編成スレッド側のマージ開始前に実行される一段目のエポック同期バリア（`wait_until_epoch`）によって完全にドレインされる。これにより、書き込みスレッド側でのリトライや明示的なロック同期を一切不要としつつ、進行中のすべての更新がデータロストなく新旧いずれかの世代に安全に振り分けられる。

#### Checkpoint Reload vs All Operations

- Checkpoint reload は T1 `reorganize()` の中でトリガーされ、`reorganize` と同一の atomic pointer swap 機構を用いる。したがって「T1 Reorganize vs Readers」「T1 Reorganize vs Writers」で述べた整合性規則がそのまま適用される。
- T1 checkpoint ファイル・T2 ファイルの構築中も、Get / Scan / Insert / Update / Delete は通常通り継続してよい。これらは新世代の `append_region` へ書き込まれるか、旧世代のスナップショットを読むかのいずれかであり、進行中の checkpoint file 構築とは一切干渉しない。
- 新世代への切り替え(manifest の `rename`、および in-memory の pointer swap)が完了するまでは、クライアントへ新世代を公開してはならない。

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

### 7.1 Group Commit（実装済み）/ Early Lock Release / Flush Pipelining（未実装・将来検討）

**Group Commit は実装済み。** `wal.hpp`/`wal.cpp`の`Wal`クラスを参照。ロックフリー（ホットパスに`std::mutex`を一切使わない）な固定長リングバッファ方式で実装されている。詳細は [Aether](https://dl.acm.org/doi/10.14778/1920841.1920928) の設計を参考にしたが、完全に同一の方式ではない：

- 各`append_*()`呼び出しは、単一のatomic `fetch_add`でLSNを確定し、その値をmod capacityしたスロット番号へ、ヒープ確保した自分のレコードへのポインタをCASで publish する（Aether論文そのものの「可変長バイト列を直接リングへ書き込む」方式ではなく、スロットにはポインタのみを置く簡易版）。
- 最初にスロットへの publish に成功し、かつ他に実行中のflushがない呼び出し元が"leader"として選出される（`reorg_running_`と同じCAS/atomic wait-notify方式、`std::mutex`は使わない）。leaderは自分を含む待機中の全レコードをドレインし、`writev()`（IOV_MAXごとにチャンク化）でまとめて書き込んだ後、バッチ全体に対して1回だけ`fdatasync()`する（`fsync()`ではない——データ復元に無関係なメタデータの同期を省く。ファイルサイズの同期はPOSIX上保証されるのでreplayに必要な分は失われない。RocksDBのWALと同じ選択）。
  - 個別レコード毎の`write()`ではなく`writev()`一括書き込みを採用しているのは、失敗時の被害範囲(1件のみ失敗させたい)という当初の懸念が、実際には元々「1回の共有catchブロックでバッチ全体を失敗扱いにする」設計だったため最初から成立しておらず、`writev()`化はこの失敗セマンティクスを何も悪化させないと判明したため。
  - fsync完了後の起床は、レコード毎の`done`フラグ経由ではなく、バッチの最高LSNを1つの共有カウンタ（`highest_settled_lsn_`）に書き込み`notify_all()`する方式（follower側は自分のLSN以下になるまで`wait()`）。当初はレコード毎`done`フラグ+個別`notify_one()`で実装したが、フルシステム(実際のInsert/Update/Delete、T1 lookup等の他のCPU処理と混在)でのAWS実測により、このnotify_one()ループのコストがバッチサイズに対して**超線形**に増大し(batch=1ではほぼ無視できるが、batch=32ではfdatasync()自体の約13倍のコストになる)、共有カウンタ方式に置き換えた。
- レコードの生存期間はイントルーシブな参照カウント（`std::atomic<int>`一発の`fetch_sub`）で管理する。`std::atomic<std::shared_ptr<T>>`は使わない（多くの実装で内部的にスピンロックを使うため、真のロックフリー性を損なう）。

以下は本実装のスコープ外、別の最適化として引き続き未実装のまま：

- エントリロック解放を WAL flush 完了前に前倒しする（early lock release）。
- 読み取りも waiter を介して未 flush データの整合を取る（flush pipelining）。

### 7.2 SIMD Tier 1 Scan

`IndexEntry` は fixed-size かつ連続配置なので、Tier 1 の append scan や range scan は SIMD 最最適化しやすい。

### 7.3 Entry-Level Adaptive Covering (Dynamic T1 Inline Optimization)

エントリーごとに T2 オフセットとインライン 64-bit 値を動的に切り替える最適化である。値が 8バイト（64ビット）以下のときに Tier 2 への書き出しをバイパスして Tier 1 インデックスの payload 領域内に直接バリューをインライン格納する。

本最適化は `vmemkv::Config` のテンプレート引数タグ（`T1InlineValue`）を介して制御される。

#### 7.3.1 メタデータとハッシュのエンコーディング
T1のインデックススロットに十分な空きビット領域がないため、64ビットのハッシュフィールド（`hash`）の最上位4ビットをメタデータ領域として再利用し、フラグとサイズ情報を格納する。

- **Bit 63 (`is_inline`)**: `1` の場合はインラインデータ、`0` の場合は T2オフセットを表す。
- **Bits 62-60 (`inline_size`)**: インラインデータのバイトサイズ（1〜8バイト）を表す。サイズ `8` は `0` としてエンコードされる。
- **Bits 59-0 (`clean_hash`)**: 実際の60ビットFnvハッシュキー。インデックスの検索、Bloom filterの登録・判定、SIMDスキャン等のハッシュ比較時には、上位4ビットをマスクしてこの60ビット部分のみを比較する。

#### 7.3.2 インライン化の動作
- **T2 オフセットとの識別 (判定)**:
  - 読み出し時、T1から取得したスロットハッシュの最上位ビット（Bit 63）を確認するだけで、T2をフェッチせずにインラインかオフセットかを100%確実に識別できる。
- **値が 1〜8 バイトの場合**:
  - `payload_bits` に対するビットシフトやビットの埋め込みは行わず、64ビットのビットパターンをそのまま無加工で格納し、デコード時は `inline_size` に従いバイトコピーを行う。これにより、`double`、`time`、連番のサロゲートキー（偶数・奇数を問わず）など、あらゆる64ビット以内のデータ型を完全にインライン化できる。

### 7.4 Chunk Allocation / Pre-faulting (T2 Lock Mitigation)

マルチコア高並列書き込み環境における Linux カーネルの仮想メモリページフォールトおよび `mmap_lock`（VMAロック）の競合によるオーバーヘッド（Cache Line Bouncing）を緩和するための最適化である。

* **スレッドローカル Chunk アロケーション**: 
  各書き込みスレッドが T2 領域へ `append` する際、アトミックカウンタから個別に領域をアロケートするのではなく、スレッドごとに大きなブロック（例: 2MB）を一括予約する。
* **一括 Pre-faulting**:
  Chunk 確保直後に、その 2MB 領域を 4KB ページ単位（標準ページサイズ）でダミーライトし、カーネルに物理メモリページを一括で割り当てさせる。
  これにより、その後の個々の `insert`（`memcpy`）においてはページフォールトの発生が **1/500** に激減し、OS カーネルの `mmap_lock` に入ることなく完全に並行して超高速にメモリコピーを実行できるようになる。
* **コンパイル時解決の徹底**:
  最適化のオーバーヘッドをゼロにするため、`vmemkv::Config` のテンプレート引数タグ（`Prefaulting`）を介して `constexpr if` によってコンパイル時に分岐とコード生成が制御される。

#### 7.4.1 T2 Get 側の先読み (`MADV_WILLNEED`) — 検証の結果、不採用

7.4 節の Pre-faulting は書き込み(Insert)経路専用であり、読み込み(Get)側——特に LTM (Larger-than-memory) 環境下でスワップアウト済みの T2 ページを読み直す際の major fault——は対象外のままだった。この読み込み側のフォールトコストを、`madvise(MADV_WILLNEED)` によるバッチ先読みで軽減できないか検証したが、**単一スレッドでも 32 スレッド並行でも明確に逆効果**という結果になったため、不採用と判断した。

* **狙い**: 複数キーの T1 lookup を先に済ませ、対応する T2 ページ群に `MADV_WILLNEED` を一括発行してから実際に値を読みに行けば、フォールト待ちを他キーの処理と重ねられるはずだった。
* **検証方法**: `madvise(MADV_PAGEOUT)` で対象ページを強制的かつ決定的にスワップアウトさせたうえで(`/proc/self/status` の `VmSwap` と `/proc/self/stat` の `majflt` で実際にスワップ・フォールトが発生したことを確認)、(a) 直接タッチ、(b) 64 ページ単位で `MADV_WILLNEED` を発行してからタッチ、の2パターンの所要時間を比較した。
* **結果**:
  - 単一スレッド: `MADV_WILLNEED` 後のタッチは major fault 数が期待値の 2000 から実測 0 まで低下し(先読みは機能している)、それでも所要時間は **1.28倍** 悪化した。
  - 32 スレッド並行(実運用の thread=32 相当、スレッドごとに独立領域を割り当てて相互干渉を排除): 同様に major fault 数は 0 まで低下したが、所要時間は **6.4倍** 悪化した。
* **原因の推定**: major fault というイベント自体は確かに消えているが、そのコストは消えておらず `madvise()` 呼び出しの中に移動しているだけである。`MADV_WILLNEED` も内部で `mmap_lock` を取得すると考えられ、素朴なフォールト任せの場合よりも同じ共有ロックへのアクセス回数を増やしてしまい、7.4 節で回避しようとしている競合 (Cache Line Bouncing) をむしろ悪化させている可能性が高い。並行度を上げるほど悪化幅が拡大した(1.28倍 → 6.4倍)ことも、ロック競合起因という推定と整合する。
* **結論**: mmap ベースの T2 に対する読み込み側の先読みは、`madvise` という OS 標準の非同期化手段を使っても改善しなかった。読み込み側の page fault / `mmap_lock` コストの軽減は、別のアプローチ(§7.4 で挙げた書き込み側 pre-faulting のような形での適用可否、あるいはカーネル側の per-VMA lock 拡張を待つなど)を要する、未解決の課題として残る。

#### 7.4.2 T2 の Huge Page 化 (`MADV_HUGEPAGE`) — 検証の結果、不採用

7.4.1 節の代案として、T2 のフォールト単位そのものを 4KB から 2MB (Transparent Huge Page) に引き上げ、1 回のフォールトでより多くのレコードをまとめて読み込むことでフォールト回数(ひいては `mmap_lock` への合流回数)自体を減らせないか検証したが、**Uniform・Zipf 的局所性のあるアクセスいずれでも効果は測定できなかった**ため、不採用と判断した。

* **狙い**: 2MB 単位でスワップインされれば、同じ 2MB 領域内の複数レコードへのアクセスが「初回だけ実フォールト、以降は無料」になり、フォールト絶対数を削減できるはずだった。
* **検証方法**: `madvise(MADV_PAGEOUT)` による決定的な強制退避は THP には効かないことが判明した(退避を要求しても `VmSwap` が全く増えない)ため、cgroup の `MemoryHigh` 予算(1GiB)の 4 倍(4GB)の匿名領域を実際に連続タッチし、カーネル自身の real reclaim に本物のスワップアウトを行わせる方式で検証した。`/proc/self/status` の `VmRSS` / `VmSwap` / `AnonHugePages` の合計がマッピングサイズと一致することを確認して初めて数値を信頼した(検証序盤、埋め込みループが `bytes[off] = (char)off` で `off` が常にページ境界=下位バイト常にゼロという凡ミスで全ページが実質ゼロページ相当になり、スワップも常駐もされない不可解な結果が出たため、ページごとに変化する擬似乱数内容で埋め直して修正している)。
* **結果**(THP 有効時、`AnonHugePages` が常駐分のほぼ全量を占めることを確認済み):
  - クラスタ化アクセス(ホットな先頭 5% 領域に集中): baseline 124.4us/タッチ vs THP 125.9us/タッチ (誤差 1%程度)。major fault 数も 1957/2000 vs 1958/2000 とほぼ同数——2MB 単位でまとめて常駐するなら大半が「無料」になるはずだったが、実際にはほぼ全タッチが個別に実フォールトしている。
  - Uniform アクセス(全域に分散、再訪なし): baseline 97.3us/タッチ vs THP 98.5us/タッチ。理論上ここは再利用機会がないため差が出ないと予想しており、その予想通りだった。
* **原因の推定**: 常駐中は THP として扱われている(`AnonHugePages` で確認済み)ものの、スワップアウト時に 2MB 単位を保ったまま書き出されず、512 個の 4KB ページに分割されてから個別にスワップされていると考えられる。クラスタ化アクセスで期待した「まとめて常駐する」恩恵は、スワップが絡む LTM 環境では実質的に失われる。
* **結論**: T2 の Huge Page 化は、常駐時のメリット(TLB 圧迫の軽減等)はあり得るとしても、LTM Get_Hit のようにスワップが継続的に発生するシナリオでのフォールト回数削減には寄与しない。

### 7.5 Sorted Bloom Filter

`sorted_region` 全体に Bloom filter を付与し、negative lookup を高速化できる。

### 7.6 WAL Rotation via `FALLOC_FL_COLLAPSE_RANGE`（未実装・将来検討）

5.5 節の WAL Rotation はレコードコピー方式(移植性重視)を基本とするが、対応ファイルシステム(ext4, xfs 等。tmpfs 等では非対応)では `fallocate(FALLOC_FL_COLLAPSE_RANGE)` によりファイル先頭のバイト範囲をコピー無しで直接除去できる。コピーを伴わないためローテーションの停止時間をさらに縮小できるが、Linux カーネル・ファイルシステム依存の機能であるため opt-in とする。

### 7.7 Tier 1 madvise(MADV_RANDOM) Optimization (NoMadvise アブレーション)

T1インデックスに対して、仮想メモリマップ時のReadahead（カーネル先読み）を抑止しランダムアクセス性能を最適化する。
- **最適化の内容**: mmap領域のマップ直後に `madvise(..., MADV_RANDOM)` を呼び出し、OSカーネルの不要なページ先読み・カーネル空間メモリバス帯域の浪費を防ぐ。
- **NoMadvise アブレーションバリアント**: `vmemkv::Config` のテンプレート引数タグを介して、madvise呼び出しの有無をコンパイル時（constexpr if）に分岐解決する。ベンチマーク測定により、このmadviseの適用によって in-memory Get_Hit において約 5% の有意な性能向上効果が得られることが実証されている。

## 8. Parameters

| Parameter | Meaning |
| --- | --- |
| `T1_MAX_INDEX_SIZE` | Tier 1 最大 entry 数 |
| `T1_REORGANIZE_SOFT_THRESHOLD` | T1 Reorganize を非同期実行するソフトしきい値（スキャン有無で動的変化） |
| `T1_REORGANIZE_HARD_THRESHOLD` | 新規書き込みをブロッキング（Stall）するハードしきい値（通常95%固定） |
| `T2_MAX_VIRTUAL_MEMORY_SIZE` | Tier 2 最大仮想アドレス空間 |
| `WAL_MAX_BYTES_SINCE_CHECKPOINT` | 直前 checkpoint 以降に許容する WAL 蓄積バイト数の上限。超過で checkpoint (T2 Reorganize) へ昇格する |
| `T2_STORAGE_FRAGMENTATION_THRESHOLD` | Tier 2 storage fragmentation の閾値 |
| `T2_ORDERING_FRAGMENTATION_THRESHOLD` | Tier 2 ordering fragmentation の閾値 |
| `WAL_SYNC_MODE` | WAL `fsync` ポリシー |

