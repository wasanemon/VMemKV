# VMemKV

OS の仮想メモリ機構（`mmap` / `fork` / `mincore`）に I/O 管理を委譲する、Larger-than-memory KVS。

## ドキュメント

### 設計

| ドキュメント                                                                       | 概要                                                                |
| ---------------------------------------------------------------------------------- | ------------------------------------------------------------------- |
| [docs/specification/high level design.md](docs/specification/high_level_design.md) | VMemKV 高レベル設計（目的・構成・比較・適用範囲）                   |
| [docs/specification/low level design.md](docs/specification/low_level_design.md)   | VMemKV 低レベル設計（データ構造・操作手順・最適化・パラメータ）     |
| [docs/latency_survey.md](docs/latency_survey.md)                                   | ストレージレイテンシ調査：環境別ランダム 4 KB read レイテンシ参考値 |

### マイクロベンチマーク（mmap vs pread+LRU）

| ドキュメント                                                     | 概要                                                    |
| ---------------------------------------------------------------- | ------------------------------------------------------- |
| [microbenchmark/REPORT.md](microbenchmark/REPORT.md)             | 総合レポート（クロスプラットフォーム比較・設計根拠）    |
| [microbenchmark/macos/REPORT.md](microbenchmark/macos/REPORT.md) | macOS レポート（Apple M4 Pro / Apple SSD）              |
| [microbenchmark/linux/REPORT.md](microbenchmark/linux/REPORT.md) | Linux レポート（Intel Xeon KVM / 仮想ブロックデバイス） |
| [microbenchmark/README.md](microbenchmark/README.md)             | ベンチマークのビルド・実行手順                          |

### KV ストア性能評価（T1Index vs RocksDB）

| ドキュメント                                             | 概要                                                                  |
| -------------------------------------------------------- | --------------------------------------------------------------------- |
| [docs/benchmark/20260430.md](docs/benchmark/20260430.md) | T1Index vs RocksDB ベンチマークレポート（macOS arm64 / Apple M4 Pro） |

## リポジトリ構成

```
.
├── docs/
│   ├── specification/
│   │   ├── high_level_design.md  # VMemKV 高レベル設計
│   │   └── low_level_design.md   # VMemKV 低レベル設計
│   ├── benchmark/
│   │   └── kv_store_evaluation_20260428.md  # KV ストア性能評価レポート
│   └── latency_survey.md         # ストレージレイテンシ調査
├── src/
│   ├── core/
│   │   └── snaplog.hpp           # SnapLog<T> コア実装
│   └── kvs/
│       ├── store_types.hpp       # 共有キー型定義
│       ├── t1_index.hpp          # T1Index（SnapLog 薄ラッパー、スレッドセーフ）
│       └── rocksdb_store.hpp     # RocksDB アダプター（オプション）
├── tests/
│   ├── test_snaplog.cpp          # SnapLog 単体テスト（doctest）
│   ├── test_kv_store.cpp         # KVStore パラメトリックテスト（T1Index + RocksDB）
│   └── CMakeLists.txt
├── benchmark/
│   ├── bench_kv.cpp              # nanobench ベンチマーク本体（テンプレート化）
│   ├── run_bench.sh              # 実験自動化スクリプト（実行ごとに logs/ へ自動保存）
│   ├── logs/                     # ベンチマーク実行ログ（YYYYMMDD_HHMMSS_*.txt）
│   └── CMakeLists.txt
└── microbenchmark/               # mmap vs pread+LRU マイクロベンチマーク（別実験）
    ├── bench.cpp
    ├── run_bench.sh
    ├── REPORT.md
    ├── macos/
    └── linux/
```

---

## ビルド・テスト・ベンチマーク

### 前提

| ツール          | バージョン                                            |
| --------------- | ----------------------------------------------------- |
| CMake           | 3.16 以上                                             |
| C++ コンパイラ  | C++20 対応（GCC 12 / Clang 16 以上）                  |
| RocksDB（任意） | `brew install rocksdb` / `apt install librocksdb-dev` |

### ビルド

```bash
# ソースルートに移動
cd VMemKV

# 設定（RocksDB なし）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 設定（RocksDB あり）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_ROCKSDB=ON

# ビルド（全ターゲット）
cmake --build build --parallel
```

### テスト

```bash
# ビルド後に実行
ctest --test-dir build --output-on-failure
```

または直接実行：

```bash
./build/tests/test_snaplog    # SnapLog 単体テスト
./build/tests/test_kv_store   # KVStore パラメトリックテスト（T1Index + RocksDB）
```

### ベンチマーク

#### 自動実験スクリプト（推奨）

```bash
# RocksDB あり（インストール済みの場合は自動検出）
bash benchmark/run_bench.sh

# RocksDB なし
bash benchmark/run_bench.sh --no-rocksdb

# 追加で任意のファイルにも保存
bash benchmark/run_bench.sh --output results/$(date +%Y%m%d).txt

# 自動ログを無効化して実行
bash benchmark/run_bench.sh --no-log
```

実行するたびに `benchmark/logs/YYYYMMDD_HHMMSS_{rocksdb|no_rocksdb}.txt` へ自動保存される。

オプション一覧：

| オプション         | デフォルト | 説明                                   |
| ------------------ | ---------- | -------------------------------------- |
| `--no-rocksdb`     | —          | RocksDB を無効化                       |
| `--build-type <T>` | `Release`  | CMake ビルドタイプ                     |
| `--build-dir <D>`  | `build`    | ビルドディレクトリ                     |
| `--output <F>`     | —          | 結果をファイルにも保存                 |
| `--no-log`         | —          | `benchmark/logs/` への自動保存を無効化 |

#### 手動実行

```bash
cmake --build build --target bench_kv
./build/benchmark/bench_kv
```

### ベンチマーク結果

詳細・考察・今後の課題は各レポートを参照。

| 日付       | 環境                       | 対象               | レポート                                                 |
| ---------- | -------------------------- | ------------------ | -------------------------------------------------------- |
| 2026-05-01 | ローカルベンチ環境         | T1Index vs RocksDB | [docs/benchmark/20260501_t1index_vs_rocksdb.md](docs/benchmark/20260501_t1index_vs_rocksdb.md) |

### 最新レポート

- [docs/benchmark/20260501_t1index_vs_rocksdb.md](docs/benchmark/20260501_t1index_vs_rocksdb.md): T1Index の全ベンチマーク結果に対する総合考察
