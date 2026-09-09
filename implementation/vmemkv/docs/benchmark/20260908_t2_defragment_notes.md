# 2026-09-08 T2 defragment() 実装メモ

`docs/benchmark/20260907_t2_defragment_redesign.md` の方向に沿った phases 0+1 の実装記録。
同 doc からの意図的な乖離とその理由、検証結果、残件を記す。

## 乖離1: relocation は stripe lock + offset 一致検証付き (doc §3 はロック不要と主張)

doc は「T1 の put は upsert のため、新旧どちらの順序で着地しても live 集合は保たれる」
として compactor 側の追加ロックを不要としている。これは live「集合」については正しいが、
値の新旧については誤りである: relocation が古いバイトを読んだ後に前景 update が新 offset
を put し、その後に relocation の put が着地すると、成功応答済みの update が失われる
(lost update)。LLD 6章の同一キー直列化契約にも反する。実装は update_impl と同じ
stripe lock 下で T1 現値を再読し、bucket offset と一致した場合のみ移設する。

## 乖離2: punch は WAL-durable + 1サイクル quarantine で行い、checkpoint 後を待たない

doc §1 の順序制約 (punch は新 offset を取り込んだ checkpoint の後) は十分条件だが、
WAL replay が移設を通常 update として再現できる以上、WAL fsync 時点で既に安全である。
WAL rotate は manifest がカバーする世代しか落とさないため、移設記録が checkpoint
なしに消えることはない。quarantine (排出と punch の間に1サイクル) は T1 chk のため
ではなく、punch 前の offset を掴んだままの in-flight reader の窓を有界にするためである。

## 乖離3: 背景プールではなく単一専用スレッド

T1 のプールは `ShardSlot*` 型専用で流用できず、reorg worker との共有は長時間移設が
checkpoint 駆動を遅らせる。移設は逐次 append + WAL であり並列化の利得がないため、
単一の専用スレッド (1秒 poll、安価な atomic 判定のみ) とした。

## 乖離4: punch 非対応 FS ではサイクル全体を実行しない

hole-punch できない FS で移設だけ回すと `bytes_used` が増える一方になり、
frontier セグメントを無限に再 victim 化して T2 容量で throw する。初回に一度だけ
対応可否を判定 (fstatfs + 初回 punch 失敗でラッチ) し、非対応では何もしない。

## 検証済み (tests/test_defrag.cpp)

- 会計の exactness: append / 同サイズ更新 (in-place、変動なし) / 拡大更新 / delete 後の
  `t2_live_bytes` が hint 算術と完全一致。
- サイクル: 移設後の全 live get 一致、2サイクル目で punch (st_blocks 減少)、移設なし
  サイクルでは moved 0。
- 回復: defrag + checkpoint + 再起動後の全 live 一致と会計一致。
- 並行: 更新スレッド群と並行した強制サイクル後の最終値一致。
- 全 313 テスト緑。

## 残件 (AWS 要)

- Phase 2: `ltm/1KB` + `ltm/64KB` 32-writer contention probe での劣化率 <=20% 測定。
- Phase 3: 長時間 churn でのフットプリント頭打ち実証。
- 8MiB セグメント固定の妥当性は未測定 (変更は config 1行だが、測定なしに変えない)。
