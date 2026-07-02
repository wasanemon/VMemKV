# VMemKV

OS の仮想メモリ機構（`mmap` / `fork` / `mincore`）に I/O 管理を委譲する、Larger-than-memory KVS。

## ドキュメント

### 設計

| ドキュメント                                                                       | 概要                                                                |
| ---------------------------------------------------------------------------------- | ------------------------------------------------------------------- |
| [docs/specification/high level design.md](docs/specification/high_level_design.md) | VMemKV 高レベル設計（目的・構成・比較・適用範囲）                   |
| [docs/specification/low level design.md](docs/specification/low_level_design.md)   | VMemKV 低レベル設計（データ構造・操作手順・最適化・パラメータ）     |
| [docs/latency_survey.md](docs/latency_survey.md)                                   | ストレージレイテンシ調査：環境別ランダム 4 KB read レイテンシ参考値 |

---

## 実験結果

今後実施される性能検証実験やアブレーション研究のレポートは、こちらに追加されます。

| 日付 / 種類 | 評価対象 | 概要・レポート |
| :--- | :--- | :--- |
| **アブレーション研究 (Ablation Study)** | VMemKV 6大最適化の性能影響度評価 | `src/config.hpp` にて体系化された各種最適化（T1インデックス最適化 ＋ T2値インライン化）が、段階的・個別剥離によってスループットおよびレイテンシに与える効果の評価。 |

---

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
./build/tests/test_kv_store   # 各バリアントに対する正確性検証
```

### clang-tidy（静的解析）

```bash
# リポジトリルートから実行
cmake -S implementation -B build-clang \
	-DCMAKE_CXX_COMPILER=clang++ \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_WARN_DEPRECATED=OFF \
	-DENABLE_ROCKSDB=ON

cmake --build build-clang --target clang-tidy-check
```

現在の `.clang-tidy` は `CheckOptions` を追加せず、デフォルト寄りの設定を維持している。  
`readability-function-cognitive-complexity` など doctest マクロ起因で過剰に厳しくなる箇所は、テストコード側で局所的に `NOLINTNEXTLINE(...)` を付けて吸収する方針。

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
