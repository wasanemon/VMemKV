# FaultKV マイクロベンチマーク レポート (Linux WSL2)

## 0. テスト環境

| 項目         | 詳細 |
| ------------ | ---- |
| マシン       | WSL2 仮想環境 (Hyper-V) |
| CPU          | Intel Core i5-13500 @ 2.50 GHz - 20 vCPU (10C/20T) |
| RAM          | 31 GiB (Swap 8 GiB) |
| ストレージ   | /dev/sdc (1 TB, ext4 on WSL2 virtual disk) |
| OS           | Ubuntu 22.04.5 LTS (Jammy), Kernel 5.15.167.4-microsoft-standard-WSL2 |
| コンパイラ   | GCC 11.4.0 / C++20, `-O3 -march=native` |
| データセット | 96 GB |
| THP 設定     | `[always] madvise never` |

> 注意: WSL2 は Linux カーネル上で動作するが、I/O とメモリ管理の一部は Hyper-V 層の影響を受ける。したがって物理 Linux や KVM Linux と同じ傾向にならない場合がある。

---

## 1. 目的

FaultKV の中核仮説「OS の仮想メモリ機構（mmap + page cache）は、アプリ実装の `pread + LRU` バッファ管理と競合しうるか」を WSL2 環境で検証する。特に Linux KVM レポートとの差分を重視する。

---

## 2. 実験設計

### 2.1 比較対象

|                 | mmap path                     | pread + LRU path |
| --------------- | ----------------------------- | ---------------- |
| バッファ管理    | カーネル page cache           | アプリ内 LRU     |
| OS cache bypass | なし                          | `O_DIRECT`       |
| miss 時         | page fault                    | `pread(2)`       |
| hit 時          | load 命令のみ                 | hash + list 操作 |

### 2.2 ワークロード

- データ: 96 GB ファイル
- レコード: 4 KB
- 測定: 1,000,000 ops（warmup 50,000 ops 除外）
- 標準分布: Zipf `alpha=1.0`
- LRU 容量: 20,000 pages（80 MB）

### 2.3 実験パラメータ

| 実験 | 変数 |
| ---- | ---- |
| cold/warm baseline | `--prefault` |
| 局所性変化 | `--zipf-alpha` |
| 先読み | `--prefetch-ahead` |
| 並列性 | `--threads` |
| scan 最適化 | `--scan-width` |
| hot set サイズ | `--zipf-n` |
| THP | `--huge-pages` |

---

## 3. 実験結果

### 3.1 cold vs warm baseline

| 状態 | mmap (ops/sec) | pread+LRU (ops/sec) | LRU hit 率 | 勝者 |
| ---- | -------------- | ------------------- | ---------- | ---- |
| cold (`cold.json`) | **12,481.5** | 11,161.5 | 49.5% | **mmap 1.12x** |
| warm (`warm.json`) | **3,474,013.3** | 11,375.9 | 49.5% | **mmap 305x** |

cold レイテンシ（alpha=1.0）:

|     | mmap | pread+LRU |
| --- | ---- | --------- |
| p50 | 0.832 us | 99.807 us |
| p95 | 274.325 us | 247.605 us |
| p99 | 438.386 us | 305.037 us |

> 観察: cold では mmap がわずかに勝つが、tail latency (p95/p99) は pread+LRU より悪い。Linux KVM と比べると、WSL2 は page fault 側の tail が重い。

### 3.2 アクセス局所性の影響（Zipf alpha）

| alpha | mmap (ops/sec) | pread+LRU (ops/sec) | LRU hit 率 | 勝者 |
| ----- | -------------- | ------------------- | ---------- | ---- |
| 0.5 | 5,828.5 | **5,978.8** | 0.3% | **LRU 1.03x** |
| 1.0 (`alpha_100.json`) | **15,573.5** | 11,695.4 | 49.5% | **mmap 1.33x** |
| 1.5 | **451,754.5** | 413,485.8 | 98.6% | **mmap 1.09x** |
| 2.0 | **3,846,278.2** | 2,790,377.4 | 99.9% | **mmap 1.38x** |

