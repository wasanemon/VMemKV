# 2026-07-23 Benchmark Result Anomaly Investigation

## 1. Scope

`TODO.md` の Item 6「現在のベンチマーク結果の怪しい点の調査と修正」に基づき、
`benchmark_results/2026071600/` の計測結果に対して指摘された以下 3 点を、ベンチマークコード
(`implementation/benchmark/bench_kv.cpp`, `implementation/src/rivals/rocksdb_store.hpp`) と
実際の JSON 結果を突き合わせて調査した。

1. LTM(1KB) の Get/Hit(Zipf) だけ Baseline が RocksDB に負けている
2. Scan-only (no reorganize) の結果がプロットされておらず、実験自体が行われたか不明
3. LTM(64KB) の GET/HIT 系、YCSB-E 系の VMemKV/RocksDB 倍率が異様に高い

## 2. Executive Summary

| # | 症状 | 分類 | 原因 |
| --- | --- | --- | --- |
| 1 | LTM(1KB) Get/Hit(Zipf) threads:1 のみ Baseline が RocksDB に負ける (0.71x) | ベンチマーク手法上の限界（未確証・追加計測は今回見送り） | 単発計測（repetitions=1）のノイズと LTM 特有の swap 待ち時間の可能性が高いが、コードレビューだけでは確証が持てない |
| 2 | Scan-only (no reorganize) が存在しない | 未実装 → **実装済み** | 該当ベンチマークがコード上に存在しなかった（TODO Item 5 未着手）。`Scan/Mode=NoReorg` `Scan/Mode=T1Reorg` `Scan/Mode=T1T2Reorg` の 3 系統として追加した |
| 3-a | LTM(64KB) Get/Hit が RocksDB の 325 倍高速 | **バグ（修正済み）** | Get のベンチマークコールバックが値の中身を一切読まず、`std::span` の参照だけを `DoNotOptimize` している。64KB のような複数ページ値では、実際にページイン（swap-in）されるのは先頭ページだけになる |
| 3-b | LTM(64KB) の YCSB-E scan 系スループットが RocksDB に対して不自然に高倍率（〜56x） | **仕様からの乖離（修正済み）** | `scan()` の公開 API 自体が値を `uint64_t`（先頭 8 バイト）にしか渡さない実装になっていた。HLD/LLD 仕様（§5-b 参照）は Tier2 の value record をそのまま返すことを求めており、この 8 バイト丸めは仕様通りではなかった |
| 3-c | YCSB-E の `results_*.json` の `real_time` / `items_per_second` が常に `0` / `Infinity` | **バグ（修正済み）** | `for (auto _ : state)` を使わない独自 30 秒ループのため、google benchmark 側のタイマーが一度も開始されていなかった |
| 3-d | `ScanReorg` の 1KB / 64KB ラベルの結果がほぼ同一 | **バグ（修正済み）** | `populate()` 呼び出しが `val_size` を無視し、常に `kInlineValueBytes`（8B）でデータ投入していた |

本調査は 2 段階で行った。まず 3-c, 3-d をベンチマークハーネスの明確なバグとして即修正し（§5）、
3-a・3-b・Point 1・Point 2 は「何を計測するか」「本番 API を変えるか」という意思決定が必要な項目
としてユーザーに確認した。結果、3-a・3-b・Point 2 は対応する方針で合意が取れたため実装し
（§6）、Point 1 のみ「今回は追加計測を行わない」という判断になった（§7）。

## 3. Point 1: LTM(1KB) Get/Hit(Zipf) で Baseline が RocksDB に負ける

`results_ltm_1KB.json` の `Op=Get/Mode=Hit/Dist=Zipf/Value=1KB` を全 thread 数で見ると次の通り。

| threads | RocksDB (ops/s) | VMemKV/Baseline (ops/s) | Baseline/RocksDB |
| ---: | ---: | ---: | ---: |
| 1 | 70,625 | 50,324 | **0.71x**（唯一の逆転） |
| 4 | 172,202 | 215,223 | 1.25x |
| 16 | 600,764 | 552,349 | 0.92x |
| 32 | 939,447 | 867,686 | 0.92x |

