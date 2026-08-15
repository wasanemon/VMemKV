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

`T2Store` は `base_boundary` を保持する: `[0, base_boundary)` の範囲が、baseリージョン専用の読み取り経路(7.9節 -- 用途・レコードサイズ別に3つある)経由で seqlock なしに安全に読める、という不変条件を表す境界値である。`base_boundary` は full reorganize(4.3節)完了時にその時点の `bytes_used` へスナップショットされるのに加えて、**5.2bis 節の T1-only checkpoint(T2 再構築を伴わない安いパス)によっても、新たに耐久化された差分バイト数ぶんインクリメンタルに伸長される** -- これらの mmap 自体は毎回張り直さず(`capacity` 分あらかじめ確保済み)、有効とみなす範囲(`base_boundary` の値)だけが伸びる。これにより、insert-only ワークロード(4.4 節、空間増幅率が常に低くフル reorganize が自然発火しない)でも、カバレッジが最初の1回で凍結されず、コーパスの成長に追従できる。

- `offset < base_boundary` の record を **base** 領域、`offset >= base_boundary` の record を **tail** 領域と呼ぶ。
- base 領域は、一度 `base_boundary` に含まれた後は二度と in-place では書き換えられない(3.3節)。そのため base 領域は、mmap の `MADV_RANDOM`(7.7節)を経由しない専用の読み取り経路で読んでも安全であり、seqlock も不要になる(経路の選び方は7.9節)。
- tail 領域は通常通り in-place 更新の対象になり得るため、主 mmap 経由(COW反映済みの最新ビュー、seqlock保護)でしか安全に読めない。
- `base_boundary` とは別に、`write_gate_boundary` という内部フィールドも保持する: in-place 更新を禁止する境界(3.3節)であり、常に `base_boundary` と同じか、それより先に(=より早いタイミングで)伸長される。両者を分けるのは、「新規の in-place 更新を締め出す」タイミングと「読み取りを許可する」タイミングが、T1-only checkpoint の差分 flush を挟んで異なる必要があるため -- 詳細と正しい順序は 5.2bis 節。

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
- 手順3の in-place 判定には `offset >= write_gate_boundary`(2.2節、`base_boundary` ではなく)の条件も含まれる。この境界未満を指す record への更新は、たとえ `new_value_len <= alloc_len` でも in-place にはせず、手順4の追記パスに強制的に回す。base 領域は専用mmapで直接読む読み取り経路(7.9節)の前提として「二度と書き換わらない」ことに依存しているため。`write_gate_boundary` を見るのは、T1-only checkpoint の昇格処理(5.2bis節)が `base_boundary` を実際に伸ばす*前*にこの境界を先に締める必要があるため -- 昇格処理が完了していない一瞬の間も、この判定は常に正しい側(より安全な側)を向く。

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

#### T2 Reorganize と Checkpoint、独立した 2 つの判定式

LTM 規模の生存コーパスに対する T2 の物理デフラグ・GC は、`WAL_MAX_BYTES_SINCE_CHECKPOINT` が要求する checkpoint 間隔よりも大幅に時間がかかり、持続的な書き込み負荷下では追いつかない。「WAL を切り詰めるために checkpoint したい」という要求と「T2 が断片化しているので GC したい」という要求は本質的に独立であり、T1-only reorganize は T2 の offset を一切動かさないため、後者が成立しない限り T2 を作り直す必要はない。そこで判定式を独立した 2 つに分離する。

$$\text{T2\_Rebuild\_Trigger} = A \ge A_{\text{limit}} \lor \text{Force}$$
$$\text{Checkpoint\_Trigger} = \text{T2\_Rebuild\_Trigger} \lor \text{WAL\_Bytes\_Since\_Checkpoint} \ge \text{WAL\_MAX\_BYTES\_SINCE\_CHECKPOINT}$$

