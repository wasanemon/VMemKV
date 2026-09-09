# T2 Defragment設計

T2のStorage Fragmentation(T1から参照されなくなった旧版・削除済み版の蓄積)を、
前景スレッドを止めずに解消する設計。live recordをappend frontierへ移設し、
空になったセグメントをhole-punchする。

## 単位

- T2を固定8MBのセグメントに分割する。
- ページ単位の穴埋めは行わない。生死混在ページから死だけ抜くことはできないため。

## 手順

1. T1全走査でセグメントごとのliveバイトを数え直し、凍結済み (`seg_end <= base_boundary`)
   かつgarbage率50%以上のセグメントをgarbageの多い順に victim とする。移動量は
   1サイクル1GiB上限。
2. 再走査で victim 範囲 (`[S_start - 1MiB, S_end)`) に開始点を持つlive offset を集め、
   昇順・重複除去する。1MiB slop はセグメント境界を跨ぐrecordのtailを逃さないためで、
   これによりpunchはセグメント全体を対象にできる。
3. 各offsetをキー単位stripe lock下で再検証 (T1現値と一致した場合のみ) し、新tailへ
   append、T1 offset 付け替え、WAL update 予約を行う。全予約をまとめて await する
   (group commit が fsync を融合)。
4. 空になったことが確認できた victim を次サイクルのpunch待ち行列へ回す
   (quarantine: 排出とpunchの間に1サイクル空ける)。
5. 前サイクルの待ち行列をpunchする。既に hollow な範囲は数えず、実 dealloc のみ計上する。

## 並行性

- 前景読は止めない。移設前後のどちらの版を読んでも正しい値か、punch後のゼロ (miss 扱い)
  のいずれかになる。punch は前サイクル排出分に限るため窓は1サイクル分に有界。
- 前景書との同一キー競合は stripe lock + 移設前の offset 一致検証で直列化する。
  検証なしの put は古い値で新しい更新を上書きし得るため、ロックフリー再配置はしない。
- checkpoint との排他はいらない。punch 範囲は msync 範囲と重ならず、T1 put と T1
  checkpoint 取り込みは通常の並行書き込みと同じ扱いになる。

## 起動・停止

- 許容space overheadを唯一の公開パラメータとする(既定20%)。
- 背景の専用ワーカースレッドが1秒ごとに安価な判定 (atomic のみ) を行う。T1のプールは
  シャード型専用のため共用しない。reorg worker とも分離し、長時間の移設が checkpoint
  駆動を遅らせないようにする。
- 全走査サイクルは、強制呼び出し、overhead がしきい値を初めて上回ったとき、前回から
  5ポイント以上悪化したとき、checkpoint が frozen 領域を増やしたとき(かつ5%以上)の
  いずれかでのみ走る。punch 待ちがある場合は走査なしの punch のみ行う。

## クラッシュ安全性

- copy-append と T1 張り替えの間に落ちても orphan copy が残るだけで、WAL replay は
  張り替え前の旧バイトに対して再構築するため正しい。進捗のみ失う。
- punch は WAL-durable (await 済み) かつ前サイクル排出分に限るため、replay は張り替え後
  の状態を必ず再現できる。punch 待ち行列自体は揮発で、再起動後は空セグメントとして
  再発見・再投入される (self-healing)。

## ファイルシステム

- hole-punch 非対応 (tmpfs など) では移設だけでは bytes_used が増える一方になるため、
  サイクル全体を実行しない。対応有無は初回に一度だけ判定する。

## パラメータ

| Parameter | Meaning | Default |
| --- | --- | --- |
| `T2DefragSpaceOverheadPercent` | 許容space overhead(%)。背景起動判定の唯一の基準 | 20 |

セグメントサイズ(8MB)、1サイクル移動上限(1GiB)、punch slop(1MiB)、quarantine(1サイクル)は
固定値とし、調整ノブに出さない。
