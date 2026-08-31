# MAP_SHARED vs MAP_PRIVATE — checkpoint() 再設計のための文献調査

VMemKV の T2 稼働中 mmap は現在 `MAP_PRIVATE`(`low_level_design.md` 5.1)。`checkpoint_internal()` は
tail 領域の生存 record を 1 件ずつ seqlock 越しに読んで `pwrite()` する設計で、AWS実測(i4i.8xlarge、
ltm/64KB、32並行書き込み)では 51% のスループット劣化かつ 60 秒以内に完走しない。この根本原因を
「MAP_SHARED + `msync()` による checkpoint 再設計」で解消できないか検討するにあたり、`andy_mmap.pdf`
(Pavlo et al., CIDR 2022)・`mdb-paper.pdf`(LMDB)・`vmcache.pdf`(LeanStore)の3本を読んだ。

低優先度で `faster.pdf`(Microsoft FASTER)も候補に挙げたが未読。

## 1. Pavlo論文は MAP_SHARED を条件付きでも許容しているか

許容していない。§6 の "when you should not use mmap" に、VMemKV に直接該当する2条件が明記されている:
「transactionally safe な更新が必要」「高速な永続ストレージデバイスで高スループットが要る」。

唯一の関連する言及は "failure-atomic msync"(Park et al., EuroSys'13)——複数ページに渡る msync を
原子的にし、副作用として対象ページの transparent eviction を無効化する、非標準のカーネルパッチ。
論文はこれを「存在する」と紹介するのみで、十分な解決策としては推奨していない。Pavlo 自身が推す代替は
mmap ではなく pointer swizzling(明示的なバッファプール)。

## 2. LMDB は torn-write 問題をどう回避しているか

**前提の訂正が必要**: LMDB は実は MAP_SHARED な書き込み可能 mmap に対して `msync()` する設計では
**ない**。mmap は**読み取り専用**に保たれる(§3.1: "keeping the mapping read-only prevents stray
writes from buggy application code from corrupting the database")。書き込みは通常の `write()`
システムコールで行い、OS の統一ページキャッシュによって read-only mmap との一貫性が保たれる。

永続性・原子性の実現機構は msync ではなく **COW B+tree + 2つの meta-page の交互更新**:
更新されたページは in-place で上書きされず、新しいページを別 offset に確保する。コミットは
「新規/dirty ページ群を書いた後、ファイル offset 0/1 にある2つの meta-page のうち**古い方だけ**を
新しい root pointer で上書きする」の一手。"No locks are needed to protect readers from writers;
readers are guaranteed to always see a valid root node."(§4.2)。WAL は存在せず、この機構自体が
WAL の代替になっている。

**スコープの教訓**(論文に明記): LMDB は**部分的に書きかけの範囲を絶対に永続化しない**。ページは
常に「完全に旧」か「完全に新」のいずれかであり、meta-page のポインタ切り替えという**単一の固定サイズ・
常に有効な**更新だけがコミット地点になる。これは VMemKV が独自に到達していた「予約済み(reserved)と
書き込み完了済み(fully-written)を区別する」問題意識と同じ結論であり、裏付けと言える。

## 3. 「base region は一度チェックポイントされたら不変、更新は tail へリダイレクト」は文献的に妥当か

妥当。LMDB の COW B+tree はこの不変条件のより一般化された形そのものである: 一度コミット済みの
root から参照されるページは二度と in-place で変更されず、更新は必ず新しいページを生む(旧ページは、
それを参照する reader トランザクションが存在しなくなるまで free-list B+tree で保持される、§4.2)。
VMemKV の `base_boundary` 不変・tail redirect 設計は、この原則をバイト範囲粒度に粗くした版に相当する。
文献はこの不変条件を複雑化させるどころか、mmap ベースストアが torn-write を避ける標準的手法として
補強している。

## 4. MAP_SHARED 以外の第三の選択肢はあるか

ある。**vmcache(LeanStore)がまさにこれ**。ファイルに紐付かない `mmap(MAP_ANONYMOUS|MAP_PRIVATE|
MAP_NORESERVE)` を、キャッシュ済みページへの高速なポインタアクセスのためだけに使い、実際のI/Oは
すべて明示的な `pread`/`pwrite`(または io_uring)で行う。エビクションも `MADV_DONTNEED` で
アプリ側が制御する。重要なのは、書き戻しを **64ページまとめてバッチ化し libaio でベクタ書き込み**
する設計が明示的な工夫点として述べられている点(§3.4)。

これが示唆するのは、VMemKV の checkpoint が遅い根本原因は「MAP_PRIVATE であること」自体ではなく、
「1レコードずつ `pwrite()` している現行実装のやり方」にある可能性である。レコード単位の pwrite を
`writev()`/io_uring でバッチ化するだけで同程度の高速化が得られるなら、MAP_SHARED が実メモリ圧迫下で
背負う恒常タックス(このセッションでの実測: -12%〜-19%、書き込みスレッド数に応じて悪化)を一切
払わずに済む。

## 5. 並行書き込みに関する注意点(単一ライタ前提の技法は直接転用できない)

- **LMDB は全体が単一ライタ前提**(§4.2、Pavlo 論文でも "LMDB solves this problem by allowing
  only a single writer" と明記)。lock-free な reader も meta-page swap による単純なコミットも、
  同時に committer が1人しかいないことに依存している。VMemKV の並行書き込み設計へそのまま転用は
  できない(LMDB 自身も「複数ライタへの汎化」は行っていない)。
- **vmcache は並行ライタ前提で設計されている**(ページ状態機械: Evicted/Locked/Marked/Unlocked、
  exclusive/shared/optimistic ロック、§3.2–3.3)。VMemKV の並行性モデルへ転用しやすい先例。
- vmcache のバッチエビクション書き込みパス(§3.4: 候補選定 → dirty ページを shared ロック →
  libaio 書き込み → exclusive へ昇格試行 → madvise/除去)は、「全ライタを止めずに複数の dirty
  範囲を一括で永続化する」並行安全なパターンの具体例であり、今まで検討していた2択(MAP_PRIVATE+
  遅いループ vs MAP_SHARED+msync)より、再設計後の checkpoint の pre-stop パスに近い形かもしれない。

## VMemKV 固有の判断が必要な点

- **`.t2chk` 統合の可否**: 3論文いずれも「稼働中バッファ/mmap そのものが唯一の永続ファイルで、
  別のチェックポイント実体を持たない」設計ではない。LMDB は1ファイル+meta-page、vmcache はそもそも
  永続的な「稼働中 mmap」を持たない(anonymous)。`.t2chk` を live T2 ファイルへ統合する案は、
  これらの論文からの直接的な裏付けを得られない——VMemKV 自身のメリットに基づいて独自に判断する必要がある。

## まとめ: 検討すべき選択肢

1. **MAP_SHARED + msync()**(当初案): checkpoint 自体は実測で軽量化(72ms/13.7GB backlog、
   増分同期は数ms)。ただし実メモリ圧迫下で恒常的なスループットタックス(-12%〜-19%)を払う。
2. **MAP_PRIVATE のまま、pwrite をバッチ化/ベクタ化**(vmcache 由来の新候補): 恒常タックスなしに
   同等の高速化が得られる可能性がある。→ 追記: **実測の結果、否定された**(下記参照)。
3. **LMDB 型: mmap を read-only にし、書き込みは `write()` システムコール経由**: 単一ライタ前提の
   要素が強く、VMemKV の並行ライタ設計への転用には追加検討が要る。

## 追記: 選択肢2(バッチ化 pwrite)の実測結果

AWS i4i.8xlarge、実メモリ圧迫下(cgroup で `MemoryHigh=1GiB` に絞り、実際に swap/reclaim が
継続発生する条件)、ltm/64KB で検証。T2 未同期領域全体(初回は約7.3GB)を**単一の巨大な `pwrite()`
呼び出し**で書き出すプロトタイプ(`VMemKVImpl::batched_pwrite_prototype()`)を実装して計測した。

結果: **1スレッドで46.8%劣化、32スレッドで12.5%劣化、いずれも60秒でタイムアウトし完走せず**。
strace で確認したところ、約7.3GBのバックログに対する `pwrite64()` 呼び出しが**60秒経っても
1回も返ってこなかった**(同程度サイズに対する msync は72ms)。

**原因の推定**: `pwrite()` はソース側(呼び出し元プロセスのメモリ)がスワップアウトされていると、
システムコール内部でそのページ群を同期的にフォールトイン(スワップから読み戻し)してから
コピーする必要がある——これは1回の巨大な、割り込み不能に近いブロッキング操作になる。対して
`msync()` は MAP_SHARED マッピングの**既に常駐している** dirty ページに対する書き戻し要求で
あり、スワップからの読み戻しを一切必要としない、カーネルのページキャッシュ機構に乗った
軽量な操作である。この非対称性が、同じ「大きな範囲を一括で永続化する」という見た目上似た
操作の実測結果を大きく分けたと考えられる。

**留保**: このプロトタイプは vmcache 論文が実際に採用している手法(固定小サイズ・64ページ単位の
バッチを libaio で書く)ではなく、「未同期分すべてを1回で」という粗い実装だった。定常状態の
インクリメンタルな同期(初回のような巨大バックログではなく、直近の少量差分のみ)であれば
挙動が異なる可能性は残るが、msync() が初回・増分いずれも明確に優れている以上、この方向を
追加検証する優先度は低いと判断する。

**結論**: 選択肢2は棄却。**選択肢1(MAP_SHARED + msync())が現時点での最有力候補**として残る。
恒常タックス(-12%〜-19%)は、現行 checkpoint() の破局的コスト(51%劣化・非完走)と比較すれば
許容範囲というのがこのセッションでの判断(前掲の考察参照)。