* **$A_{\text{limit}}$ (空間増幅率上限):** `1.3` (Storage Fragmentation $\ge 30\%$ 相当。RocksDB 等の標準である空間増幅率上限に基づき定義される)。
* **`WAL_MAX_BYTES_SINCE_CHECKPOINT`**: 直前 checkpoint の checkpoint LSN 以降に WAL へ append されたバイト数の上限。Checkpoint はこのサイズベースのトリガーのみで判定し、書き込みレートに関わらず起動時の WAL replay 時間を有界に保つ。
* **`Force`**: `defragment()` の明示呼び出し、あるいは参照可能な T2 checkpoint ファイルがまだ存在しない状態での `checkpoint()` 呼び出し(初回、`no_t2_checkpoint_yet`)。
* この 2 つの判定式自体は単一の内部関数の分岐にはせず、公開 API を **`reorganize()`**(T1-only インメモリマージ、T2 に触れず checkpoint もしない)・**`defragment()`**(常に T2 をフル再構築(GC)し checkpoint を永続化する)・**`checkpoint()`**(参照可能な T2 checkpoint が無ければ `defragment()` 相当、あれば T1 マージ + T2 差分 pwrite/fsync という最も安いパスで checkpoint を永続化する)の 3 つに分割する。しきい値の評価とどのメソッドを呼ぶかの決定は、呼び出し側(バックグラウンドワーカー `reorg_worker_loop()` および起動時 `recover_from_wal()`)の責務とする。判定結果は 3 通り: **(1)** `T2_Rebuild_Trigger` 成立 -- `defragment()`(GC + checkpoint、4.3 節・5 章)。**(2)** `T2_Rebuild_Trigger` 不成立かつ `Checkpoint_Trigger` 成立 -- `checkpoint()` の安いパス、すなわち **T1-only checkpoint**(5.2 節後半)、T2 は再構築せず既存の checkpoint ファイルを参照したまま checkpoint のみ行う。**(3)** どちらも不成立 -- `reorganize()`(T1-only、checkpoint なし)。

| API | do_t2_rebuild | do_checkpoint | 目的 |
|---|---|---|---|
| `reorganize()` | false | false | T1 の Append→Sorted マージのみ。T2/ディスク非関与 |
| `checkpoint()` | 既存 T2 checkpoint の有無で分岐 | true | 耐久性の確保。安いパス優先、初回のみフルリビルド |
| `defragment()` | true | true | T2 の断片化解消(GC)。常にフルリビルド |
* **初期挿入 (Insert-only) ワークロード:** ゴミデータが発生しないため $A$ は常に $1.0 < 1.3$ に保たれ、`WAL_MAX_BYTES_SINCE_CHECKPOINT` 到達は(初回を除き)常に (2) `checkpoint()` の T1-only パスに帰着する。

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

`checkpoint reload` は次の手順で行う。新世代の T2/T1 構築は `fork` を用いず同一プロセス内で行う(5.1 節の理由により、通常の `write()` によるシーケンシャル書き出し + 再 `mmap` で構築するため、別アドレス空間を用意する必要がない)。

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

#### 5.2bis T1-only Checkpoint Flow (T2 再構築なし)

4.4 節 (2) の判定(`T2_Rebuild_Trigger` 不成立、`Checkpoint_Trigger` 成立)に対応する軽量フロー。公開 API の `checkpoint()` メソッドが、参照可能な T2 checkpoint ファイルが既に存在する場合に選ぶパスがこれである(存在しなければ `defragment()` 相当のフルリビルドに委譲する)。T1 の merge 自体は上記手順 1〜3 と同じだが、手順 4 の T2 全体書き直しを行わず、既存の T2 checkpoint ファイルを引き続き参照する。

