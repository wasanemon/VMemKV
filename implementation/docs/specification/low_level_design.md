# VMemKV Low Level Design

## 1. Overview

本書は [high_level_design.md](./high_level_design.md) を受けて、VMemKV の low-level なデータレイアウト、操作手順、バックグラウンド処理、opt-in 最適化、および主要パラメータを定義する。

## 2. Core Data Structures

### 2.1 Tier 1

Tier 1 は fixed-size のエントリを格納するインデックス層である。`sorted_region` と `append_region`
はどちらもエントリの配列として保持するが、フィールド構成はランタイム表現とオンディスク
(checkpoint)表現とで異なる。

**概念モデル**(両表現に共通するコアフィールド):

```c++
using StoreKeyPrefix = std::array<std::byte, 16>; // 16-byte key prefix

struct IndexEntry {
    StoreKeyPrefix key_prefix;  // primary sort key
    uint64_t hash;              // hash(full_key)
    uint64_t payload_bits;      // Tier 2 offset, or an entry-level inlined value (2.1.1 節)
};

constexpr uint64_t TOMBSTONE_PAYLOAD = UINT64_MAX;
```

| Field | Meaning |
| --- | --- |
| `key_prefix` | 主ソートキー。Tier 1 の並び順を決める。 |
| `hash` | フルキーのハッシュ値。prefix だけでは区別できない候補の絞り込みに使う。 |
| `payload_bits` | 通常は Tier 2 の byte offset。entry 単位でインライン化されている場合は値そのもの(2.1.1 節)。`UINT64_MAX` は tombstone。 |

**ランタイム表現**: `sorted_region` は `SortedSlot`(48バイト: 上記コアフィールド32バイト +
`generation`/`version`)、`append_region` は `AppendSlot`(56バイト: `SortedSlot` の全フィールド +
`published` フラグ)の配列を持つ。`generation` は `payload_bits` が指す Tier 2 世代を、`version` は
in-place 更新中の一貫性を保証する seqlock を保持する(6章)。`hash`/`payload_bits`/`generation` は
すべて `std::atomic` であり、並行な読み取りと put() の in-place 更新を両立させる。

**オンディスク(checkpoint)表現**: `T1ChkEntry`(32バイト、上記コアフィールドのみ)。`generation`/
`version`/`published` はランタイムの並行制御専用であり、単一プロセスの読み込みで完結する
checkpoint には不要なため永続化しない(5.4節)。

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

**Base/Tail Split**

`T2Store` は `base_boundary` を保持する: `[0, base_boundary)` の範囲が、baseリージョン専用の読み取り経路(7.9節 -- 用途・レコードサイズ別に3つある)経由で seqlock なしに安全に読める、という不変条件を表す境界値である。`base_boundary` は、その `T2Store` インスタンスの生存期間中は**一定**であり、`checkpoint_internal()`(5.2 節)が新しい世代を作って丸ごと入れ替える時にのみ、新しい値へ進む。

- `offset < base_boundary` の record を **base** 領域、`offset >= base_boundary` の record を **tail** 領域と呼ぶ。
- base 領域は、一度 `base_boundary` に含まれた後は二度と in-place では書き換えられない(3.3節)。そのため base 領域は、mmap の `MADV_RANDOM`(7.7節)を経由しない専用の読み取り経路で読んでも安全であり、seqlock も不要になる(経路の選び方は7.9節)。
- tail 領域は通常通り in-place 更新の対象になり得るため、主 mmap 経由(COW反映済みの最新ビュー、seqlock保護)でしか安全に読めない。

**Invariants**

- Tier 2 の live record は必ず Tier 1 のいずれかの live `IndexEntry.payload_bits` から到達可能である。
- Tier 1 から参照されなくなった Tier 2 record は garbage である。現在、Tier 2 のどの機構もこの garbage を物理的に回収しない(4.3 節) -- `bytes_used` は単調増加のみで、reorganize/checkpoint のいずれも縮小させない。
- `update` は可能なら既存 record を in-place で上書きする。
- 新しい value が `alloc_len` を超える場合は `bytes_used` 位置に新 record を追加し、Tier 1 の `offset` を差し替える。
- 追記時の `bytes_used` の増分は、`ValueRecordHeader + key bytes + value bytes + padding（alloc_len まで）` に、次の record 開始位置を `alignof(ValueRecordHeader)` 境界に揃えるための調整を加えた合計で決まる。
- 常に `bytes_used <= bytes_capacity` を満たす。
- 追記要求で容量不足 (`bytes_used + required_bytes > bytes_capacity`) になった場合、その write request は失敗として扱う(キューイングや自動リトライは行わない)。

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
- 手順3の in-place 判定には `offset >= base_boundary`(2.2節)の条件も含まれる。この境界未満を指す record への更新は、たとえ `new_value_len <= alloc_len` でも in-place にはせず、手順4の追記パスに強制的に回す。base 領域は専用mmapで直接読む読み取り経路(7.9節)の前提として「二度と書き換わらない」ことに依存しているため。`base_boundary` はその `T2Store` インスタンスの生存期間中一定なので、この判定は追加の同期なしに安全である。

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