逆転しているのは `threads:1` のみで、しかも他の value size（8B, 64KB）では同じ `threads:1` でも
Baseline が RocksDB を大きく上回っている（8B: 2.6x, 64KB: 325x、後述の通り 64KB 側は別の意味で
異常値だが、少なくとも Baseline が RocksDB に負けてはいない）。

生ログを見ると `real_time` と `cpu_time` に大きな差がある。

| Variant | real_time (ns/op) | cpu_time (ns/op) | iowait 相当割合 |
| --- | ---: | ---: | ---: |
| RocksDB | 14,159 | 5,043 | 64% |
| VMemKV/Baseline | 19,871 | 2,739 | **86%** |

つまり両者とも、計測時間の大半をブロッキング待ち（LTM 環境下でのページフォールト/swap-in）に
費やしており、CPU 時間だけで見ると VMemKV/Baseline の方がむしろ RocksDB より短い
（2,739ns vs 5,043ns）。逆転は「待ち時間」側で発生している。

### 調査した仮説と評価

- **単発計測のノイズ**: このベンチ一式は `--benchmark_repetitions` を指定しておらず
  （`run_bench.sh` 参照）、各セルは 1 回の試行のみで標準偏差が取れない。LTM 環境下の swap I/O
  レイテンシは実行毎のブレが大きいため、0.71x という比較的小さな逆転（他の異常値は 300 倍オーダー）
  は誤差の範囲内である可能性が高い。
- **`crud_holder` の使い回しによる履歴依存**: `Get/Hit(Zipf)`, `Get/Hit(Uniform)`, `Get/Miss`,
  `Update`, `YCSB-E` は同一の `crud_holder`（同一 store インスタンス）を `reuse_store=true` で
  共有しており（`bench_kv.cpp:791-792, 839-907`）、実行順序によってページキャッシュ/swap の
  ウォームアップ状態が後続ベンチマークに引き継がれる。`Get/Hit(Zipf)` は登録順で最初に走る
  read-only ベンチマークだが、直前の `Insert` ベンチマーク（別 holder だが同一プロセス・同一
  cgroup メモリ予算を共有）が残した swap 圧の影響を受けている可能性がある。
- **コード上の構造的な原因は見つからなかった**: `get_impl`（`vmemkv_impl.hpp:386-417`）は
  Baseline を含めどの variant でも同一の経路を通っており、1KB という value size に固有の分岐は
  存在しない。したがって「Baseline のコードパスが 1KB でだけ遅い」ことを示す静的な根拠はない。

### 結論と推奨アクション

現時点では **再現性未確認の測定ノイズ** の可能性が最も高く、コードレビューだけでは断定できない。
次のいずれかで再検証することを推奨する。

1. `--benchmark_repetitions=5` 以上を付けて該当セルのみ再計測し、中央値・分散を確認する。
2. `getrusage(RUSAGE_THREAD)` の `ru_minflt` / `ru_majflt`（マイナー/メジャーページフォールト数）を
   ベンチマークの `state.counters` に追加し、Baseline と RocksDB で実際に発生したページフォールト数を
   直接比較する。
3. `Get/Hit` 系ベンチマークを `reuse_store=false` にして他ベンチマークの影響を切り離した上で再計測する。

## 4. Point 2: Scan-only (no reorganize) が見つからない（**実装済み**）

調査時点では、`bench_kv.cpp` 内に reorganize を行わない scan 専用ベンチマークは存在しなかった。
登録されていたのは `ScanReorg` のみで、populate 直後に必ず `store.reorganize()`（T1+T2 フル
reorganize）を呼んでから計測しており、**T1+T2 まで完全に reorganize された状態の scan** しか
存在しなかった。これは `TODO.md` Item 5「ベンチマーク案の追加」が未着手だったことに起因する。

