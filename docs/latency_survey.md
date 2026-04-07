# ストレージレイテンシ調査：環境別ランダム 4 KB read レイテンシ

FaultKV の mmap vs pread+LRU 比較における「クロスオーバー点」を特定するための参考資料。

## 環境別の代表的なレイテンシ（p50 目安）

| ストレージ                               | レイテンシ  | 出所・備考                                             |
| ---------------------------------------- | ----------- | ------------------------------------------------------ |
| DRAM                                     | ~100 ns     | [Latency Numbers Every Programmer Should Know][norvig] |
| Apple M4 Pro SSD（Apple Fabric 直結）    | **~1 µs**   | 本プロジェクト実測値                                   |
| Intel Optane P5800X（3D XPoint NVMe）    | ~10–20 µs   | SPDK + O_DIRECT 測定の典型値                           |
| ハイエンド consumer NVMe（PCIe 4.0/5.0） | ~50–100 µs  | Samsung 990 Pro 等                                     |
| エンタープライズ NVMe（直接接続）        | ~100–200 µs | FAST 論文群の典型値                                    |
| KVM 仮想ブロックデバイス                 | **~150 µs** | 本プロジェクト実測値                                   |
| AWS EBS gp3                              | ~1–3 ms     | "single-digit millisecond"                             |
| Azure Premium SSD                        | ~1–3 ms     | [Azure managed disk types][azure-disk]                 |
| Azure Ultra Disk / AWS io2 Block Express | ~200–500 µs | "sub-millisecond"                                      |
| HDD                                      | ~5–15 ms    | ディスクシーク + 回転待ち                              |

## 参考文献

- [Latency Numbers Every Programmer Should Know (Colin Scott, 2020)][norvig]
- [Azure managed disk types][azure-disk]
- [Are You Sure You Want to Use MMAP in Your Database Management System? (Pavlo ら, CIDR 2022)][pavlo]

[norvig]: https://colin-scott.github.io/personal_website/research/interactive_latency.html
[azure-disk]: https://learn.microsoft.com/en-us/azure/virtual-machines/disks-types
[pavlo]: https://db.cs.cmu.edu/papers/2022/cidr2022-p13-crotty.pdf

## FaultKV への含意

クロスオーバー点はおよそ **50–100 µs**（エンタープライズ NVMe 直結の上端）。

| ストレージ速度                     | pread miss vs page fault | FaultKV 競争力 |
| ---------------------------------- | ------------------------ | -------------- |
| < 5 µs（最新 NVMe）                | pread ≪ page fault       | 劣る           |
| ~100–200 µs（KVM / エンプラ NVMe） | pread ≈ page fault       | 同等以上       |
| ~1–3 ms（クラウド EBS）            | pread ≫ page fault       | 有利           |
| ~5–15 ms（HDD）                    | pread ≫ page fault       | 圧倒的有利     |
