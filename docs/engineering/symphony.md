# Symphony sandbox operations

Symphonyは`tokachi269/wire-symphony-test`のIssueだけを監視し、`D:/GitHub/wire`を変更しない。ラベルはモデル名ではなく仕事の種類を表す。具体的なモデル割当はworkflowだけで管理する。

## Routing

| Label | Model | Purpose | Write access | Max turns |
|---|---|---|---|---:|
| `agent:implement` | `gpt-5.6-luna` / `max` | scoped implementation, verification, independent subagent review | isolated workspace only | 10 |
| `agent:review` | configured review model (`gpt-5.6-sol` / `high` initially) | standalone design or review | read-only | 2 |
| `agent:blocked` | none | operator attention; never dispatches | none | none |

1つのIssueへ実行ラベルを複数付けない。通常実装では親agentだけが変更し、強いモデルのsubagentは定義済みの停止条件に当たった診断と、commit後の独立reviewに限定する。review modelを変更するときはIssueラベルではなくworkflowの1か所だけを更新する。

## Before starting

1. `D:/GitHub/wire-symphony-test`の`WORKFLOW.md`と`WORKFLOW.review.md`が意図したbranchにあることを確認する。
2. GitHubのfine-grained tokenをPowerShellの`GITHUB_TOKEN`へ設定する。対象repoは`wire-symphony-test`だけに絞り、Issuesをread/writeにする。tokenをrepoやworkflowへ書かない。
3. `codex login status`が成功することを確認する。
4. GitHub Issueフォームから、実装またはレビューのどちらか1つを作る。OWNER、MEMBER、COLLABORATORがフォームを送信するとrouting workflowが対応ラベルを自動付与する。外部ユーザーのIssueは自動実行しない。

## Start

通常実装はPowerShellで次を実行する。

```powershell
Set-Location D:\GitHub\wire-symphony-test
.\tools\start_symphony.ps1 implement
```

standalone reviewは別terminalで必要なときだけ起動する。通常の実装後reviewは実装agentが別セッションのsubagentを起動するため、こちらを起動する必要はない。

```powershell
Set-Location D:\GitHub\wire-symphony-test
.\tools\start_symphony.ps1 review
```

dashboardは実装queueが`http://localhost:4000/`、standalone review queueが`http://localhost:4001/`である。

## Issue lifecycle

1. 信頼済みユーザーがIssueフォームを送信すると、GitHub Actionsが対応する実行ラベルを付け、Symphonyが取得する。手動でラベルを付ける必要はない。
2. 実装agentはIssueごとの隔離workspaceで変更・検証し、local commitを作る。pushと本体変更は行わない。
3. commit後、別セッションのreviewer subagentが`docs/engineering/review_policy.md`に従い完全なdiffをread-onlyでreviewする。
4. findingがあれば実装agentが修正・再検証・commitし、reviewerが再確認する。
5. review完了またはblockedの要約がIssueへ記録され、実行ラベルが外れる。blockedなら`agent:blocked`が付く。Issueは自動closeしない。
6. 帰宅後にIssue記載のworkspaceとcommitを確認する。採用するcommitだけを`wire-symphony-test`へcherry-pickしてCIを通す。
7. 実験結果が良ければ、同じcommitを本体`D:/GitHub/wire`へcherry-pickする。

通常のpost-implementation reviewに別Issueは作らない。既存commitの単独auditや実装前設計だけをstandalone review Issueとして登録する。

## Failure handling

- Issueのラベルが残ったままなら、dashboardとlogsを確認する。原因を直す前に同じIssueを重複作成しない。
- Issue本文に未決定事項があり結果が変わる場合、Symphonyは実装せずblockedとして返す。
- workspace内のcommitを確認するまでworkspaceを削除しない。
- GitHub token、Codex認証、repo、label、workflow pathのいずれかが欠けると起動またはpollingに失敗する。
