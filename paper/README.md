# paper

このディレクトリには、VMemKV の論文執筆に関連するメモ、下書き、評価計画などを置いています。

## まず見るべきもの

- `evalutation/`
  - 現時点で最も重要な内容です。
  - 評価計画、ワークロード整理、追加で読むべき文献などをまとめています。
  - 論文作業を進める場合は、まずこのディレクトリの内容を確認してください。

## 優先度が低いもの

- `draft/`
  - 論文本文や構成案の下書きです。
  - 現時点では評価まわりの整理より優先度は低めです。

- `others/`
  - 関連メモや補助的な整理資料です。
  - 現時点では主要な参照先ではありません。

## 注意

このディレクトリ内の資料には、実装側の設計変更を追いきれていない部分があります。
そのため、現在の実装と内容が乖離している可能性があります。

論文や評価方針に反映する際は、必ず最新の実装と照らし合わせて確認してください。

## 現在のシステム仕様の扱い

2026-07-04 時点では、`paper/` 配下のメモ・下書き・評価計画は、現在の
VMemKV 実装方針より古い情報を含んでいる可能性があります。
したがって、システムとしての最終状態や実装方針を判断するときは、
このディレクトリではなく `implementation/` 側の設計文書と
`implementation/TODO.md` を基準にしてください。

特に、`implementation/TODO.md` に記載された残 TODO が反映された状態を
最終状態とみなす場合、VMemKV のシステム構想はほぼ固まっているものとして扱います。
この前提では、主な確定方針は次の通りです。

- RAM 常駐の Tier 1 index と、mmap-backed な Tier 2 value layer を分離する。
- Tier 2 は `MAP_PRIVATE` mmap を前提とし、通常実行中の T2 file writeback を
  durability path にはしない。
- durability は WAL、checkpoint、recovery によって担保する。
- checkpoint / reload は `fork()` 前提ではなく、in-process の background
  serialization / checkpointing に寄せる。
- reorganize は Tier 1 の ordering fragmentation と Tier 2 の storage
  fragmentation を修復し、checkpoint / generation update と連携する。

このため、`paper/` 内に残っている `fork`, `mincore`, checkpoint flow,
評価計画、論文上の主張などは、確定情報として扱わないでください。