`reorganize` は Ordering Fragmentation を解消する: Tier 1 `append_region` の肥大化により候補探索・確認コストが増え、Get / Scan が遅くなる問題である。

Tier 2 側にも delete や append-update の結果として生じる Storage Fragmentation(Tier 1 から参照されない古い Tier 2 record の蓄積)が存在するが、現在この蓄積を物理的に回収する機構はない(4.3 節)。

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
7. `append_region` に紐づくハッシュインデックス(`append_index`)を完全にクリア(空に初期化)する。

**Effect**

- Ordering Fragmentation を解消する。
- delete 済み entry を Tier 1 から取り除く。

### 4.3 T2 Reorganize (`checkpoint_internal()`)

T2 側の reorganize は `checkpoint_internal()`(5.2 節)一本であり、Tier 1 の `reorganize` と同じサイクルの中で必ずセットで実行される。entry 単位でインライン化されている entry(2.1.1 節、7.3 節)は Tier 2 に一切アクセスしないため、この処理の対象から外れる。

`checkpoint_internal()` は Tier 2 の**単一の永続ファイル**に対する in-place な追記のみを行う: このサイクルより前に durable だったバイト(`[0, old_base_boundary)`)には一切触れず、tail 領域(`[old_base_boundary, bytes_used)`)にある生存中の record を、それぞれが既に占有している同じ offset へ `pwrite()` するだけである。record のリロケーション(offset の付け替え)や、参照を失った record の物理的な回収は行わない -- つまり Storage Fragmentation の解消(GC)はこの処理の対象外である。Tier 1 の `payload_bits` は書き換えず、T2Memory の世代タグのみを新しい世代に張り替える。

**Input**

- T1 の `reorganize` 後 `sorted_region`
- T1 の現 `append_region`
- Tier 2 の `[old_base_boundary, bytes_used)` にある生存中の record

**Output**

- 新しい世代の `T2Memory`(同一ファイルの再 `mmap`。offset は変わらず、`base_boundary` のみ前進する)
- 世代タグを更新した Tier 1

**Procedure**

1. T1 の tail 領域(`sorted_region` 決定後の `append_region`、および tail 常駐の生存 entry)を走査し、各 live `IndexEntry.payload_bits` から Tier 2 record を取得する。
2. 取得した record を、それが既に占有している同じ offset へ Tier 2 の永続ファイルに `pwrite()` する(alloc_len を含め、フットプリントは変更しない)。
3. Tombstone 化されている entry と、Tier 1 から参照されない Tier 2 record は書き込まない。
4. 新しい `base_boundary` を、この時点での Tier 2 の `bytes_used` に定める。
5. Tier 1 の各 live entry の世代タグを、新しく `mmap` した T2Memory の世代に更新する(`payload_bits` 自体は不変)。

**Effect**

- Tier 2 の tail 領域を永続化し、`base_boundary` を前進させる。
- Storage Fragmentation は解消しない(4.1 節)。

### 4.4 Reorganize / Checkpoint Trigger

`checkpoint()` を起動するトリガーは WAL サイズのみである。

$$\text{Checkpoint\_Trigger} = \text{WAL\_Bytes\_Since\_Checkpoint} \ge \text{WAL\_MAX\_BYTES\_SINCE\_CHECKPOINT} \lor \text{tail\_entries\_near\_capacity} \lor \text{Force}$$

