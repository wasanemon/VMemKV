# VMemKV 評価計画

このメモは、VMemKV 論文の評価戦略を定義するためのものです。最終的な論文本体ではなく、評価設計のための計画文書として書いています。

対象ブランチ / 実装基準: `wasanemon/VMemKV` の `paper-writing` ブランチ。設計方針としては `pr-14` の方向性を基準にします。

論文の中心主張:

> VMemKV は、virtual memory 上に構築された in-place-first な二層 key-value store である。KV 固有の metadata と update control は compact かつ mutable な T1 index に保持し、larger-than-memory な value residency は mmap-backed T2 region を通じて OS に委譲する。

したがって、評価は一般的な KV benchmark suite であってはいけません。この特定の主張が信頼できるかどうかを答える必要があります。

---

## 1. 評価で答えるべき問い

### RQ1. in-place-first update は update amplification を削減するか？

VMemKV の最も特徴的な主張は、既存 key に対する update を、多くの場合、新しい logical version に変換せずに実行できる点です。

ここで重要な実験は、単なる throughput ではありません。VMemKV が append/version-based design と比べて、logical update あたりの書き込み量を本当に削減できるかを測る必要があります。

必要な指標:

- update throughput
- p50 / p95 / p99 update latency
- 更新された logical value bytes
- T2 に append された bytes
- T2 上で in-place overwrite された bytes
- 取得可能であれば physical device bytes written
- in-place update hit rate
- append fallback rate
- 生成された T2 garbage bytes

期待される結果:

- fixed-size update は、ほとんど T2 in-place path に乗るべきである。
- value-growth update は、append + T1 offset swing に fallback するべきである。
- VMemKV は、stable-size value に対して低い update amplification を示し、value が成長するにつれて性能・書き込み量が段階的に悪化することを示すべきである。

これは論文にとって最も重要な評価です。

---

### RQ2. in-place update と append fallback の境界はどこにあるか？

VMemKV は、すべての update が物理的に in-place であると主張すべきではありません。正しい主張は in-place-first です。

意図的に境界を跨ぐ実験が必要です。

- same-size overwrite
- shrink update
- もしサポートされていれば、事前確保された `alloc_len` 内に収まる小さな growth
- `alloc_len` を超える growth
- random growth distribution

必要な指標:

- in-place hit rate
- append fallback rate
- latency distribution
- T2 bytes_used growth
- unreachable T2 bytes
- reorganize reclaimable bytes

期待される結果:

- same-size update と shrink update は安定して軽いはずである。
- value-growth update は T2 garbage を増やし、最終的に reorganization を必要とするはずである。

現在の `alloc_len` が `value_len` と同じである場合、最初の実装では same-size/shrink のみが in-place となり、growth は append fallback になることを示せばよいです。後続の slack-allocation variant を追加できるなら、reserve space が in-place hit rate を改善するかを検証できます。

---

### RQ3. OS に委譲した larger-than-memory value management は、想定条件で機能するか？

VMemKV は value residency を OS に委譲します。したがって、評価は in-memory microbenchmark だけではなく、larger-than-memory 条件で行う必要があります。

変化させる要素:

- dataset size / DRAM ratio: in-memory, near-memory, 2x memory, 4x memory
- access distribution: uniform, Zipf alpha 0.8, 1.0, 1.2
- value size: 64 B, 1 KiB, 4 KiB, 16 KiB
- workload mix: read-only, read-heavy, update-heavy, mixed

必要な指標:

- throughput
- p50 / p95 / p99 / p999 latency
- major page faults
- minor page faults
- 取得可能であれば page fault time
- SSD read bandwidth
- SSD write bandwidth
- CPU cycles / instruction count
- 取得可能であれば LLC misses と dTLB misses

期待される結果:

- T1 が memory-resident に保たれ、T2 が memory を超える条件で、VMemKV は最も説得力を持つべきである。
- memory を大きく超える value region に対する uniform cold access は悪い可能性がある。この場合でも、論文で target workload を正直に scope できていれば問題ない。
- skewed workload では、OS page cache residency の価値が見えるべきである。

---

### RQ4. mutable T1 index は、sorted region と append region の両方で効率的に動くか？

T1 の珍しい点は、`sorted_region` と `append_region` の両方で、ordering key を動かさずに payload を update できることです。

以下の key 位置ごとに update cost を示す必要があります。

- reorganize 前の append_region にある key
- reorganize 後の sorted_region にある key
- 両 region が混在している状態

必要な指標:

- key location ごとの update throughput
- key location ごとの get throughput
- T1 reorganize 前後の scan throughput
- append_region size sensitivity
- AppendMap の効果

