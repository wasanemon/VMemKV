# なぜ VMemKV は undo log を必要としないか

VMemKV の Tier 2 稼働中 mmap は `MAP_SHARED` である(low_level_design.md 5.1 節)。これは
古典的な buffer management の分類でいう **STEAL** を許容する設計である: OS のページキャッシュが
dirty page を、アプリケーションの意図と無関係な任意のタイミングでディスクへ書き戻し得る。
STEAL を許容する buffer management は一般に undo log を必要とする(下記参照)。本書は、
VMemKV が STEAL でありながら undo log を必要としない理由を示す。

## 1. STEAL/FORCE の定義と、古典的な STEAL→UNDO の要求

- **STEAL**: トランザクションがコミットする前に、その変更を含む dirty page がディスクへ
  書き戻され得る。
- **FORCE**: トランザクションのコミットは、そのトランザクションが変更した全ページが
  ディスクへ書き戻されるまで完了しない。

VMemKV は STEAL(上記)かつ **NO-FORCE** である: 個々の書き込み(insert/update/delete)は、
WAL レコードが `fsync` された時点でコミット完了とみなされ、呼び出し元へ成功を返す
(low_level_design.md 3.2 節)。対応する Tier 2 の物理ページがディスクへ反映されているかは
コミットの条件に含まれない。

古典的な ARIES 系の buffer management で STEAL が undo log を要求するのは、**物理ページ単位
の redo** を用いるためである: リカバリはページの LSN とログレコードの LSN を比較し、
「このページは既にこのログの内容を反映済みか」を判定して、必要ならページへ物理的な diff を
再適用する。この方式では、ディスク上のページの内容そのものがリカバリの入力になるため、
コミットされなかったトランザクションの変更が乗ったページが正式なページとして存在してしまうと、
それを取り消す(undo する)手段が別途必要になる。

## 2. VMemKV の redo は論理(operation)単位である

VMemKV のリカバリは、Tier 2 の生バイトを直接の入力としない。manifest が指す `t2_bytes_used`
未満(base 領域)のバイトのみを信頼し、それ以上の範囲(tail 領域)は物理的に何が書かれていようと
一切パースしない(low_level_design.md 5.3 節)。tail 領域の内容は、WAL の該当区間を
`write_entry_lockfree()` — 通常の書き込み経路そのもの — で再生することによって、
**新しい offset へ完全に新規のレコードとして再構築**される(4.4 節、5.2 節)。

つまり VMemKV の redo は「ページの物理バイトへ diff を当てる」のではなく、「操作(insert/update/
delete)そのものを再実行する」という論理 redo である。古い offset に何が残っていようと、
リプレイが再現する状態はそこを一切参照しない。この性質により、STEAL によってディスクへ
先行反映された tail 領域のバイトが「トランザクションの成否と無関係に居座る」という問題自体が
発生しない。

## 3. torn write の各パターンが無害である理由

**(a) tail 領域のレコードが torn な状態でディスクへ stolen された場合**
そのオフセットは manifest の境界未満ではないため、リカバリは一切参照しない。対応する WAL
レコードが fsync 済みであれば、そのレコードは通常の書き込み経路で再生され、新しい offset へ
正しく再構築される。WAL レコードが存在しない(fsync 前にクラッシュした)場合、その操作は
「起きなかったこと」として扱われる — これは呼び出し元へ成功を返していない操作の扱いと一致する。

**(b) WAL レコード自体が torn な場合**
WAL の replay はファイル末尾の不完全なレコードを検出し破棄する(checksum / 長さの検証による)。
torn な WAL レコードは「存在しないレコード」と等価に扱われ、(a) の「WAL レコードが存在しない」
場合に帰着する。

**(c) base 領域(manifest 境界未満)のレコードが torn である場合**
このケースは構造的に発生しない。境界は `checkpoint_internal()` が新規 append を短時間止め
(`stop_writers_and_wait()`)、その時点で完全に書き終わっている record のみを対象に `msync()`
が成功した後にしか前進しない(low_level_design.md 4.3 節)。したがって manifest が公表する
範囲には、書きかけの record が含まれる余地がない。

## 4. 結論

VMemKV は STEAL かつ NO-FORCE だが、undo log を必要としない。理由は、Tier 2 の tail 領域の
生バイトにいかなる権威も与えず、そこは常に WAL からの論理 redo(操作の再実行)によって
新しい offset へ再構築されるためである。base 領域は、公表前に完全性を保証してから境界を
前進させる設計により、そもそも torn な状態で公表されることがない。物理ページ単位の redo を
用いる古典的な設計とは異なり、STEAL と UNDO の結合が成立しない。