### 実装内容

`bench_kv.cpp` の `ScanReorg` 登録ブロックを、reorganize 度合いの異なる 3 シナリオのループに
一般化した。

| ラベル (Op=Scan/Mode=...) | populate 後の処理 | 相当する TODO Item 5 の項目 |
| --- | --- | --- |
| `NoReorg` | populate のみ（追加の reorganize 呼び出しなし） | Scan (Not reorganized) |
| `T1Reorg` | `store.reorganize(false)`（T1 のみ: append_region を sorted_region にマージするが T2 の物理 GC はスキップ） | Scan (T1 only reorganized) |
| `T1T2Reorg` | `store.reorganize(true)`（T1+T2 フル reorganize。旧 `ScanReorg` と同じ処理） | Scan (T1+T2 reorganized)（旧 `ScanReorg` の改称） |

`force_t2_gc` フラグは元々 `VMemKVImpl::reorganize(bool)` にしか存在せず、公開 API
`StoreAdapter::reorganize()` は引数なしのオーバーロードしか持たなかったため、
`StoreAdapter::reorganize(bool force_t2_gc)` を追加した（`store_adapter.hpp`）。RocksDB には
T1/T2 の区別が存在しない（自動 compaction のみ）ため、`StoreAdapter<RocksDBStore>` 側では
`if constexpr` でこの引数を無視する no-op として実装している。

RocksDB 自体は 3 シナリオとも同一の populate のみ（reorganize 呼び出しなし）で計測される
（既存の "RocksDB は自動 compaction のため手動 reorganize 概念がない" という前提を維持）。

小規模スモークテストで `Reorgs_T1` / `Reorgs_T2` カウンタを確認し、`NoReorg`=(0,0),
`T1Reorg`=(1,0), `T1T2Reorg`=(1,1) と、意図通りの reorganize 回数になっていることを確認済み。

## 5. Point 3: LTM(64KB) の GET/HIT・YCSB-E 系の倍率異常

### 5-a. Get/Hit: 値のバイト列を一切読んでいない（**修正済み**）

`Get/Hit` のベンチマークコールバックは次の通り(`bench_kv.cpp:851-852`)。

```cpp
store.get(make_key(key_index),
          [](std::span<const std::byte> value) { benchmark::DoNotOptimize(value); });
```

`benchmark::DoNotOptimize(value)` は `value`（`std::span` = ポインタ + 長さの組）というオブジェクト
自体をコンパイラに最適化除去させないためのものであり、**span が指すバイト列を実際に読み込む
保証はない**。`get_impl`（`vmemkv_impl.hpp:386-417`）の内部でも、キー一致判定
（`byte_span_equal(record.key, full_key)`）はキー部分しか触れず、値のメモリは
コールバックに `record.value` を渡す時点でも dereference されない。

これがなぜ 1KB では顕在化せず 64KB でだけ顕在化するかというと、T2 のレコードは
`header + key + value` が連続領域に配置されており、キー比較のために読む先頭ページには
1KB 程度の value ならほぼ全体が収まる。一方 64KB は 4KB ページで 16 ページ分に及ぶため、
先頭ページ（キー比較で touch される）以外の 15 ページは **swap から一度もページインされない**。

実データで確認すると (`results_ltm_*.json`, `threads:1`, `Get/Hit/Zipf`):

| Value size | RocksDB (ops/s) | VMemKV/Baseline (ops/s) | 倍率 |
| --- | ---: | ---: | ---: |
| 8B | 623,192 | 1,636,789 | 2.6x |
| 1KB | 70,625 | 50,324 | 0.7x |
| **64KB** | 14,690 | **4,769,372** | **325x** |

値サイズが大きくなるほど本来は相対的に遅くなるはず（LTM で大きい値ほど swap I/O が支配的になる）
にもかかわらず、64KB でだけ RocksDB比 325 倍という非単調な挙動になっている。これは
実際の値取得コストを計測できていないことの明確な証拠である。