期待される結果:

- 既存 key の update は、その key が sorted_region に移っただけで高コスト化してはいけない。
- append_region が大きい場合、point lookup/update に対して AppendMap が重要になるべきである。
- Reorganize は scan と negative lookup の挙動を改善するべきである。

---

### RQ5. reorganization は通常 update path ではなく、deferred repair mechanism として機能するか？

VMemKV の主張は、reorganization が不要になるというものではありません。主張は、reorganization が通常 update path から切り離されているという点です。

評価するもの:

- ordering fragmentation を生む insert-heavy workload
- T2 unreachable records を生む delete-heavy workload
- T2 garbage をほとんど生まない fixed-size update-heavy workload
- T2 garbage を生む value-growth update-heavy workload
- reorganize 前後の scan
- background または periodic reorganization 中の foreground latency

必要な指標:

- reorganize duration
- stop-the-world または publish pause time がある場合はその時間
- T2 reorganization 中に copy された bytes
- reclaim された bytes
- reorganization 前後の T1 append_region size
- reorganization 前後の T2 bytes_used
- reorganization 中の foreground p99 latency

期待される結果:

- fixed-size update は T2 garbage をほとんど、またはまったく生成しないべきである。
- delete と growth update は reclaimable T2 garbage を生成するべきである。
- Reorganization は scan locality を改善し、space を reclaim するべきだが、update の可視化そのものに毎回必要であってはいけない。

---

### RQ6. どの最適化が本質的で、どの最適化が補助的か？

実装にはすでに多くの variant が公開されています。評価では、すべての最適化を同じ重要度で扱わない方がよいです。

既存の variant family:

- Baseline
- cumulative T1 optimizations: AppendMap -> BloomFilter -> SimdScan -> MemoryHints
- subtractive ablations: No AppendMap / No BloomFilter / No SimdScan / No MemoryHints
- inline value variants: no inline / 1-7B inline / 8B inline / all inline
- RocksDB adapter

必要な指標:

- point lookup latency
- update latency
- negative lookup latency
- scan latency
- memory overhead
- T2 access count

期待される結果:

- append_region が大きい場合、AppendMap は point operation に必須であるべきである。
- BloomFilter は主に reorganize 後の negative lookup に効くべきである。
- SIMD と MemoryHints は、測定結果がそう示さない限り、二次的な最適化として扱うべきである。
- Inline values は tiny value workload では重要になるべきだが、論文の main claim にしてはいけない。

---

### RQ7. VMemKV は実装複雑性を下げているか？

Implementation simplicity は貢献ですが、慎重に提示する必要があります。主観的に「簡単」と主張するのではなく、VMemKV が何を実装していないかを測る、または表で示すべきです。

提示すべき evidence:

- VMemKV、RocksDB/LSM、value-log design、buffer-pool design の component table
- tests と benchmarks を除いた core T1/T2/reorganize path の lines of code
- 必要となる主要 storage-engine mechanism の数:
  - user-space buffer pool
  - page replacement policy
  - multi-level compaction scheduler
  - value-log GC
  - page-id-to-frame table
  - pointer swizzling
  - explicit read cache

期待される framing:

> VMemKV は、logical control を T1 に保持し、physical value residency を OS に委譲することで、DB 側の責務を減らす。これは storage management を消すのではなく、storage management の範囲を狭める設計である。

これは論文中では大きな benchmark section ではなく、小さな table として提示するのがよいです。

---

## 2. Baselines

### 必須 baseline

#### B1. RocksDB

目的:

- 実用的な LSM-based storage engine と比較する。
- すでに `VMemKV_RocksDB` adapter 経由で統合されている。

使用対象:

- point get/update/delete/insert
- mixed workload
- adapter が同等の scan semantics をサポートする場合は range scan
- physical bytes を測定できるなら update amplification comparison

注意:

- RocksDB tuning は明記する必要がある。
- byte amplification をきれいに比較するため、compression はおそらく無効化すべきである。
- とくに VMemKV の durability が未確定であるため、WAL / fsync policy は必ず記述する必要がある。

#### B2. VMemKV append-update baseline

目的:

- in-place-first update の価値を分離して示す。

これは最も重要な未実装 baseline です。

定義:

- すべての update で、常に新しい T2 record を append し、T1 offset を更新する。
- value が old allocation に収まる場合でも、`update_value_at` を呼ばない。

これは、同じ T1/T2 infrastructure を保ったまま、value-log style の update behavior を模倣する baseline です。

必要な実装:

