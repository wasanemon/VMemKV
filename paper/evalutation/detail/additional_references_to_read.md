# Additional References to Read for VMemKV

Status: `must_read_papers/` に無いが、VMemKV の主張・評価・Related Work に影響しうる追加文献の候補リスト。`literature_workload_matrix.md` §8 の完全版。

書誌情報の規約: ここに挙げる venue / 年は執筆時点の記憶ベースの候補であり、**引用前に必ず原典で確認すること**。確度が低いものは Needs verification を付す。読む優先度 — **P0**: 論文の主張の正否に直結、投稿前必読 / **P1**: 評価・Related Work の質を大きく上げる / **P2**: あると良い。

---

## 1. OS paging 委譲・mmap KVS の直接の先行研究（新規性防衛線）

| Paper / system | なぜ必要か | 関係する主張・評価 | 優先度 | 評価計画への影響 | 実性能 baseline? | RW/design comparison で十分? | must_read_papers に追加? | 備考 |
|---|---|---|---|---|---|---|---|---|
| **Stoica & Ailamaki, "Enabling Efficient OS Paging for Main-Memory OLTP Databases" (DaMoN 2013)** | 「hot 構造を RAM に固定し cold データを OS paging に委譲」という VMemKV の骨格そのものの直接の祖先。Crotty CIDR'22 の参考文献経由で査読者に既知 | Contribution 1（OS 委譲の新規性表明）。引かないと事実誤認 | **P0** | 新規性の言い換えが必須になる（「初の提示」→「KVS への適用と定量特徴づけ」） | 不要 | RW で十分 | **追加すべき** | Needs verification（正確な書誌） |
| **Tucana (Papagiannis et al., USENIX ATC 2016)** | mmap 上に KV engine を構築し、その限界に当たった工学系譜の起点 | Related Work §10.4、新規性 | P1 | mmap KVS 系譜の一段落が必要に | 不要 | RW で十分 | 追加推奨 | ScaleCache Sec 2.2 でも言及 |
| **Kreon (Papagiannis et al., SoCC 2018 / ACM TOS)** | 同系譜。memory-mapped I/O 用にカーネル側 mmio path まで自作した＝「素の mmap では足りなかった」証言 | 同上 | P1 | 同上 | 不要 | RW | 追加推奨 | Needs verification |
| **FastMap (Papagiannis et al., USENIX ATC 2020)** | "Optimizing Memory-mapped I/O for Fast Storage Devices"。mmap fault path のスケール限界を定量化しカーネル改造で回避 | E1 の設計（fault path 律速の予測値）、Related Work | P1 | TLB/fault 計測の期待値の校正 | 不要 | RW | 追加推奨 | |
| **MongoDB MMAPv1 → WiredTiger 移行の記録** | production mmap storage engine の撤退事例（2015 deprecate / 2019 削除, Crotty Sec 2.3/Table 1） | Limitations、Crotty Table 1 の failure mode への個別回答 | P2 | 回答表を書くなら素材 | 不要 | RW 一文 | 不要（Crotty 経由で引用可） | 一次資料は blog/docs |
| **LMDB（正式引用）** | mmap incumbent。vmcache / PrediCache の標準 baseline | E2/E5 の secondary baseline（main-text 1 図に昇格を提案済み） | **P0**（baseline として） | LMDB 統合の実装タスクが発生 | **必要（secondary/diagnostic）** | 比較もする | **追加すべき** | 引用形式（Chu の LADIS'11 MDB report か software citation か）Needs verification |

## 2. RAM-index + SSD value / SSD-conscious KVS（最近傍の非 mmap 設計）