1. 上記手順 1〜3 と同様(`checkpoint_lsn` の確定、T1 の凍結・merge、T1 checkpoint 一時ファイルへの書き出し)。ただし世代番号は T1 側専用のもの(`t1_generation = checkpoint_lsn`)として確定し、T2 checkpoint ファイルの世代番号(`t2_generation`)は直前の値のまま変えない(manifest の世代番号を T1/T2 で分離する理由、後述)。
2. **T2 差分の耐久化(このフロー固有の手順)**: T2 の稼働中 mmap は `MAP_PRIVATE`(5.1 節の NOTE参照、書き込みはプロセス内 COW ページに留まりファイルへは反映されない)であるため、直前の T2 checkpoint(フルリビルド、または以前の T1-only checkpoint の差分フラッシュ)以降に通常の Insert / Update が T2 へ書き込んだバイトは、まだディスク上のどこにも存在しない。これらを WAL に頼らず失わずに済ませるには、このタイミングで実際にディスクへ書く必要がある。直前に耐久化済みのバイト数(`t2_durable_bytes_`)から現在の `bytes_used` までの範囲を、稼働中 mmap 上の該当バイト列から `pwrite()` で **同じ T2 checkpoint ファイルの同じオフセット** へ追記し、`fsync()` する(この範囲より前のバイトは T1-only checkpoint では一切移動しないため、そのままで良い)。コストは O(直前の checkpoint 以降に増えた T2 バイト数) であり、O(生存コーパス全体) のフルリビルドより大幅に安いが、ゼロではない。
   - **`base_boundary` のインクリメンタル昇格(2.2節)**。この手順の `pwrite()`/`fsync()` を挟んで、以下の順序を厳守する:
     1. まず `write_gate_boundary` を今回の昇格対象範囲の上端(= この手順で `pwrite()` する `bytes_used`)へ bump する。この時点で、新たにこの範囲を対象とする in-place 更新(3.3節)は締め出される。ただし `base_boundary` はまだ動かさないため、読み取り側(base領域用の読み取り専用mmap群、2.2節/7.9節)には一切影響しない。
     2. 上記の締め出しより*前*に、既に古い `write_gate_boundary` を見て in-place 書き込み中だったスレッドが残っていないことを保証する必要がある。専用の epoch ベースの reference tracker(`update_epoch_tracker_`)を使い、epoch をひとつ進めたうえで、当該 in-place 更新のクリティカルセクションが epoch 登録済みだった全スレッドの完了を待つ。`T2FlatFile::stop_writers_and_wait()`(6章)は新規 append だけをゲートする別機構であり、in-place 更新はこの待機の対象外なので流用できない。
     3. この待機が完了して初めて、対象範囲は「二度と in-place で変わらない」ことが確定する。ここで初めて上記の `pwrite()`/`fsync()` を実行する。
     4. `pwrite()`/`fsync()` が成功した後(耐久化完了後)、初めて `base_boundary` を同じ値まで bump して公開する。読み取り可否の公開を書き込みの締め出しより*後*に行うのは、base領域用の読み取り専用mmap群は別 fd 経由の書き込みも透過的に反映する(7.9節、`MAP_PRIVATE` かつ一度も書き込まれないマッピングは COW が発動しないため)ので、`base_boundary` を先に公開してしまうと、まだディスクに届いていないバイトを読者に見せてしまう(stale read)ためである。
   - これらのmmap自体は毎回張り直さない(2.2節)ので、この昇格はこの手順の外側にある通常の delta flush と同程度のコストで完結する -- 新たな一時ファイルの作成や `swap_memory()` は発生しない。
3. manifest 一時ファイルへ `t1_generation`(新)・`t2_generation`(不変)・耐久化後の `t2_bytes_used` を書き、`fsync` し、`rename()` でアトミックに正式パスへ差し替える。
4. WAL をローテーションする(5.5 節)。
5. 不要になった旧世代の T1 checkpoint ファイルのみ削除する。**T2 checkpoint ファイルは削除しない**(複数サイクルにわたって参照され続けるため)。

T1/T2 で世代番号を分離する理由: 従来は 1 つの世代番号が T1 checkpoint ファイル名・T2 checkpoint ファイル名・checkpoint LSN の 3 役を兼ねていたが、T1-only checkpoint は T1 側だけを新しくして T2 側は据え置くため、この 2 つが分岐しうる。manifest は `t1_generation` と `t2_generation` を独立したフィールドとして持つ(5.4 節・checkpoint.hpp 参照)。

### 5.3 Correctness Rule