* **`WAL_MAX_BYTES_SINCE_CHECKPOINT`**: 直前 checkpoint の checkpoint LSN 以降に WAL へ append されたバイト数の上限。Checkpoint はこのサイズベースのトリガーのみで判定し、書き込みレートに関わらず起動時の WAL replay 時間を有界に保つ。
* **`tail_entries_near_capacity`**: tail 領域の生存 entry を追跡する固定容量バッファ(`checkpoint_internal()`の `copy_live_entries()`が消費する)が閾値に近づいた場合、容量枯渇を避けるため早期に checkpoint する。
* **`Force`**: `checkpoint()` の明示呼び出し。
* 公開 API は **`reorganize()`**(T1-only インメモリマージ、T2 に触れず checkpoint もしない)・**`defragment()`**・**`checkpoint()`** の3つ。`checkpoint()` は `checkpoint_internal()` を呼ぶ。`defragment()` は現状 `reorganize()` と同じ効果(T1-only マージ)のプレースホルダであり、将来 Storage Fragmentation を解消する実装のための入口として API 上残してある。

| API | 効果 |
|---|---|
| `reorganize()` | T1 の Append→Sorted マージのみ。T2/ディスク非関与 |
| `checkpoint()` | Tier 2 の tail を in-place で永続化し、manifest を commit して WAL を rotate する |
| `defragment()` | `reorganize()` と同一のプレースホルダ。Storage Fragmentation は解消しない |

### 4.5 T1 Reorganize Auto-Trigger (ワークロード適応型 L2 キャッシュサイズ制限と Soft/Hard しきい値)

T1 `reorganize` は、`append_region` のサイズに応じて自動的にバックグラウンド実行がトリガーされる。この制御には、ライトバーストを吸収するための容量確保と、並行スキャン（Scan）のレイテンシ低減を両立させるため、**ワークロード適応型（Workload-Adaptive）自動トリガー機構**を導入する。

#### 1. しきい値の定義 (Soft Limit と Hard Limit)
*   **Soft Limit ($T_{\text{soft}}$):** バックグラウンド Reorg スレッドの起動を促すソフトしきい値。
*   **Hard Limit ($T_{\text{hard}}$):** アペンドバッファの完全枯渇とメモリ破綻を防ぐため、新規の書き込み操作（Insert/Update）を一時的にブロッキング（Stall）させるハードしきい値。常に `APPEND_CAP` の 95% に固定し、バースト書き込み耐性を最大化する。

#### 2. ワークロード適応型 Soft Limit 計算式 (Workload-Adaptive Thresholding)
スキャン（Scan）操作の有無に応じて、ソフトしきい値 $T_{\text{soft}}$ を動的に切り替える。

*   **スキャン非アクティブ（Pure-Insert ワークロード）:**
    スキャンが実行されていない場合、未ソート領域の走査コストを考慮する必要がないため、書き込み効率と CPU 効率を優先してしきい値を引き上げる。
    $$T_{\text{soft}} = \text{APPEND\_CAP} \times \frac{\text{SoftThresholdPercent}}{100}$$

*   **スキャンアクティブ（Scan-Heavy / Mixed ワークロード）:**
    スキャンが実行されている場合、未ソートのアペンド領域に対する $O(N)$ 線形走査がボトルネックとなる。スキャンの走査データを各 CPU コアの **L2 キャッシュ（プライベートキャッシュ）** に完全に収め、キャッシュライン無効化やメモリバス帯域の競合を防ぐため、L2 キャッシュ容量に基づいた絶対件数へしきい値を縮小する。
    $$T_{\text{soft}} = \min \left( \frac{\text{L2\_Cache\_Bytes}}{\text{sizeof(AppendSlot)}}, \; \text{APPEND\_CAP} \times \frac{\text{SoftThresholdPercent}}{100} \right)$$
    
    *   $\text{L2\_Cache\_Bytes} = 1 \text{ MB}$ （現代の一般的なコアあたり L2 キャッシュ容量）
    *   $\text{sizeof(AppendSlot)} = 56 \text{ バイト}$
    *   スキャンアクティブ時の絶対上限件数: **18,724 件**

#### 3. スキャンアクティブ状態の検出 (Read-only Fast Path)
マルチスレッド並行スキャンにおいてフラグ書き込みによるキャッシュラインの奪い合い（Cache Bouncing）を回避するため、**Read-Check-Write (TEST and SET) パターン**による軽量なアトミックフラグ `scan_active_` を用いる。
1. `scan()` の開始時に `scan_active_` が `false` の場合のみ `true` を書き込む。すでに `true` の場合は読み取り（Read-only）でバイパスし、無駄なキャッシュ無効化を防ぐ。
2. `reorganize()` のマージ完了時に、`scan_active_` を `false` にリセットする。