RocksDB 側 (`rocksdb_store.hpp:72-82`) は `PinnableSlice` に値を実体化するため、block cache から
実際にブロックを読む（≒ 64KB を丸ごと読む）コストを払っている。したがって、この比較は
「VMemKV は値を読まずに済ませている／RocksDB は読んでいる」という不公平な比較になっている。

**修正内容**: `bench_kv.cpp` に `touch_bytes(std::span<const std::byte>)` ヘルパーを追加し、値の
全バイトを畳み込んでチェックサムを計算するようにした（`checksum = checksum * 131 + byte`）。
`Get/Hit` のコールバックを `benchmark::DoNotOptimize(value)` から
`benchmark::DoNotOptimize(touch_bytes(value))` に変更し、値の全ページが実際に読み込まれる
（LTM 環境なら必要に応じて swap-in される）ようにした。同じ関数を YCSB-E / Scan 系の
コールバックにも適用している（§5-b）。

### 5-b. Scan: `scan()` API 自体が値を 8 バイトの `uint64_t` に丸めている（**修正済み**）

`scan_impl`（`vmemkv_impl.hpp:512-521`）は T2 レコードに対して次のようにコールバックする。

```cpp
callback(record.key, decode_u64(record.value));
```

`decode_u64` は先頭 8 バイトだけを `uint64_t` に変換するヘルパーであり、これは公開 API
`StoreAdapter::scan()`（`store_adapter.hpp:104-114`）のシグネチャそのものに起因する
（コールバックは `(key, uint64_t value)` を受け取る設計）。つまりこれは
**ベンチマークの書き方の問題ではなく、`scan()` という公開 API の意味論そのもの**である。

RocksDB 側の rival 実装（`rocksdb_store.hpp:151-173`）も同様に先頭 8 バイトしかデコードしないが、
`iterator->value()` の呼び出し自体が RocksDB 内部でブロックの実体化（block cache 読み込み、
必要なら disk read）を強制するため、RocksDB は依然として値サイズに比例したコストを払う。
一方 VMemKV は mmap 経由で先頭 8 バイトしか touch しないため、値の残りは swap から一切
読み込まれない。

`ycsb_e_timeline_*.json`（95% が scan の workload）の定常状態（後半 10 秒平均）で確認すると：

| Value size | VMemKV/Baseline scan/s | RocksDB scan/s | 倍率 |
| --- | ---: | ---: | ---: |
| 8B | 97,347 | 30,706 | 3.2x |
| 1KB | 90,436 | 30,236 | 3.0x |
| **64KB** | 76,165 (-22%) | 1,349 (**-95.6%**) | **56.5x** |

VMemKV は 8B→64KB で値サイズが 8,000 倍になってもスループットは 22% しか落ちないのに対し、
RocksDB は 95.6% 低下している。これは RocksDB が実際に 64KB を読んでいる／VMemKV が
読んでいないことの直接証拠であり、倍率が 3x 前後から 56x まで跳ね上がる主因である。

**仕様との照合**: `docs/specification/high_level_design.md:118` は「Scan: まず Tier 1 で範囲を
絞り込み、得られた offset 集合を使って Tier 2 で **value records を収集して返す**」と明記しており、
`low_level_design.md:194-195` の Scan procedure も「`payload_bits` から Tier 2 record を取得する」
「full key を比較し、範囲内の **record** だけを返す」としている（index-level covering、つまり
T1 に inline された小さい値だけが例外的に Tier1 payload のみで完結してよいとされる）。したがって
現行の「全レコードを問答無用で 8 バイトに丸める」実装は、非 inline（Tier2 バック）のレコードに
関しては **仕様からの乖離** であり、意図した設計ではなかったと判断した。