| Paper / system | なぜ必要か | 関係する主張・評価 | 優先度 | 評価計画への影響 | 実性能 baseline? | RW で十分? | 追加? | 備考 |
|---|---|---|---|---|---|---|---|---|
| **KVell (Lepers et al., SOSP 2019)** | full in-RAM index + unordered SSD slabs + direct I/O。「mmap を使わない VMemKV」に最も近い設計。F2 Sec 2.1/Table 2 が KVell の index page-out 時 RA/WA 25–95x を実測——**T1 が RAM から溢れた場合の VMemKV の未来図** | T1 footprint 論法、Related Work、E1 の設計 | **P0** | T1 footprint 表に「index が溢れたら何が起きるか」の脚注を追加 | 不要（F2 の実測を引用） | RW で十分 | **追加すべき** | |
| **SILT (Lim et al., SOSP 2011) / FAWN (SOSP 2009)** | 「index の bytes/key を極小化する」系譜の古典。F2 の <1 B/key の先祖 | T1 32 B/key の位置づけ（対極） | P2 | footprint 表の文脈付け | 不要 | RW 一文 | 任意 | |
| **Titan (TiKV / PingCAP)** | WiscKey 系 value separation の実装（RocksDB fork）。BlobDB の代替候補 | E2 の baseline 選定の防御（なぜ BlobDB を選んだか） | P2 | BlobDB 採用理由の一文 | 不要（BlobDB で足りる） | RW | 任意 | 論文なし、docs 引用。Needs verification |
| **HashKV (Chan et al., USENIX ATC 2018)** | update-heavy 下の value-log GC 改善。VMemKV の reorganization と同じ問題への別解 | E4（reorganization の Related Work）、update-heavy 評価の設計 | P1 | GC/reorg 干渉実験の対比軸 | 不要 | RW | 追加推奨 | |
| **BadgerDB (Dgraph)** | WiscKey の Go 実装（production）。「value separation は実用化済み」の証拠 | Related Work | P2 | なし | 不要 | RW 一文 | 不要 | docs 引用 |
| **PebblesDB (Raju et al., SOSP 2017)** | LSM の WA 削減（fragmented LSM）。WA 比較の文脈 | E2 WA の Related Work | P2 | なし | 不要 | RW 一文 | 任意 | |
| **SplinterDB (Conway et al., USENIX ATC 2020)** | F2 の baseline。NVMe 向け B^ε-tree | Related Work（F2 引用時に自然に登場） | P2 | なし | 不要 | RW | 不要 | |

## 3. LSM / compaction / tail latency

| Paper / system | なぜ必要か | 関係する主張・評価 | 優先度 | 評価計画への影響 | 実性能 baseline? | RW で十分? | 追加? | 備考 |
|---|---|---|---|---|---|---|---|---|
| **SILK (Balmau et al., USENIX ATC 2019)** | LSM の compaction 起因 tail latency を I/O スケジューリングで制御。VMemKV の「reorganization 中の foreground p99」実験（E4a）の直接の対置 | E4a の動機と比較の語彙 | P1 | E4a の関連研究として設計に反映 | 不要 | RW | 追加推奨 | |
| **LSM survey (Luo & Carey, VLDB Journal 2020)** | compaction の分類語彙。「LSM-style multi-level compaction を中心機構にしない」の厳密化 | §6.6 の用語防衛 | P2 | なし | 不要 | RW | 任意 | |
| **Bigtable / LevelDB / RocksDB** | corpus 済み（bigtable.pdf, rocksdb.pdf） | — | — | — | — | — | 済み | |

## 4. crash recovery / durability / WAL

| Paper / system | なぜ必要か | 関係する主張・評価 | 優先度 | 評価計画への影響 | 実性能 baseline? | RW で十分? | 追加? | 備考 |
|---|---|---|---|---|---|---|---|---|
| **ALICE (Pillai et al., OSDI 2014)** | crash-state 網羅の標準方法論。WiscKey も採用（Sec 4.2.4）。E6 を実施するなら方法の正統性の根拠 | E6（recovery sanity check）の設計 | **P1（E6 実施時は P0）** | E6 の手順を「ALICE-style-lite」として記述 | 不要 | 方法論 | 追加推奨 | |
| **Aether (Johnson et al., VLDB 2010)** | group commit / early lock release / flush pipelining。LLD 7.2 が既に参照 | WAL 実装時の最適化引用 | P1（WAL 実装時） | なし（実装の参考） | 不要 | RW | LLD が参照済み、書誌確定を | Needs verification（正確な書誌） |
| **HyPer fork-snapshot (Kemper & Neumann, ICDE 2011)** | fork CoW snapshot の先行研究。fork checkpoint を「残す」場合の必須引用 | §6.4（checkpoint 機構）の新規性境界 | **P0（fork 継続時）/ P2（廃止時）** | fork 継続なら E4b の位置づけが変わる | 不要 | RW | fork 決定後に追加 | TODO.md は fork 廃止方針で宙吊り |
| **Redis BGSAVE（fork CoW 永続化）** | 同上の production 事例 | 同上 | P2 | なし | 不要 | RW 一文 | 不要（docs 引用） | |
| **Failure-atomic msync (Park et al., EuroSys 2013)** | msync 境界の原子性を保証する研究。VMemKV が msync を「使わない」理由の説明に使える | mapping mode 決定の議論（§4.3 想定） | P2 | なし | 不要 | RW 一文 | 任意 | |