- `AppendOnlyUpdate` または `DisableT2InPlaceUpdate` のような config tag を追加する。
- benchmark variant として公開する。

使用対象:

- RQ1 update amplification
- RQ2 in-place boundary
- RQ5 reorganization pressure

#### B3. VMemKV baseline / all-on variants

目的:

- base architecture と opt-in optimizations を区別する。

使用するもの:

- `VMemKV_Baseline`
- cumulative variants
- all-on variant
- subtractive ablations

#### B4. mmap-only / OS-only baseline

目的:

- VMemKV が単なる mmap ではないことを示す。

最小構成:

- T1/T2 の logical control を持たない mmap-backed array/file access
- あるいは、真の KVS baseline を実装できるなら、simple unordered-map index + mmap value file

これは有用ですが、B2 ほど重要ではありません。

### Optional baseline

#### B5. pread + LRU baseline

目的:

- OS-delegated residency と明示的 user-space caching を比較する。

既存の microbenchmark results は motivation として使えますが、KV-integrated な pread+LRU baseline があるとより強いです。

時間がある場合のみ実装します。

#### B6. LevelDB

目的:

- classic LSM baseline と比較する。

RocksDB をすでに使い、benchmark 時間が限られる場合は optional でよいです。

#### B7. Bitcask-like append-only baseline

目的:

- append-only data file + in-memory index と比較する。

安く実装できるなら有用です。そうでない場合、B2 が VMemKV framework 内で最も重要な value-log-style update behavior をすでに捉えます。

---

## 3. Workloads

### W1. Load

- sequential insert
- random insert
- 後続 workload のための dataset construction

指標:

- load throughput
- bytes written
- T2 bytes_used
- T1 size

目的:

- append-friendly insert behavior を示す。
- read/update/scan experiments の dataset を作る。

---

### W2. Fixed-size update

入力:

- N keys を fixed-size values で preload する。
- 既存 key に対して、同じ value size で繰り返し update する。

パラメータ:

- value size: 8 B, 64 B, 1 KiB, 4 KiB
- access distribution: uniform and Zipf
- thread count: 1, 4, hardware concurrency

指標:

- update throughput
- p50/p95/p99 latency
- in-place hit rate
- append fallback rate
- logical update あたりの bytes written
- T2 bytes_used growth

目的:

- in-place-first update claim の主要な証拠にする。

---

### W3. Value-growth update

入力:

- N keys を size S の values で preload する。
- size S, 2S, 4S, または random growth の values で update する。

パラメータ:

- initial size: 64 B, 1 KiB
- new size: same, +16 B, 2x, 4x
- optional slack allocation: 実装されている場合、0%, 25%, 50%, 100%

指標:

- in-place hit rate
- append fallback rate
- T2 garbage bytes
- reorganization reclaim ratio
- update latency

目的:

- in-place-first claim の境界を描く。

---

### W4. Mixed read/update workload

推奨する YCSB-like mixes:

- A-like: 50% read / 50% update
- B-like: 95% read / 5% update
- C-like: 100% read
- F-like: read-modify-write

パラメータ:

- uniform vs Zipf alpha 1.0
- value size: 64 B, 1 KiB, 4 KiB
- dataset / memory ratio: in-memory, 2x, 4x

指標:

- throughput
- latency CDF
- update amplification
- page faults
- SSD bandwidth

目的:

- realistic mix における end-to-end behavior を示す。

---

### W5. Range scan and scan-after-reorganize

入力:

- ordered keys を preload する。
- 異なる scan window size を試す。
- T1/T2 reorganize 前後で測る。

パラメータ:

- scan window: 100, 1K, 10K keys
- dataset size: in-memory and larger-than-memory
- reorg 前の append_region fraction: 0%, 10%, 50%, 100%

指標:

- scan throughput
- per-record scan latency
- page faults
- T2 read locality
- reorganization の効果

目的:

- reorganization が update visibility のためではなく、ordering/locality の repair のためであることを示す。

---

### W6. Delete-heavy workload

入力:

- N keys を preload する。
- key の一部を delete する。
- reorganize 前後で reads/scans を測る。

パラメータ:

- delete ratio: 10%, 50%, 90%
- uniform deletes vs clustered deletes

指標:

- delete throughput
- T2 unreachable bytes
- reorg 前後の scan cost
- reorg によって reclaim された bytes

目的:

- T1 tombstone behavior と deferred T2 garbage collection を示す。

---

## 4. Measurement Methodology

### Performance metrics

- operations per second
- p50 / p95 / p99 / p999 latency
- update-heavy workload の latency CDF
- background reorganization 中の throughput

### Storage metrics

