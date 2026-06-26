# 2026-05-01 T1Index vs RocksDB Benchmark Report

## 1. Scope

本レポートは、`benchmark/latest_full_20260501.txt` の計測結果と、
[high_level_design.md](../specification/high_level_design.md) /
[low_level_design.md](../specification/low_level_design.md) に記載された仕様だけを根拠としてまとめた性能評価である。

本実験の対象は、VMemKV 全体ではなく、その中の Tier 1 に相当する fixed-size index 層である。high-level design では、VMemKV は Tier 1 と Tier 2 から成る二層構造として定義されており、point lookup と scan の起点は Tier 1 が担う。本実験は、この Tier 1 を単独の in-memory KVS として切り出したときに、

- `Reorganizing Two-Region` 構造そのものが高い性能を持ちうるか
- `append_region` と `sorted_region` の分離、および `reorganize` が性能特性にどう表れるか
- low-level design で定義した opt-in 最適化が Tier 1 単体でどこまで有効か

を確認することを目的とする。

したがって、本レポートは VMemKV の durability、checkpoint reload、Tier 2 の variable-size record 管理、WAL replay を含む全体評価ではない。一方で、仕様書上 Tier 1 は VMemKV のホットパスそのものであるため、この実験は「VMemKV 全体の性能上限を決める中核部分が、単体でどの程度強いか」を確認する preliminary evaluation と位置づけられる。

対象は以下の比較である。

- T1Index の各構成
- RocksDB

計測条件は以下。

- Build: `Release`
- RocksDB: enabled
- Multi-thread duration: 20 seconds / run
- Hardware concurrency: 12 threads

なお、nanobench の一部 single-thread 項目には `Unstable` 表示が付いている。したがって、細かな数値差よりも、繰り返し現れている大きな傾向を重視して解釈する。

## 2. Executive Summary

### 2.1 Overall Conclusion

- 少なくとも本計測条件では、T1Index は多くの workload で RocksDB を大きく上回る。
- 特に insert / get / update / delete / mixed workload では、T1Index の優位が大きい。
- scan workload でも T1Index は強く、さらに `reorganize` 後に性能が向上する。
- 仕様書が主張している `Reorganizing Two-Region` の性能特性は、今回の結果によって概ね支持された。

仕様との対応で見ると、これは自然である。high-level design では `append_region` は unordered であり、base design の point lookup は `append_region` の探索コストを含む。一方、`reorganize` は ordering fragmentation を解消し、low-level design では opt-in として `append_region` 用 hashmap と Bloom filter が定義されている。今回の結果は、その設計意図がかなり素直に表面化したものといえる。

### 2.2 Effect of Optimizations

- `append_region` 探索の高速化は、point workload に対して支配的な改善要因だった。
- `reorganize` は scan workload に対して大きな改善をもたらした。
- Bloom filter は通常の positive lookup では寄与が小さく、`reorganize` 後の negative lookup で価値が見えた。
- SIMD / Memory Hints は補助的であり、主要な性能差を説明する要因ではなかった。

## 3. Single-Thread Results

代表値を抜くと次のとおり。

| Workload | T1 AllOff | T1 best observed | RocksDB | Observation |
| --- | ---: | ---: | ---: | --- |
| Insert | 0.300 M ops/s | 7.101 M ops/s | 0.491 M ops/s | T1 は最適化有効時に大幅増速 |
| Get hit | 0.763 M ops/s | 12.629 M ops/s | 2.661 M ops/s | T1 best は RocksDB を大きく上回る |
| Get miss | 0.487 M ops/s | 8.369 M ops/s | 2.402 M ops/s | miss でも T1 best が優位 |
| Update | 0.866 M ops/s | 11.153 M ops/s | 0.422 M ops/s | update も T1 が大幅優位 |
| Delete | 0.049 M ops/s | 7.555 M ops/s | 0.439 M ops/s | delete は最適化有無で極端に差が出る |