## 5. Checkpoint Reload

### 5.1 Motivation

T2 の稼働中 mmap は `MAP_PRIVATE` であり、書き込みはプロセス内の COW ページに留まるだけでファイルには一切反映されない(下記 NOTE 参照)。したがって Tier 2 の tail 領域を再起動後も残るデータにするには、`checkpoint_internal()` がその内容を明示的にファイルへ `pwrite()` する必要がある。

`checkpoint_internal()` は record を一切リロケーションしない(4.3 節)ため、新しい一時ファイルを構築する必要も、ファイル単位で切り替える必要もない -- Tier 2 の**単一の永続ファイル**に、既に生存中の record が占有している offset へそのまま `pwrite()` するだけで済む。旧世代(`old_base_boundary` 未満)のバイトは一切書き換えない。

> [!NOTE]
> tail 領域の record は主 mmap 経由の `pwrite()` ではなく、通常のファイル I/O としての `pwrite()` で書き出す。
> mmap 経由で書き込みページをファイルへ反映させるには `MAP_SHARED` かつ明示的な `msync()` が必要になり、`MAP_PRIVATE` で書き込みを COW に留めている T2 の主 mmap の設計(下記 NOTE)とは別の書き込み経路が要る。`pwrite()` はカーネルページキャッシュを経由するため、書き込み先が既にページキャッシュに乗っていれば追加のマイナーページフォルトを伴わない。

> [!NOTE]
> T2 の mmap には `MAP_PRIVATE | MAP_NORESERVE | PROT_READ | PROT_WRITE` を使用します。
> `MAP_PRIVATE` により書き込みはプロセス内の COW ページに留まり、ファイルには反映されません（再起動時揮発）。
> `MAP_NORESERVE` により、カーネルが mmap 時点でスワップ領域を一括予約するのを回避します。これにより、物理 RAM を大幅に超える仮想アドレス空間（例: 64 GB）を確保しても ENOMEM が発生しません。物理ページはアクセス時にオンデマンドで割り当てられ、通常通りスワップアウトされます。
> この設計により、mprotect による書き込みページ保護の往復（RO → RW → RO）が不要になり、書き込みパスが大幅に簡潔かつ高速になります。


### 5.2 Flow (`checkpoint_internal()`)

`checkpoint()` は内部機構 `checkpoint_internal()` を呼ぶ(4.4 節)。同一プロセス内で完結し、`fork` は使わない。

1. `reorganize` トリガー(4.4 節)成立時、**先に** `checkpoint_lsn = wal.next_lsn() - 1` を読む。同時に、現行世代の `old_base_boundary` を捕捉する(この値は現行世代の生存期間中一定 -- 2.2 節)。
2. Tier 2 の永続ファイルを開く(初回のみ `O_CREAT` で作成)。まだ一度も checkpoint していないストアでも、このファイルが Tier 2 の唯一の実体になる。
3. Pre-stop パス(性能最適化、任意回数実行可): tail 領域(`payload_bits` の offset が `old_base_boundary` 以上)にある生存中の entry を、書き込みスレッドを止めずに走査し、各 record を seqlock 越しに読み取ってその offset へ `pwrite()` する。
4. `T2FlatFile::stop_writers_and_wait()` で新規の書き込みハンドル発行を止め、既に発行済みのハンドルが全て解放されるまで待つ。
5. Post-stop パス(唯一の破壊的パス、正しさの主体): pre-stop パスで見つからなかった残りの生存 entry を同様に `pwrite()` する。この時点で writer は止まっているため、tail 領域の生存集合は確定している。
6. この時点の Tier 2 `bytes_used` を新しい `base_boundary` として確定する。
7. 必要なら capacity を拡張(`ftruncate`)し、`fsync()` する。
8. 拡張後の capacity で Tier 2 ファイルを新しく `mmap` し、新しい世代の `T2Memory` を構築する(`base_boundary` = 手順 6 の値)。
9. T1 `reorganize()` を呼び出す。`offset_mapper` は record を一切リロケーションしないため、`payload_bits` を書き換えず、世代タグのみを手順 8 の新世代へ更新する。同じ呼び出しの中で、マージ済みの `sorted_region` を T1 checkpoint の一時ファイルへ書き出す(temp + `rename`、5.4 節)。
10. in-memory の新 `T2Memory` を、既存の reorganize と同じアトミックポインタスワップで公開する(旧世代は epoch based reclamation で安全に解放する)。書き込みスレッドを再開する。
11. manifest 一時ファイルに世代情報(`generation` = `checkpoint_lsn`)と `t2_bytes_used` を書き、`fsync` してから `rename()` でアトミックに正式パスへ差し替える。この瞬間に新世代が公式に有効化される。
12. WAL をローテーションする(5.5 節)。