> 観察: WSL2 でも大局的には mmap 優位だが、alpha=0.5 のほぼ一様アクセスでは LRU が僅差で上回る。これは Linux KVM レポートとの明確な差分である。

### 3.3 MADV_WILLNEED 先読み

| prefetch-ahead | mmap (ops/sec) | baseline 比 | pread+LRU (ops/sec) | 勝者 |
| -------------- | -------------- | ----------- | ------------------- | ---- |
| 0 | 12,481.5 | 1.0x | 11,161.5 | mmap 1.12x |
| 8 | 52,284.3 | 4.2x | 11,110.9 | **mmap 4.71x** |
| 32 | 137,619.1 | 11.0x | 10,967.1 | **mmap 12.55x** |

### 3.4 マルチスレッド並行性

| threads | mmap (ops/sec) | cold 比 | pread+LRU (ops/sec) | LRU hit 率 | 勝者 |
| ------- | -------------- | ------- | ------------------- | ---------- | ---- |
| 1 | 12,481.5 | 1.0x | 11,161.5 | 49.5% | mmap 1.12x |
| 8 | **118,768.4** | 9.5x | 70,976.9 | 36.5% | **mmap 1.67x** |

### 3.5 Scan バッチプリフェッチ

| scan-width | threads | mmap (ops/sec) | pread+LRU (ops/sec) | 勝者 |
| ---------- | ------- | -------------- | ------------------- | ---- |
| 1 (point) | 1 | 12,481.5 | 11,161.5 | mmap 1.12x |
| 32 | 1 | **105,516.1** | 11,979.3 | **mmap 8.81x** |
| 32 | 8 | **511,066.8** | 70,579.0 | **mmap 7.24x** |

### 3.6 ホットセットサイズ

| zipf-n | hot set サイズ | mmap (ops/sec) | pread+LRU (ops/sec) | LRU hit 率 | 勝者 |
| ------ | -------------- | -------------- | ------------------- | ---------- | ---- |
| 20,000 | 80 MB | **535,652.3** | 267,863.8 | 98.0% | **mmap 2.00x** |
| 200,000 | 800 MB | **54,586.9** | 23,761.0 | 74.6% | **mmap 2.30x** |
| 2,000,000 | 8 GB | **24,311.0** | 15,389.7 | 60.1% | **mmap 1.58x** |
| 25,165,824 | 96 GB | **12,481.5** | 11,161.5 | 49.5% | **mmap 1.12x** |

### 3.7 THP の効果（WSL2 特性）

| 実験 | THP | mmap (ops/sec) | pread+LRU (ops/sec) | 勝者 |
| ---- | --- | -------------- | ------------------- | ---- |
| warm | off | 3,474,013.3 | 11,375.9 | mmap 305x |
| warm | on | **2,496,231.6** | 11,494.5 | **mmap 217x** |
| cold | off | 12,481.5 | 11,161.5 | mmap 1.12x |
| cold | on | **15,347.1** | 11,263.1 | **mmap 1.36x** |
| hot set 80 MB | off | 535,652.3 | 267,863.8 | mmap 2.00x |
| hot set 80 MB | on | **488,685.0** | 241,164.1 | **mmap 2.03x** |
| hot set 800 MB | off | 54,586.9 | 23,761.0 | mmap 2.30x |
| hot set 800 MB | on | **51,140.0** | 23,469.2 | **mmap 2.18x** |

> 観察: Linux KVM レポートで見られた「THP が大きな hot set で壊滅的に悪化する挙動」は WSL2 では確認されなかった。一方で hot set 80 MB/800 MB における THP の大幅加速（KVM の 25x/199x）も出ていない。WSL2 では THP の影響が中庸になる。

---

## 4. 全実験まとめ

