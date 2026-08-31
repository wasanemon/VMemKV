# ベンチマーク/テスト用ファイルシステムの選定

VMemKVのT2ストレージは`pwrite()`/`fsync()`/`ftruncate()`/`mmap()`/`rename()`のみで実装されており、reflink(`ioctl(FICLONE)`)や`FALLOC_FL_PUNCH_HOLE`など、特定ファイルシステムに依存する機能を一切使わない。そのため、ベンチマーク・テストで使うNVMe/ループバックボリュームは標準的な`ext4`でよく、XFSである必要はない。

## 背景: なぜ一時期XFS(`reflink=1`)が必要だった

以前の`checkpoint_and_defragment()`(reflinkクローン + hole punchによる差分デフラグ機構)は、`ioctl(FICLONE)`によるreflinkクローンを利用しており、これはXFSやbtrfsなどreflink対応ファイルシステムでしか動作しない。このためAWSベンチマークのNVMeフォーマットは`mkfs.xfs -m reflink=1`、ローカル開発環境(ext4ルートのWSL2等)向けにはループバックXFSボリュームを用意するスクリプトが存在した。

## 廃止した理由

直接計測により、`FALLOC_FL_PUNCH_HOLE`は**穴あけ対象のバイト範囲がファイルシステムのブロックサイズ(通常4KB)に満たない場合、何も回収しない**ことが判明した。本プロジェクトの典型的な小さい値サイズのワークロードでは、デッドになった範囲が4KBブロックをちょうど1つ丸ごと覆うことは稀で、実運用上ほぼ何も回収できない。つまりreflink+punch機構の「回収」を担う側(punch)が実効性を持たず、reflinkによる差分コピーの高速化だけが残っても機構全体としての価値がなかった。

この結果を受けて`checkpoint_and_defragment()`は廃止し、`checkpoint_internal()`(既存offsetへのin-place `pwrite()`)と`defragment_internal()`(生存データ全件を新規ファイルへ再配置し`rename()`で丸ごと差し替え)に分離した。どちらもreflinkやpunch_holeを一切必要としない。ファイル丸ごとの置き換え(`rename()`)は、パンチのようなブロック粒度の制約を受けずに100%の空間回収ができる。

> **追記**: `defragment_internal()`とその公開API `defragment()`はその後round 3でコードベースから
> 完全に削除された。現在は`checkpoint_internal()`のみが稼働している。reflink/punch-holeを不要と
> する本ドキュメントの結論(ext4で十分)自体は変わらない。

## 現在の方針

- AWSベンチマーク用NVMeフォーマット: `ext4`(`run_bench_aws_c6id.sh`)
- テスト用一時ディレクトリ: システム標準の一時ディレクトリ(`VMEMKV_TEST_TMPDIR`で上書き可能だが、特定ファイルシステムを要求しない)
- CIのXFSループバックボリューム作成ステップは削除済み
