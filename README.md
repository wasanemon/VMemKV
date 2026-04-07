# FaultKV

OS の仮想メモリ機構（`mmap` / `fork` / `mincore`）に I/O 管理を委譲する、Larger-than-memory KVS。

## ドキュメント

### 設計

| ドキュメント                                         | 概要                                                                |
| ---------------------------------------------------- | ------------------------------------------------------------------- |
| [docs/specification/high level design.md](docs/specification/high%20level%20design.md) | FaultKV 高レベル設計（目的・構成・比較・適用範囲）                  |
| [docs/specification/low level design.md](docs/specification/low%20level%20design.md) | FaultKV 低レベル設計（データ構造・操作手順・最適化・パラメータ）    |
| [docs/latency_survey.md](docs/latency_survey.md)     | ストレージレイテンシ調査：環境別ランダム 4 KB read レイテンシ参考値 |

### マイクロベンチマーク（mmap vs pread+LRU）

| ドキュメント                                                     | 概要                                                    |
| ---------------------------------------------------------------- | ------------------------------------------------------- |
| [microbenchmark/REPORT.md](microbenchmark/REPORT.md)             | 総合レポート（クロスプラットフォーム比較・設計根拠）    |
| [microbenchmark/macos/REPORT.md](microbenchmark/macos/REPORT.md) | macOS レポート（Apple M4 Pro / Apple SSD）              |
| [microbenchmark/linux/REPORT.md](microbenchmark/linux/REPORT.md) | Linux レポート（Intel Xeon KVM / 仮想ブロックデバイス） |
| [microbenchmark/README.md](microbenchmark/README.md)             | ベンチマークのビルド・実行手順                          |

## リポジトリ構成

```
.
├── docs/
│   ├── specification/
│   │   ├── high level design.md # FaultKV 高レベル設計
│   │   └── low level design.md  # FaultKV 低レベル設計
│   └── latency_survey.md     # ストレージレイテンシ調査
└── microbenchmark/
    ├── bench.cpp              # ベンチマーク本体
    ├── run_bench.sh           # 統合実験スクリプト（macOS / Linux 自動判定）
    ├── REPORT.md              # クロスプラットフォーム総合レポート
    ├── macos/                 # macOS 実験結果 JSON + レポート
    └── linux/                 # Linux 実験結果 JSON + レポート
```