- inserted/updated された logical payload bytes
- T2 bytes appended
- T2 bytes overwritten in place
- T2 bytes_used
- unreachable T2 bytes
- reorganization で reclaim された bytes
- 取得可能であれば physical device bytes written

Derived metrics:

```text
update_amplification_logical = T2_bytes_written_or_appended / logical_update_bytes
append_fallback_rate = append_fallback_updates / total_updates
in_place_update_rate = in_place_updates / total_updates
reclaim_ratio = bytes_reclaimed_by_reorg / unreachable_bytes_before_reorg
```

### OS / hardware metrics

Linux を推奨します。

- `perf stat`
  - page-faults
  - minor-faults
  - major-faults
  - cycles
  - instructions
  - cache-misses
  - dTLB-load-misses
  - LLC-load-misses
- `/proc/self/stat` または `getrusage`
  - process ごとの minor / major faults
- `iostat -dx`
  - read/write bandwidth
  - device utilization
- `/proc/diskstats`
  - physical sectors read/written

### Memory control

larger-than-memory 評価には、制御された memory pressure が必要です。

選択肢:

- memory limit 付き cgroup 内で実行する。
- fixed-memory VM を使う。
- machine memory に対して dataset size を変化させる。
- 許可される環境では、cold run の前に明示的に page cache を drop する。
- warm results と cold results を分けて報告する。

cold-start 結果と steady-state 結果を、label なしに混ぜてはいけません。

---

## 5. 必要な Instrumentation

現在の benchmark support は有用ですが、論文の main claim には不十分です。

軽量な `Stats` structure を VMemKV に追加し、できれば benchmark harness から取得できるようにします。

推奨 counters:

```cpp
struct VMemKVStats {
    uint64_t get_count;
    uint64_t insert_count;
    uint64_t update_count;
    uint64_t delete_count;

    uint64_t t1_existing_payload_updates;
    uint64_t t1_append_inserts;
    uint64_t t1_tombstone_updates;

    uint64_t t2_appends;
    uint64_t t2_in_place_updates;
    uint64_t t2_append_update_fallbacks;
    uint64_t t2_bytes_appended;
    uint64_t t2_bytes_overwritten;
    uint64_t t2_unreachable_bytes_estimate;

    uint64_t reorganize_count;
    uint64_t reorganize_bytes_copied;
    uint64_t reorganize_bytes_reclaimed;
    uint64_t reorganize_duration_ns;
    uint64_t reorganize_publish_pause_ns;
};
```

最低限必要な counters:

- `t2_in_place_updates`
- `t2_append_update_fallbacks`
- `t2_bytes_appended`
- `t2_bytes_overwritten`
- `reorganize_bytes_reclaimed`
- `reorganize_duration_ns`

これらがないと、in-place-first claim を説得力を持って裏付けることはできません。

---

## 6. 優先度付き計画

### P0: 論文に必須

1. **Fixed-size update vs append-update baseline**
   - VMemKV all-on
   - VMemKV append-update baseline
   - RocksDB
   - metrics: throughput, latency, update amplification, T2 bytes growth

2. **Value-growth boundary**
   - same-size vs growing updates
   - metrics: in-place rate, fallback rate, garbage bytes, latency

3. **Larger-than-memory mixed workload**
   - dataset / memory ratio sweep
   - read-heavy and update-heavy mixes
   - metrics: throughput, p99, page faults, SSD bandwidth

4. **Reorganization effect**
   - before / after scan
   - delete-heavy and value-growth workloads
   - metrics: reclaimed bytes, duration, scan improvement, foreground p99

5. **既存 VMemKV variants を使った ablation**
   - baseline, cumulative, all-on, no AppendMap, no BloomFilter, no MemoryHints, inline variants
   - どの optimization がどの claim を支えるかに集中する。

### P1: 強く推奨

1. Implementation simplicity table
2. T1 update cost by key location: append_region vs sorted_region
3. BloomFilter あり/なしの reorganize 後 negative lookup
4. Memory ratio and Zipf alpha sweep
5. Reorganization 中の tail latency

### P2: optional / 時間がある場合のみ

1. pread+LRU KV baseline
2. LevelDB baseline
3. Bitcask-like baseline
4. THP / madvise sweep
5. durability design が確定した後の crash recovery / restart time

---

## 7. まだ評価しないもの

現在の paper claim を支えない評価には、労力を割きすぎないようにします。

優先しないもの:

- distributed transactions
- phantom avoidance
- secondary indexes
- full SQL/DBMS workloads
- write/recovery design が確定していない状態での crash consistency
- ablation を超えた詳細な SIMD microbenchmarks
- あらゆる RocksDB tuning permutation