## 5. mmap / page cache / kernel 側の進化（「今なら委譲できる」側の材料）

| Paper / system | なぜ必要か | 関係する主張・評価 | 優先度 | 評価計画への影響 | 実性能 baseline? | RW で十分? | 追加? | 備考 |
|---|---|---|---|---|---|---|---|---|
| **MGLRU（Multi-Gen LRU, Linux 6.1+; lwn/カーネル文書）** | Crotty CIDR'22 の実験は Linux 5.11。eviction 挙動はその後変化。E1 の結果解釈と「post-CIDR'22 kernel」という主張の根拠 | E0（kernel version の意味付け）、§10.4 の定量的応答 | **P1** | E0 に kernel 世代の明記と、可能なら MGLRU on/off の 1 実験 | 不要 | docs 引用 | 追加推奨 | 論文ではなく kernel docs |
| **DAMON (Park et al., HPDC/ASPLOS 系)** | kernel 側の access-aware memory 管理。「OS は賢くなりうる」側 | Related Work 一文 | P2 | なし | 不要 | RW | 任意 | Needs verification |
| **TPP (Maruf et al., ASPLOS 2023) / Pond (Li et al., ASPLOS 2023) / TMO (Weiner et al., ASPLOS 2022)** | tiered memory / CXL 時代の「kernel 全域 tiering」。「なぜ今 OS 委譲か」の最も現代的な動機付け | Introduction の motivation 補強 | P2 | なし | 不要 | RW | 任意 | |
| **Haas & Leis, "What Modern NVMe Storage Can Do, and How to Exploit It" (VLDB 2023)** | NVMe を飽和させるのに必要な並列度・async I/O の定量。pread twin / fio bound の設計根拠 | E5 の設計、Limitations | P1 | fio bound と thread 数設定の根拠 | 不要 | RW | 追加推奨 | Needs verification（正確なタイトル） |
| **exmap** | vmcache.pdf 内（Sec 5.5–5.7）で扱われており追加 PDF 不要 | — | — | — | — | — | 済み | |

## 6. benchmark 方法論

| Paper / system | なぜ必要か | 関係する主張・評価 | 優先度 | 評価計画への影響 | 実性能 baseline? | RW で十分? | 追加? | 備考 |
|---|---|---|---|---|---|---|---|---|
| **YCSB (Cooper et al., SoCC 2010)** | E2 の workload 定義の正典 | E2 全体 | **P0** | 既に計画済み。引用のみ | benchmark | — | **追加すべき** | |
| **RocksDB db_bench / tuning guide（公式 docs）** | E2 の RocksDB 側公平設定（block cache, WAL, compression, direct I/O, background jobs, rate limiter, BlobDB options）の根拠 | E0/E2 | **P1** | E0 の設定表を db_bench 既定値と対応付ける | — | docs | 追加推奨 | docs 引用 |
| **「Benchmarking crimes」系 (Heiser の checklist 等)** | 公平性チェックリストとして内部レビューに有用 | E0 | P2 | なし | — | — | 不要 | |

---

## 採否サマリ

- **must_read_papers に追加すべき（P0）**: Stoica & Ailamaki (DaMoN'13)、KVell (SOSP'19)、LMDB（正式引用 + baseline 化）、YCSB (SoCC'10)、（fork 継続決定時のみ）HyPer (ICDE'11)。
- **追加推奨（P1）**: Tucana / Kreon / FastMap、HashKV、SILK、ALICE、MGLRU 文書、Haas & Leis (VLDB'23)、RocksDB tuning docs、Aether の書誌確定。
- **Related Work 一文で十分（P2）**: SILT/FAWN、Titan、BadgerDB、PebblesDB、SplinterDB、LSM survey、MongoDB MMAPv1、Redis BGSAVE、failure-atomic msync、DAMON、TPP/Pond/TMO、LevelDB。
- **実性能 baseline として必要なのは追加分では LMDB のみ**。他はすべて Related Work / design comparison / 方法論ソースで足りる。

## 関連の薄い候補（採用しない理由）

- **WiredTiger**: vmcache/PrediCache の baseline として登場するが、VMemKV の KVS 比較には RocksDB/LMDB で十分。RW 一文。
- **Bw-Tree / Masstree 等 in-memory index 系**: T1 の実装選択の文脈でしかなく、評価には影響しない。
- **ZNS / FDP 系（How to Write to SSDs の後半）**: VMemKV は conventional SSD 前提で十分。future work 一文。
