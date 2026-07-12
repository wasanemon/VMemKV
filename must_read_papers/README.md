# Must Read Papers

| 論文名 | VMemKVとの関係 |
|---|---|
| [WiscKey](/must_read_papers/wisckey.pdf) | 「LSM-TreeのMemtableのvalueにはポインタだけを入れる」「Valueは別のファイルにひたすらappend-only」というアプローチを採用した論文。この２つのアイデアはVMemKVとほぼ同じ。|
| |



# 先行研究の評価実験設定値一覧

| 先行研究 (発表年) | レコード数 /<br>データセット容量 | Key / Value サイズ | Valueの特徴<br>(固定長/可変長/混合) | メモリ (バッファ)<br>容量 | メモリ制限の<br>実現手法 | データセット /<br>メモリ比率 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **F2** (2025) | 2.5億件 /<br>約30GB | Key: 8B, 48B<br>Value: 100B等 | YCSBでは**固定長**、実環境(MixGraph)では**可変長** | 3GB など | Linux **cgroups**によるOSレベルの制限、および内部設定 | 10倍 <br>(実験により4〜40倍) |
| **How to Write to SSDs** (2026) | (記載なし) /<br>160GB〜800GB | - | - | 16GB〜80GB程度 | DBMSの**バッファプール容量**の設定 | 5倍 〜 20倍 |
| **Predictive Translation** (2026) | 1億件 / 20GB<br>50億件 / 1TB | Key: 8B<br>Value: 120B | **固定長** | 256GB (In-mem)<br>128GB (OOM) | バッファマネージャーの設定 | 約8倍 (OOM時) |
| **vmcache** (2023) | 50億件 / 1TB | Key: 8B<br>Value: 120B | **固定長** (推測) | 128GB | バッファプールの容量設定 | 約8倍 |
| **KVell** (2019) | 1億件 / 100GB<br>50億件 / 5TB<br>実環境 / 256GB | Key: (記載なし)<br>Value: 250B〜1KB | YCSBでは**固定長**、実環境(Nutanix)では**複数サイズの混合** | データセットの1/3 | 利用可能なRAM予算の割り当て | 3倍 |
| **FASTER** (2018) | 2.5億件 /<br>約27GB | Key: 8B<br>Value: 8B, 100B | **固定長** | 連続的に変化 | 最大メモリバジェットの指定 | In-memoryからOOMまで変化 |
| **WiscKey** (2016) | (記載なし) /<br>100GB | Key: 16B<br>Value: 64B〜256KB | 実験ごとに異なる**固定長**サイズで評価 | 64GB | テストマシンの**物理メモリ上限** | 約1.5倍強 |
| **Enabling Efficient OS Paging** (2013) | (TPC-C scale 16) /<br>数十GB | Key: 4B<br>Value: 200B (想定) | **固定長** | 3GB 〜 161GB | OSが利用可能な**物理DRAM量**を意図的に制限 | ページング発生まで変化 |