この論文は、以下によって評価されるべきです。

1. in-place-first update behavior
2. OS-delegated larger-than-memory value management
3. T1/T2 responsibility separation
4. reorganization as deferred repair
5. implementation simplicity

---

## 8. Evaluation section の推奨 outline

```text
8. Evaluation
   8.1 Experimental Setup
       Hardware, OS, filesystem, storage, memory limit, compiler, RocksDB configuration.

   8.2 Update Amplification and In-Place Hit Rate
       Fixed-size update vs append-update baseline vs RocksDB.

   8.3 Boundary of In-Place-First Updates
       Same-size, shrink, and value-growth update experiments.

   8.4 Larger-than-Memory Behavior
       Dataset/memory ratio, skew, value size, page faults, and tail latency.

   8.5 Reorganization as Deferred Repair
       Scan before/after reorg, delete/value-growth garbage, reclaimed bytes, pause time.

   8.6 Component Ablation
       AppendMap, BloomFilter, SIMD, MemoryHints, InlineShort, Inline8B.

   8.7 Implementation Complexity
       Small table comparing DB-side mechanisms across VMemKV, LSM, value-log, buffer-pool designs.
```

論文を短くする必要がある場合は、8.3 を 8.2 に統合し、8.6 を関連する各 experiment section に吸収します。

---

## 9. 最小限の benchmark 実装 checklist

中園氏、または実装担当者向け:

- [ ] T1/T2 update path 用の VMemKV stats counters を追加する。
- [ ] T2 in-place update を無効化し、force append-on-update する config を追加する。
- [ ] benchmark mode: fixed-size update を追加する。
- [ ] benchmark mode: value-growth update を追加する。
- [ ] benchmark mode: configurable ratio の mixed read/update を追加する。
- [ ] benchmark mode: dataset size / value size sweep を追加する。
- [ ] benchmark mode: scan before/after reorganize を追加する。
- [ ] benchmark output を machine-readable CSV または JSON にする。
- [ ] 各 run で page faults と process RSS を記録する。
- [ ] T2 bytes_used と bytes reclaimed を記録する。
- [ ] RocksDB options と sync policy を文書化する。

---

## 10. 論文に載せたい図

### Figure 1: Update amplification

- x-axis: value size または workload type
- y-axis: logical update あたりの bytes written
- lines: VMemKV in-place, VMemKV append-update, RocksDB

### Figure 2: In-place boundary

- x-axis: new value size / old value size
- y-axis: in-place hit rate and update latency

### Figure 3: Larger-than-memory performance

- x-axis: dataset / memory ratio
- y-axis: throughput and p99 latency
- series: VMemKV, RocksDB, optional pread+LRU

### Figure 4: Page fault behavior

- x-axis: dataset / memory ratio または Zipf alpha
- y-axis: major faults / operation

### Figure 5: Reorganization effect

- scan throughput, T2 bytes_used, reclaimed bytes の before/after bars

### Figure 6: Ablation

- VMemKV variants に対する key operations の bar chart
- 関連する箇所でのみ AppendMap / BloomFilter / Inline effects を強調する。

### Table 1: Implementation responsibility

VMemKV、LSM/RocksDB、WiscKey-like value-log、buffer-pool engine を比較する。

Columns:

- explicit buffer pool
- page replacement policy
- multi-level compaction
- value-log GC
- in-place update path
- OS-managed value residency
- ordered scan support

---

## 11. Negative results が出た場合の解釈

VMemKV がすべての throughput graph で勝たなくても、評価は成立するように設計するべきです。

### 一部 workload で RocksDB が速い場合

以下を示せていれば、論文の主張は崩れません。

- stable-size updates に対して update amplification が低い。
- implementation responsibility が少ない。
- 想定 workload regime で larger-than-memory behavior が competitive である。

### very fast NVMe 上で mmap-backed T2 が不利な場合

これは scope condition として扱います。OS-delegated residency は workload と device に依存することが予想されます。

### value-growth updates が大量の garbage を生む場合

これは想定内です。in-place-first の境界を示し、reorganization の必要性を動機づける結果として扱えます。

### SIMD や MemoryHints があまり効かない場合

これらは secondary optimizations として扱います。中心主張にしてはいけません。

---

## 12. 評価の一文目標

評価が示すべきことは、VMemKV が単なる「mmap plus a hash table」ではないという点です。mutable T1 index、allocation-preserving T2 updates、deferred reorganization が組み合わさることで、in-place-first な larger-than-memory KV storage が実現可能であり、測定可能な性質として現れることを示す必要があります。