**修正内容**: `VMemKVImpl::scan_impl`（`vmemkv_impl.hpp`）のコールバックを、T1 inline
covering されたエントリでは値バイト列を保持するスタックバッファ（`get_impl` の inline 分岐と
同じパターン）に、T2 バックのレコードでは `record.value`（`decode_u64` で丸めず、フルサイズの
`std::span`）に、それぞれ変更した。T2 分岐で `record.value` をそのまま SeqLock 保護区間内で
コールバックに渡す設計は、既に `get_impl` が同じパターン（`callback(record.value)`,
`vmemkv_impl.hpp:411`）を使っていたため、新しい並行性パターンを持ち込むものではない。

呼び出し元の更新:
- `RocksDBStore::scan_impl`（`rocksdb_store.hpp`）: 8 バイトへの手動デコードを削除し、
  `callback(to_bytes(iterator->key()), to_bytes(iterator->value()))` として `PinnableSlice` の
  フルバイト列をそのまま渡すようにした。
- `bench_kv.cpp` の Scan 系コールバック 2 箇所（YCSB-E, ScanReorg→Scan）を、
  `[](std::span<const std::byte>, uint64_t value) { DoNotOptimize(value); }` から
  `[](std::span<const std::byte>, std::span<const std::byte> value) { DoNotOptimize(touch_bytes(value)); }`
  に変更（§5-a の `touch_bytes` を再利用）。
- `tests/test_kv_store.cpp` の `scan()` 呼び出し 6 箇所を新シグネチャに追従させた。整数値を
  格納したテストは `test_util::decode_scanned_u64()`（新規追加、値の memcpy デコード）で
  従来通り `uint64_t` として比較できるようにした。

`ninja` でのフルビルドと `tests/test_kv_store`（133 test cases / 56,609 assertions）が全て
成功することを確認済み。挙動が変わるのは「scan の value に何バイト渡るか」のみで、
scan が返す key・件数・順序・tombstone 除外などの意味論は変えていない。

### 5-c. YCSB-E の集計値が常に 0 / Infinity（**修正済み**）

`results_*.json` の YCSB-E 行は全て `iterations: 0`, `real_time: 0.0`,
`items_per_second: Infinity` になっていた（全 variant・全 value size で再現、`Elapsed_Sec` の
カスタムカウンタだけは正しく 30 秒前後を記録していた）。

原因は YCSB-E のベンチマーク本体が `for (auto _ : state)` を使わず、独自の `while (true)` ループで
30 秒間ポーリングする実装になっていたため（`register_bench` 呼び出し時に `Iterations(1)` を
指定しているにもかかわらず）、google benchmark 側の `StartKeepRunning`/`FinishKeepRunning` が
一度も呼ばれず、内部タイマーが起動していなかったことによる。

この行自体は今回の 3 点（特に 3-a, 3-b）の直接の原因ではない（全 variant で一律に壊れているため、
比較すると相殺されて見た目上は "気づきにくい" 形で埋もれていた）が、そのまま `items_per_second`
を見ると誤読を招くため、本調査で修正した。

修正内容（`bench_kv.cpp`）: 既存の `while (true)` 本体全体を `for (auto _ : state) { ... }` で包んだ。
`Iterations(1)` が既に指定されているため、このループは 1 回だけ実行され、
`for` の begin/end で google benchmark の計測区間が正しく 30 秒間全体をカバーするようになる。
実際に計測される中身（scan/insert の中身、`YCSBTimelineCollector` による毎秒集計）は変更していない。

### 5-d. `ScanReorg` の 1KB/64KB ラベルが実際には同一データ（**修正済み**）

`ScanReorg` ベンチマーク登録部（`bench_kv.cpp:1110-1151`、外側の `for (size_t val_size : value_sizes)`
ループ内）は、ベンチマーク名には外側ループの `value_name`（"1KB(20% 8B)" / "64KB(20% 8B)"）を
使う一方、実際の populate 呼び出しは常に固定値 `kInlineValueBytes`（8B）を使っていた。

