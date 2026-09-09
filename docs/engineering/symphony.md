# Symphony sandbox operations

Symphonyは`tokachi269/wire-symphony-test`のIssueだけを監視し、`D:/GitHub/wire`を変更しない。通常実装はLuna Max、Solは設計とレビューだけに限定する。

## Routing

| Label | Model | Purpose | Write access | Max turns |
|---|---|---|---|---:|
| `symphony-luna` | `gpt-5.6-luna` / `max` | scoped implementation and verification | isolated workspace only | 10 |
| `symphony-sol-review` | `gpt-5.6-sol` / `high` | pre-implementation design or post-implementation review | read-only | 2 |

1つのIssueへ両方のラベルを付けない。通常はLunaだけを使う。Solを使うのは、実装前にownerや契約境界の判断が必要な場合、または実装commitの高リスク部分をレビューする場合だけとする。

## Before starting

1. `D:/GitHub/wire-symphony-test`の`WORKFLOW.md`と`WORKFLOW.sol-review.md`が意図したbranchにあることを確認する。
2. GitHubのfine-grained tokenをPowerShellの`GITHUB_TOKEN`へ設定する。対象repoは`wire-symphony-test`だけに絞り、Issuesをread/writeにする。tokenをrepoやworkflowへ書かない。
3. `codex login status`が成功することを確認する。
4. GitHub Issueフォームから、実装またはレビューのどちらか1つを作る。

## Start

通常実装はPowerShellで次を実行する。

```powershell
Set-Location D:\GitHub\wire-symphony-test
.\tools\start_symphony.ps1 luna
```

Solレビューは別terminalで必要なときだけ起動する。

```powershell
Set-Location D:\GitHub\wire-symphony-test
.\tools\start_symphony.ps1 sol
```

dashboardはLunaが`http://localhost:4000/`、Solが`http://localhost:4001/`である。

## Issue lifecycle

1. Open Issueへ対応するラベルを1つ付けるとSymphonyが取得する。
2. LunaはIssueごとの隔離workspaceで変更・検証し、local commitを作る。pushと本体変更は行わない。
3. 完了またはblockedの要約がIssueへ記録され、処理済みラベルが外れる。Issueは自動closeしない。
4. 帰宅後にIssue記載のworkspaceとcommitを確認する。採用するcommitだけを`wire-symphony-test`へcherry-pickしてCIを通す。
5. 実験結果が良ければ、同じcommitを本体`D:/GitHub/wire`へcherry-pickする。

Solでpost-implementation reviewを行う場合は、対象commitを先に`wire-symphony-test`へcherry-pickし、そのhashをレビューIssueへ書いてからラベルを付ける。

## Failure handling

- Issueのラベルが残ったままなら、dashboardとlogsを確認する。原因を直す前に同じIssueを重複作成しない。
- Issue本文に未決定事項があり結果が変わる場合、Symphonyは実装せずblockedとして返す。
- workspace内のcommitを確認するまでworkspaceを削除しない。
- GitHub token、Codex認証、repo、label、workflow pathのいずれかが欠けると起動またはpollingに失敗する。