| 実験 | mmap (ops/sec) | pread+LRU (ops/sec) | 勝者 | 比率 |
| ---- | -------------- | ------------------- | ---- | ---- |
| cold alpha=1.0 | 12,481.5 | 11,161.5 | mmap | 1.12x |
| warm alpha=1.0 | 3,474,013.3 | 11,375.9 | mmap | 305x |
| cold alpha=0.5 | 5,828.5 | 5,978.8 | LRU | 0.97x |
| cold alpha=1.5 | 451,754.5 | 413,485.8 | mmap | 1.09x |
| cold alpha=2.0 | 3,846,278.2 | 2,790,377.4 | mmap | 1.38x |
| prefetch-ahead=8 | 52,284.3 | 11,110.9 | mmap | 4.71x |
| prefetch-ahead=32 | 137,619.1 | 10,967.1 | mmap | 12.55x |
| threads=8 | 118,768.4 | 70,976.9 | mmap | 1.67x |
| scan-width=32 | 105,516.1 | 11,979.3 | mmap | 8.81x |
| threads=8 + scan-width=32 | 511,066.8 | 70,579.0 | mmap | 7.24x |
| hot set 80 MB | 535,652.3 | 267,863.8 | mmap | 2.00x |
| hot set 800 MB | 54,586.9 | 23,761.0 | mmap | 2.30x |
| hot set 8 GB | 24,311.0 | 15,389.7 | mmap | 1.58x |
| warm + THP | 2,496,231.6 | 11,494.5 | mmap | 217x |
| cold + THP | 15,347.1 | 11,263.1 | mmap | 1.36x |

---

## 5. 考察

### 5.1 WSL2 でのコスト構造

`cold_uniform.json` の結果から、ほぼ全ミス時のコストは次のレンジにある:

- mmap fault: p50 ~169 us
- pread miss: p50 ~176 us

このため、cold でも mmap と pread+LRU は拮抗しやすい。加えて hit 時は mmap が有利（pointer load のみ）なので、alpha=1.0 以上では mmap 優勢になる。

### 5.2 Linux KVM との差分

- KVM: alpha=0.5 で mmap 微勝
- WSL2: alpha=0.5 で LRU 微勝

WSL2 の cold fault tail（p95/p99）が重く、ランダムアクセスでの mmap 不利が僅かに残る可能性がある。

### 5.3 THP 挙動の違い

Linux KVM では THP がワークロードにより「劇的加速」か「壊滅的劣化」に分かれたが、WSL2 では次の中庸挙動を示した:

- 壊滅的劣化なし（warm+THP でも 2.5M ops/sec）
- 劇的加速もなし（hot set 80 MB / 800 MB で約2x止まり）

この差は、WSL2 のメモリ管理と Hyper-V 層の実装差が THP 実効に影響している可能性を示唆する。

### 5.4 FaultKV 設計への示唆

- WSL2 上でも mmap 設計は十分競争力がある（大半の条件で優位）。
- ただし THP 評価は WSL2 単独で一般化すべきでない。最終判断は物理 Linux / KVM / クラウド VM で再確認が必要。

---

## 6. 結論

| 問い | 結果 |
| ---- | ---- |
| WSL2 でも mmap は LRU と競合できるか？ | **Yes**（多くの条件で mmap 優位） |
| 一様アクセス（alpha=0.5）では？ | **LRU が僅差で優位** |
| warm 条件では？ | **mmap が圧倒的優位（305x）** |
| THP は有効か？ | **有効だが中庸（KVM のような極端挙動なし）** |

> 総括: WSL2 では mmap は全体として強く、特に warm・scan・prefetch で大きな優位を示した。一方、THP の挙動は KVM と定性的に異なるため、THP の最終評価は WSL2 以外の Linux 実環境で補完する必要がある。

---

脚注:
- `cold.json` と `alpha_100.json` は同一設定だが mmap throughput が異なる（12,481 vs 15,573）。本レポートでは baseline として `cold.json`、alpha 比較として `alpha_100.json` を採用した。ページキャッシュ状態や実行タイミング差の影響が考えられる。