```cpp
// 修正前
auto scan_reorg_meta = make_metadata(reorg_dataset_size, kInlineValueBytes);
...
[reorg_dataset_size, sname](auto &store) {
  populate(store, {reorg_dataset_size, kInlineValueBytes});
  ...
```

実データでもこれは裏付けられる。`results_ltm_1KB.json` と `results_ltm_64KB.json` の
`Op=ScanReorg` は、ラベル上は 8,000 倍の value size 差があるはずなのに、全 variant・全 thread 数で
5% 未満の差しかない（例: `Baseline/Dist=Zipf/threads:1`: 1KB ラベル 128,864 ops/s vs
64KB ラベル 134,125 ops/s）。これは両者が実際には同一の 8B ワークロードを計測していたことを
意味する。

修正内容: `kInlineValueBytes` を外側ループの `val_size` に置き換え、`populate()` とメタデータの
両方が実際の value size を使うようにした。これにより「1KB でどれだけ ScanReorg が遅くなるか」
「64KB でどれだけ遅くなるか」を初めて区別して観測できるようになる。

## 6. 適用した修正まとめ

初回調査時点（3-c, 3-d）と、ユーザーとの意思決定確認後（3-a, 3-b, Point 2）の 2 段階で
以下を修正した。フルビルド（`ninja`）と既存テストスイート（`tests/test_kv_store`,
133 test cases / 56,609 assertions）が全て成功することを確認済み。

1. **YCSB-E タイマー起動バグ**（§5-c）: `bench_kv.cpp` の YCSB-E 本体を
   `for (auto _ : state) { ... }` でラップ。`results_*.json` の `real_time` /
   `items_per_second` が意味のある値になる。挙動・計測対象自体への影響はない。
2. **`ScanReorg` value size ラベルの誤り**（§5-d）: populate を `kInlineValueBytes` 固定から
   `val_size` に修正し、ラベルと実データを一致させた。
3. **Get/Hit・Scan 系が値バイト列を読まない**（§5-a）: `touch_bytes()` ヘルパーを追加し、
   `Get/Hit` および Scan 系コールバックが値の全バイトを実際に読む（チェックサム畳み込み）
   ように変更した。
4. **`scan()` API の 8 バイト丸め**（§5-b）: `VMemKVImpl::scan_impl` と
   `RocksDBStore::scan_impl` を、Tier2 バックの値はフルサイズの `std::span` を返すように変更
   （HLD/LLD の記述に合わせる修正）。呼び出し元（`bench_kv.cpp`, `tests/test_kv_store.cpp`）を
   新シグネチャに追従させた。
5. **Scan-only (no reorganize) 系ベンチマークの追加**（§4, TODO Item 5 相当）:
   `Scan/Mode=NoReorg` / `Scan/Mode=T1Reorg` / `Scan/Mode=T1T2Reorg`（旧 `ScanReorg` の改称）の
   3 シナリオとして実装。`StoreAdapter::reorganize(bool force_t2_gc)` を新規追加し、T1 のみの
   reorganize を公開 API 経由で呼べるようにした。

## 7. 今回は対応を見送った点

- **Point 1 (LTM(1KB) Get/Hit(Zipf) threads:1 で Baseline が逆転する件)**: ユーザー確認の結果、
  今回は追加計測を行わないことで合意した。判断根拠は次の通り。
  - 3-a（Get が値を読まない問題）を修正したことで、64KB のケースは今後 RocksDB に対して
    順当に（不自然な高速化なしに）計測され直す可能性が高く、LTM 全体の傾向が変わりうる。
  - 単発計測（repetitions なし）に起因するノイズを減らす方針・実験時間を削減する方針は
    今後の検討課題として残った（本調査ではベンチマーク実行フロー自体は変更していない）。
  - したがって Point 1 の再評価は、3-a/3-b の修正を反映した **次回のベンチマーク再実行**
    まで保留する。再実行時は §3 に記載した推奨（repetitions 追加、ページフォールト数の記録）も
    合わせて検討するとよい。