このフローに stop-the-world は存在しない。手順 4-5 の writer stop は Tier 2 の生存集合を確定させるための短い barrier であり、手順 9 の T1 publish 完了までの間だけ新規書き込みをブロックする。それ以前(pre-stop パス)や以降(manifest commit・WAL rotate)は通常の並行書き込みと共存する。

**クラッシュ安全性**: 手順 3・5 の `pwrite()` は、いずれも `old_base_boundary` 以上の offset にのみ書き込む。この範囲は manifest がまだ古い(小さい)`t2_bytes_used` を指している間は「未コミット」として扱われるため、サイクルが手順 11 の `rename()` 完了前に中断しても、それまでの `pwrite()` は単に参照されない孤立バイトとして残るだけである。次の成功する checkpoint サイクルは同じ `old_base_boundary` から始まり、同じ offset へ改めて `pwrite()` するため、上書きされて安全に収束する。

**Hash Index Lifecycle in Checkpoint**

- `reorganize` 直後は `append_region` が空 (size = 0) となるため、T1 checkpoint file には `sorted_region` のデータのみがシーケンシャルに書き出され、`append_index` の状態自体はファイルへシリアライズ（永続化）しない。
- `reload` 時、新しくマッピングされた T1 インスタンスは、`append_index` を空に初期化した状態で起動し、その後の WAL リプレイおよび新規クライアント書き込み時に適宜ハッシュインデックスへの登録・更新を行う。

### 5.3 Correctness Rule

- checkpoint LSN の決定方法: `checkpoint_lsn = wal.next_lsn() - 1` を、5.2 節手順 4 の writer stop より**前**に 1 回読む。Insert / Update / Delete は T1 / T2 への適用完了後に WAL append する(3.2 節)ため、この読み取り時点で既に WAL LSN が採番済みの操作は、その T1/T2 適用も完了していることが保証される。
- checkpoint LSN を実際より低く見積もる(conservative に倒す)ことは許容される: 起動時の WAL tail replay が checkpoint に既に含まれるエントリを再度適用しても、`T1Index::put()` の overwrite-in-place 意味論により副作用がなく安全である(冪等)。
- checkpoint LSN を実際より高く見積もってはならない: それを行うと、checkpoint に反映されていない有効な更新を tail replay がスキップしてしまい、データロストになる。
- manifest が `rename()` で正式パスへ反映されるまで、その manifest が指す `t2_bytes_used` を超える範囲の Tier 2 ファイルの内容は、たとえ物理的に書き込まれていても信頼してはならない(上記クラッシュ安全性の節参照)。
- 起動時は、manifest が指す T1 checkpoint ファイルと Tier 2 checkpoint ファイルを読み込み(`t2_bytes_used` を境界として採用)、その後 checkpoint LSN の次の record から WAL の末尾までを replay する。manifest が存在しない、または読み込みに失敗する場合は、T2 を破棄し WAL 全体を LSN 1 から replay する(item 1 で実装済みのフォールバック挙動)。
- Tier 2 checkpoint ファイルが物理的に保持しているバイト数(ファイルサイズ)は、manifest の `t2_bytes_used` **以上**であればよい(capacity は生存データより先に拡張されることがあるため)。

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
- **ロード手順**: 起動時、manifest が指す世代の T1 chk ファイルを `mmap(MAP_PRIVATE)` し、header の magic / format_version / checksum を検証する。そこから `IndexEntry` 配列を 2 パスの O(N) コピーでランタイム表現へ変換する: (1) 各 `IndexEntry` を、この checkpoint が参照する T2 世代のタグを付けた `EntrySnapshot` に変換、(2) その `EntrySnapshot` 列から新しい `sorted_region`(`SortedSlot` 配列)を構築する。パースや個別のハッシュテーブル挿入ループ(1 entry ずつのハッシュ計算・衝突解決)は不要であり、これによって典型的な WAL 全量 replay や B-Tree 逐次挿入よりも大幅に軽い Fast Boot を実現する。
- **`append_index` はシリアライズしない**: 5.2 節の Hash Index Lifecycle の通り、checkpoint 直後は `append_region` が空であるため、ハッシュインデックス自体は永続化不要で、起動時に空の状態から再構築される。
- **runtime 表現との関係**: プロセス内で通常の(checkpoint を伴わない)T1-only reorganize が作る `sorted_region` は、従来通り heap 上の配列のままでよい。checkpoint 時にのみ、この配列の内容を上記フォーマットでファイルへコピーする。次回起動時の T1 chk 読み込みだけがこのファイルを直接 `mmap` して使う。同一プロセス内で checkpoint 直後にランタイム表現自体を mmap 領域へ切り替えるゼロコピー最適化は、今回はスコープ外とする。

