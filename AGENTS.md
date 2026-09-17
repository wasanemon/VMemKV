# 論文執筆の作業方針

- このプロジェクトでは、リポジトリの内容に基づいて論文を執筆する。
- まず日本語で執筆する。
- 論文原稿や関連ファイルの作成・編集は、原則としてリポジトリ直下の `/paper/` 以下で行う。
- `/paper/` 以外のファイルは原則として読み取り専用とし、ユーザーが明示的に指示した場合のみ作成・変更・削除する。
- Slack の情報源は [対象チャンネル](https://kawashima-ken.slack.com/archives/C08B41HFC85) とし、おおむね [このメッセージ](https://kawashima-ken.slack.com/archives/C08B41HFC85/p1773190772374969) 以降の、Yusuke Miyazaki と nikezono を中心とした議論を参照する。
- 同チャンネルで並行して議論されている Masahide Fukuyama の Helios の話題と、VMemKV の議論を混同しない。参照時は話題とスレッドの文脈を確認し、Helios の仕様・実験結果・主張を VMemKV のものとして扱わない。
- Slack は読み取り専用とし、投稿・編集・削除・リアクションなどの書き込み操作は行わない。
- 論文執筆用の GitHub リポジトリは [wasanemon/VMemKV](https://github.com/wasanemon/VMemKV) で、`origin` として登録されている。実装担当者のリポジトリとは分離されており、コミットや `origin` への push は原則として行ってよい。
- 作業のルールは `AGENTS.md`、現在の作業状態は `paper/PROGRESS.md` に記録する。
- `paper/PROGRESS.md` は冒頭に最終更新日を置き、「現在の段階」「完了したこと」「未解決の論点」「次に行うこと」の4項目で簡潔に記述する。成果物がある場合はリンクを添える。
- 作業の区切りで進捗を更新し、確認済みの内容と検討中の仮説を明確に区別する。