- checkpoint LSN の決定方法: `t1.reorganize()` (手順 1 の凍結)を呼び出す**前**に `checkpoint_lsn = wal.next_lsn() - 1` を 1 回読む。Insert / Update / Delete は T1 / T2 への適用完了後に WAL append する(3.2 節)ため、この読み取り時点で既に WAL LSN が採番済みの操作は、その T1 適用も凍結前に完了していることが保証され、必ず凍結対象(旧世代)に含まれる。
- checkpoint LSN は、手順 1 の凍結(atomic pointer swap)が完了した時点で WAL へ fsync 済みであることが確定している LSN の最大値以下でなければならない。
- checkpoint LSN を実際より低く見積もる(conservative に倒す)ことは許容される: 起動時の WAL tail replay が checkpoint に既に含まれるエントリを再度適用しても、`T1Index::put()` の overwrite-in-place 意味論により副作用がなく安全である(冪等)。
- checkpoint LSN を実際より高く見積もってはならない: それを行うと、checkpoint に反映されていない有効な更新を tail replay がスキップしてしまい、データロストになる。
- 新しい manifest が `rename()` で正式パスへ反映されるまで、旧世代の T1 checkpoint ファイル・T2 ファイル・WAL は削除してはならない。T1 checkpoint ファイルは checkpoint の度に必ず新しくなるため常に旧世代を削除してよいが、T2 checkpoint ファイルは T1-only checkpoint(5.2bis 節)が複数サイクルにわたって同じファイルを参照し続けうるため、`t2_generation` が実際に変わった(= このサイクルで T2 を再構築した)場合にのみ削除してよい。
- 起動時は、manifest が指す `t1_generation` の T1 checkpoint ファイルと `t2_generation` の T2 checkpoint ファイルを読み込み、その後 checkpoint LSN の次の record から WAL の末尾までを replay する。manifest が存在しない、または読み込みに失敗する場合は、T2 を破棄し WAL 全体を LSN 1 から replay する(item 1 で実装済みのフォールバック挙動)。
- **T1-only checkpoint 固有の制約(5.2bis 節)**: T2 の稼働中 mmap が `MAP_PRIVATE` であるため、manifest が指す `t2_generation` の checkpoint ファイルが物理的に保持しているバイト数と、manifest の `t2_bytes_used` は常に一致していなければならない。両者を一致させる責務は、T1-only checkpoint が commit する直前に行う差分の `pwrite()` + `fsync()`(5.2bis 節手順 2)にあり、これを怠ると、WAL ローテーション後に該当 T2 バイトを復元する手段が失われ、リスタート後のデータロストになる。

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

### 7.4 Chunk Allocation / Pre-faulting (T2 Lock Mitigation)

マルチコア高並列書き込み環境における Linux カーネルの仮想メモリページフォールトおよび `mmap_lock`（VMAロック）の競合によるオーバーヘッド（Cache Line Bouncing）を緩和するための最適化である。

* **スレッドローカル Chunk アロケーション**: 
  各書き込みスレッドが T2 領域へ `append` する際、アトミックカウンタから個別に領域をアロケートするのではなく、スレッドごとに大きなブロック（例: 2MB）を一括予約する。
* **一括 Pre-faulting**:
  Chunk 確保直後に、その 2MB 領域を 4KB ページ単位（標準ページサイズ）でダミーライトし、カーネルに物理メモリページを一括で割り当てさせる。
  これにより、その後の個々の `insert`（`memcpy`）においてはページフォールトの発生が **1/500** に激減し、OS カーネルの `mmap_lock` に入ることなく完全に並行して超高速にメモリコピーを実行できるようになる。
* **コンパイル時解決の徹底**:
  最適化のオーバーヘッドをゼロにするため、`vmemkv::Config` のテンプレート引数タグ（`Prefaulting`）を介して `constexpr if` によってコンパイル時に分岐とコード生成が制御される。

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
- **Prefaulting との連携**: `Prefaulting`(7.4節、値のinsert時プリフォルト最適化)が同時に有効な場合、`base_mmap_scan`・`base_mmap_scan_seq`の両方が `MAP_POPULATE` 相当で作成時に即座にウォームアップされる(reorganize直後の初回アクセスがコールドフォルトの嵐で不安定になるのを防ぐ)。
- **測定方法・結果**: `implementation/docs/benchmark/20260807_scan_t2_base_tail_io_uring_read.md`(base/tail split と実データ読み取りの元設計)、`implementation/docs/benchmark/20260810_t2_no_madvise_random.md`を参照。`Scan` は他の Op と共通のマスターコーパスを使用する。

## 8. Parameters

| Parameter | Meaning |
| --- | --- |
| `T1_MAX_INDEX_SIZE` | Tier 1 最大 entry 数 |
| `T1_REORGANIZE_SOFT_THRESHOLD` | T1 Reorganize を非同期実行するソフトしきい値（スキャン有無で動的変化） |
| `T1_REORGANIZE_HARD_THRESHOLD` | 新規書き込みをブロッキング（Stall）するハードしきい値（通常95%固定） |
| `T2_MAX_VIRTUAL_MEMORY_SIZE` | Tier 2 最大仮想アドレス空間 |
| `WAL_MAX_BYTES_SINCE_CHECKPOINT` | 直前 checkpoint 以降に許容する WAL 蓄積バイト数の上限。超過で checkpoint (T2 Reorganize) へ昇格する |
| `T2_STORAGE_FRAGMENTATION_THRESHOLD` | Tier 2 storage fragmentation の閾値 |