### 5.5 WAL Rotation

WAL は先頭からの truncate を行わない(可変長レコード列の先頭を削るのは高コストなため)。代わりに、checkpoint 完了後にレコードを新しいファイルへ移し替える「ローテーション」を行う。

**Procedure**

`Wal::rotate(checkpoint_lsn)` は、`await_durable()` と同じリーダー選出(`flushing_` フラグのアトミック交換)でリーダー権を取り、以下を実行する。

1. まず `drain_pending()` を呼び、rotate 開始時点までに reserve 済みの未フラッシュレコードを通常どおり writev()+fdatasync() で吐き切る。
2. 現在の WAL ファイルから `lsn > checkpoint_lsn` のレコードのみを新しい一時ファイル(`<path>.rotate_tmp`)へ順次コピーする。
3. 新ファイルを `fsync` し、`rename()` で正式な WAL パスへアトミックに差し替える。旧 fd を close し、`Wal` の内部 `fd_` を新 fd へ切り替える。
4. リーダー権を解放する。解放直後に `next_lsn_` が rotate 開始時点から進んでいれば(rotate 実行中に新規 reserve が入っていれば)、リーダー権を再取得して `drain_pending()` をもう一度実行し、その分をフラッシュしてから最終的に解放する。

**Notes**

- レコードの reserve(LSN 発行 + リングへの publish)自体はリーダー選出と独立してロックフリーに進むため、rotate() 実行中も新規の reserve は一切ブロックされない。ブロックされ得るのは、rotate() がリーダーである間に `await_durable()` を呼んでフォロワーになったスレッドの durability wait だけである。
- コピー対象レコードは元の `lsn` をヘッダに保持したまま新ファイルへ入るため、`Wal` のコンストラクタが既に持つ「末尾の有効 record から `next_lsn_` を復元する」ロジックがそのまま機能する。
- `next_lsn_` は rotate() の中では一切変更しない: rotate 実行中に新たに reserve される LSN はまだディスク上に無くこのファイルベースのコピーからは見えないため、コピー結果から `next_lsn_` を再計算すると既に払い出し済みの LSN より後退しかねない。`next_lsn_` は常にインメモリのアトミックカウンタが真の値であり、rotation は古い生存レコードを新ファイルへ移すだけで誰の番号も振り直さない。
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

本節の最適化は 7.9 節を除きすべて opt-in であり、無効でも正しく動作する。7.9 節は base/tail split(2.2節)・in-place 更新の base 領域への強制迂回(3.3節)を前提とする常時有効の読み取り経路であり、無効化する経路は存在しない(7.7 節の `MADV_RANDOM` と同様の扱い)。

### 7.1 Group Commit（実装済み）/ Early Lock Release / Flush Pipelining（未実装・将来検討）