ここで最も重要なのは、point operation の支配要因が非常に一貫している点である。`AllOff` と `UseAppendMapV` 系の差が大きく、他の opt-in 単独構成は `AllOff` に近い。これは仕様書にある通り、base design の `Get` が `append_region` の探索を含み、opt-in の hashmap 化がそこを expected `O(1)` に落とすためと解釈できる。

`Delete` の差が特に極端なのも同じ説明で理解できる。仕様上 delete 自体は Tier 1 上の tombstone 化で済むため、本来の処理は軽い。しかし対象 key を見つける経路が遅い構成では、delete の見かけの性能が lookup コストに支配される。

## 4. Scan and Reorganize

scan 系では、仕様書が強調している `reorganize` の意味がよく出ている。

### 4.1 Single-thread scan

`SCAN/10000` の代表値:

- `T1/AllOff/Scan`: 1.011 G ops/s
- `T1/AllOff/Scan(reorg)`: 1.726 G ops/s
- `T1/Cumulative/AllOn/Scan(reorg)`: 1.827 G ops/s
- `RocksDB/Scan`: 18.607 M ops/s

`reorganize` 後は、T1 のどの構成でも scan が大きく改善している。これは high-level design の

- `append_region` の肥大化が ordering fragmentation を生む
- `reorganize` が `append_region` を `sorted_region` に吸収して scan 性能を改善する

という説明と整合的である。

逆に言うと、scan の主要改善は `SIMD` や `Bloom filter` ではなく、まず `reorganize` 自体にある。これは 1000 / 5000 / 10000 の scan 系でも同じ傾向だった。

### 4.2 Data-scale scan

`SCAN N=20000 win=1000` では次の傾向が見える。

- T1 の各構成: 約 56M から 75M ops/s
- RocksDB: 約 9.4M から 10.0M ops/s

ここでは T1 が一貫して RocksDB を上回るが、構成間差は point operation ほど大きくない。仕様上、scan は `sorted_region` の範囲列挙が主経路であり、point lookup 向け opt-in の恩恵が支配的でないため、この結果は自然である。

## 5. Multi-Thread Throughput

multi-thread でも傾向は変わらない。

### 5.1 GET

代表値:

| Threads | T1 AppendMap | T1 AllOn | RocksDB |
| --- | ---: | ---: | ---: |
| 1 | 11.74 | 11.77 | 2.53 |
| 4 | 8.93 | 9.46 | 7.59 |
| 12 | 5.65 | 6.02 | 4.98 |

RocksDB は thread 数増加である程度スケールするが、T1 best は全点で上回った。とくに 1-thread / 4-thread では差が大きい。

### 5.2 INSERT

代表値:

| Threads | T1 AppendMap | T1 AllOn | RocksDB |
| --- | ---: | ---: | ---: |
| 1 | 4.35 | 4.35 | 0.31 |
| 4 | 2.23 | 2.64 | 0.34 |
| 12 | 1.56 | 1.58 | 0.18 |

insert は仕様上 `append_region` への追記が基本経路であるため、T1 の強さが最も出やすい。RocksDB は 4-thread, 12-thread でも大きく伸びず、今回条件では T1 が明確に優位だった。

### 5.3 MIXED (80R/20W)

代表値:

| Threads | T1 AppendMap | T1 AllOn | RocksDB |
| --- | ---: | ---: | ---: |
| 1 | 11.08 | 11.22 | 0.77 |
| 4 | 7.56 | 10.87 | 1.13 |
| 12 | 4.80 | 5.06 | 0.72 |

read-heavy mixed でも T1 が支配的に高い。仕様上、point read と update の双方が Tier 1 を主経路とし、append 追記および in-place update を使い分ける設計なので、read-heavy mixed で有利になるのは妥当である。

## 6. Data-Scale Behavior

### 6.1 Positive Get

`GET N=20000` は設計上もっとも説明力が高い。