**Group Commit は実装済み。** `wal.hpp`/`wal.cpp`の`Wal`クラスを参照。ロックフリー（ホットパスに`std::mutex`を一切使わない）な固定長リングバッファ方式で実装されている。詳細は [Aether](https://dl.acm.org/doi/10.14778/1920841.1920928) の設計を参考にしたが、完全に同一の方式ではない：

- 各`append_*()`呼び出しは、単一のatomic `fetch_add`でLSNを確定し、その値をmod capacityしたスロット番号へ、ヒープ確保した自分のレコードへのポインタをCASで publish する（Aether論文そのものの「可変長バイト列を直接リングへ書き込む」方式ではなく、スロットにはポインタのみを置く簡易版）。
- 最初にスロットへの publish に成功し、かつ他に実行中のflushがない呼び出し元が"leader"として選出される（`reorg_running_`と同じCAS/atomic wait-notify方式、`std::mutex`は使わない）。leaderは自分を含む待機中の全レコードをドレインし、`writev()`（IOV_MAXごとにチャンク化）でまとめて書き込んだ後、バッチ全体に対して1回だけ`fdatasync()`する（`fsync()`ではない——データ復元に無関係なメタデータの同期を省く。ファイルサイズの同期はPOSIX上保証されるのでreplayに必要な分は失われない。RocksDBのWALと同じ選択）。
  - 個別レコード毎の`write()`ではなく`writev()`一括書き込みを採用している。失敗はバッチ全体を対象とする単一の共有catchブロックで扱うため、`writev()`化によって失敗セマンティクスの粒度は変わらない。
  - fsync完了後の起床は、レコード毎の`done`フラグ + 個別`notify_one()`ではなく、バッチの最高LSNを1つの共有カウンタ（`highest_settled_lsn_`）に書き込み`notify_all()`する方式を用いる（follower側は自分のLSN以下になるまで`wait()`）。レコード毎`notify_one()`はバッチサイズに対してループコストが超線形に増大する(batch=32では`fdatasync()`自体の約13倍のコストになる)ため、共有カウンタ方式でバッチ内の起床を1回のnotifyへ集約する。
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

### 7.4 T2 書き込み時の Chunk Allocation / Pre-faulting は不採用

各書き込みスレッドがT2領域へ`append`する際にスレッドごと大きなブロック(2MB)を一括予約し、直後に4KBページ単位でダミーライトして物理メモリページを一括割り当てさせる手法は、ページタッチ処理を`acquire_write_handle()`で取得したT2書き込みハンドル保持中に行うことになる。これは`checkpoint_internal()`の`stop_writers_and_wait()`(進行中のwriterが全員ハンドルを手放すまで待つdrain処理)を直接長引かせ、持続的な同時書き込み負荷下では`checkpoint_internal()`自体を2.7〜5.7倍遅くする。scanとinsertが混在するYCSB-Eワークロードでは、これによりscanスループットが最大16秒連続でゼロになる(詳細は`benchmark_results/pages/2026081711_charts.html`のYCSB-Eタブを参照)。唯一の恩恵(Zipf分布のホットな読み取りにおける初回page fault遅延の回避)は低スレッド数に偏っており、32スレッドの現実的な並行度ではほぼ消失する。

読み込み側の`base_mmap_scan`/`base_mmap_scan_seq`ウォームアップ(7.9節)は、書き込みハンドルとは無関係なタイミング・機構で実行されるため、この問題を持たず、常時有効である。

#### 7.4.1 T2 の Huge Page 化 (`MADV_HUGEPAGE`) は不採用

THP はスワップアウト時に 2MB 単位を保たず、512 個の 4KB ページに分割されてから個別にスワップされる。そのため、LTM Get_Hit のようにスワップが継続的に発生するシナリオでは、フォールト単位を 2MB に引き上げてもフォールト回数の削減には寄与しない。詳細は `implementation/docs/benchmark/20260805_t2_huge_page_investigation.md` を参照。

### 7.5 Sorted Bloom Filter

`sorted_region` 全体に Bloom filter を付与し、negative lookup を高速化できる。

### 7.6 WAL Rotation via `FALLOC_FL_COLLAPSE_RANGE`（未実装・将来検討）

5.5 節の WAL Rotation はレコードコピー方式(移植性重視)を基本とするが、対応ファイルシステム(ext4, xfs 等。tmpfs 等では非対応)では `fallocate(FALLOC_FL_COLLAPSE_RANGE)` によりファイル先頭のバイト範囲をコピー無しで直接除去できる。コピーを伴わないためローテーションの停止時間をさらに縮小できるが、Linux カーネル・ファイルシステム依存の機能であるため opt-in とする。

### 7.7 Tier 1 madvise(MADV_RANDOM) Optimization

T1インデックスに対して、仮想メモリマップ時のReadahead（カーネル先読み）を抑止しランダムアクセス性能を最適化する。
- **最適化の内容**: mmap領域のマップ直後に `madvise(..., MADV_RANDOM)` を呼び出し、OSカーネルの不要なページ先読み・カーネル空間メモリバス帯域の浪費を防ぐ。in-memory Get_Hit において約5%の性能向上をもたらす。常時有効であり、無効化する経路は存在しない。

### 7.8 Scan の io_uring 並列プリフェッチは不採用

`madvise(MADV_POPULATE_READ)` によるページキャッシュ温めは所有権のあるコピーを伴わないため、cgroup の継続的な回収圧力下では読み取り前に再度追い出される(prefetch-then-evict)。Scan の高速化は 7.9 節の base 専用 mmap(所有権付きの実データ読み取り)によって行う。詳細は `implementation/docs/benchmark/20260806_scan_madvise_tradeoff.md` を参照。

### 7.9 Scan/Get の base 領域専用読み取り経路

T2 の「base」領域(2.2節)は書き込み後二度と変更されないため、主 mmap の `MADV_RANDOM`(7.7節)を経由しない専用の読み取り経路から直接読み取ることができ、seqlockによる再試行は不要になる(3.3節の in-place 更新の base 領域への強制迂回がこの不変性を保証する)。

- **前提となる T2 の変更**: base/tail split(2.2節)と、in-place 更新の base 領域への強制迂回(3.3節)。
- **3つの読み取り経路を、レコードごとにそのレコード自身の長さ(2.1節の Embedded Block Count)で選ぶ**。madvise は VMA(マッピング全体)単位の属性であり、個々の読み取り単位のものではないため、小さいレコードと大きいレコードの両方にうまく対応するには複数の経路が要る。どの経路を選んでも指す先は同一のバイト列なので、選択を誤ってもreadahead方針のミスマッチにしかならず、データが誤ることはない:
  - `base_mmap_scan_seq`(read-only mmap、`MADV_SEQUENTIAL`): 埋め込みサイズヒントが1ページ以下のレコード用。広い先読み窓で多数の小さいレコードのフォルトを少数の major fault にまとめられる。`scan_impl()`・`get_impl()`双方の小レコード読み取りで使う。
  - `base_mmap_scan`(read-only mmap、カーネルのデフォルト(適応的)readahead方針): それより大きいレコード用。`scan_impl()`の大レコード読み取りと、`get_impl()`の大レコード読み取りのうちページキャッシュ常駐が確認できた場合(`mincore()`)に使う。無条件に`MADV_SEQUENTIAL`を付けると、大きいレコードのコーパスをZipfのような偏ったアクセスで読む場合に読み取りバイト数が余分に増えることが測定で判明したため、こちらは意図的に控えめな方針にしてある。
  - `read_fd`(`dup()`したファイルディスクリプタ経由の`pread()`): `get_impl()`の大レコード読み取りで、上記のページキャッシュ常駐確認が取れなかった場合に、そのレコード1つぶんにサイズを絞って読む。
- **マッピング作成時のウォームアップ**: `base_mmap_scan`・`base_mmap_scan_seq`の両方を、作成時に `MAP_POPULATE` 相当で即座にウォームアップする(reorganize直後の初回アクセスがコールドフォルトの嵐で不安定になるのを防ぐ)。1KB In-Memory Get/Hit/Uniformで約250倍の退行を防ぐ効果があり、常時有効。
- **測定方法・結果**: `implementation/docs/benchmark/20260807_scan_t2_base_tail_io_uring_read.md`(base/tail split と実データ読み取りの元設計)、`implementation/docs/benchmark/20260810_t2_no_madvise_random.md`を参照。`Scan` は他の Op と共通のマスターコーパスを使用する。

## 8. Parameters

| Parameter | Meaning |
| --- | --- |
| `T1_MAX_INDEX_SIZE` | Tier 1 最大 entry 数 |
| `T1_REORGANIZE_SOFT_THRESHOLD` | T1 Reorganize を非同期実行するソフトしきい値（スキャン有無で動的変化） |
| `T1_REORGANIZE_HARD_THRESHOLD` | 新規書き込みをブロッキング（Stall）するハードしきい値（通常95%固定） |
| `T2_MAX_VIRTUAL_MEMORY_SIZE` | Tier 2 最大仮想アドレス空間 |
| `WAL_MAX_BYTES_SINCE_CHECKPOINT` | 直前 checkpoint 以降に許容する WAL 蓄積バイト数の上限。超過で checkpoint (T2 Reorganize) へ昇格する |