| Case | Zipf | Uniform |
| --- | ---: | ---: |
| T1 AllOff | 0.138 M ops/s | 0.250 M ops/s |
| T1 AppendMap | 9.489 M ops/s | 8.946 M ops/s |
| T1 AllOn | 9.485 M ops/s | 8.860 M ops/s |
| RocksDB | 2.273 M ops/s | 2.016 M ops/s |

`AllOff` が dataset size とともに大きく悪化し、`UseAppendMapV` 系はほぼ一定帯域を保っている。これは low-level design にある

- base design: `O(|append_region|) + O(log |sorted_region|)`
- append hashmap 有効時: expected `O(1) + O(log |sorted_region|)`

そのものの挙動である。

### 6.2 Negative Get after Reorganize

`NEGATIVE_GET(reorg) N=20000` では Bloom filter の意味がはっきり見える。

| Case | Zipf | Uniform |
| --- | ---: | ---: |
| T1 AllOff | 10.117 M ops/s | 10.144 M ops/s |
| T1 Single Bloom | 10.790 M ops/s | 13.826 M ops/s |
| T1 AllOn | 9.776 M ops/s | 10.495 M ops/s |
| RocksDB | 3.191 M ops/s | 2.656 M ops/s |

Uniform miss では Bloom filter 有効構成が最も良い。これは仕様書の「`sorted_region` ネガティブルックアップ用 Bloom filter による miss 時の O(1) 化」と整合的である。

一方で Zipf miss では改善が限定的であり、Bloom filter は万能ではない。negative lookup が多く、かつ `reorganize` 後の sorted lookup が支配的な workload で価値が高い、という読み方が適切である。

### 6.3 Scan vs Dataset Size

`SCAN N=20000 win=1000` では、

- T1 best: 74.647 M ops/s
- T1 AllOff: 63.138 M ops/s
- RocksDB: 9.966 M ops/s

だった。scan では point lookup 向け opt-in の寄与は限定的で、主要因は `sorted_region` 中心の範囲走査と `reorganize` による ordering fragmentation 解消である、と読むのがよい。

## 7. Interpretation by Specification

本結果から、仕様書に対して次の評価ができる。

### 7.1 設計の主張として強く支持される点

- `append_region` の unordered 性は本当に性能ボトルネックになる。
- `reorganize` は scan に対して有効である。
- `append_region` 用 hashmap は point workload に対して最重要の opt-in である。
- Bloom filter は negative lookup 向けとしては妥当である。

### 7.2 限定的な効果にとどまった点

- SIMD scan は一部 workload で改善したが、支配的ではない。
- Memory Hints も一部で改善したが、主役ではない。

これらは仕様書にある「すべて独立の opt-in で、無効でも正しく動作する」という位置づけに合っている。つまり、補助最適化としては妥当だが、ベース性能を決めているのは別の要素である。

## 8. Practical Recommendation

今回の結果だけから実運用向けの推奨を書くなら次の通りである。

- point read / write が中心なら、`append_region` 高速化はほぼ必須である。
- negative lookup が多い read-mostly workload なら、Bloom filter は検討価値が高い。
- scan 重視なら、まず `reorganize` の運用頻度が重要であり、他 opt-in の優先度はその後になる。
- future benchmark では、`Unstable` 表示が付いた項目に対して `minEpochIterations` を引き上げ、ばらつきを減らして再計測する価値がある。

## 9. Final Assessment

仕様書の観点から見ると、今回の結果はかなり筋が通っている。

- base design の弱点は `append_region` 探索に現れた
- `reorganize` の価値は scan で確認できた
- opt-in 最適化のうち、本質的なものと補助的なものが明瞭に分かれた

純粋な性能比較としては、今回の条件では T1Index は多くの workload で RocksDB を上回った。特に insert / get / update / mixed workload では優位が大きく、scan でも強かった。

したがって、少なくとも Tier 1-only の fixed-size KVS としては、仕様書が目指している「単純な構造で高い性能を得る」という方向性は、今回のベンチマーク結果によって十分支持されたと言える。
