# Symphony sandbox operations

Symphonyは`tokachi269/wire-symphony-test`のIssueだけを監視し、`D:/GitHub/wire`を変更しない。ラベルはモデル名ではなく仕事の種類を表す。具体的なモデル割当はworkflowだけで管理する。

## Routing

| Label | Model | Purpose | Write access | Max turns |
|---|---|---|---|---:|
| `agent:run` | none | dispatch gate paired with `agent:implement` | follows implementation workflow | none |
| `agent:implement` | routine model (`gpt-5.6-luna` / `max` initially) | scoped implementation, verification, and Draft PR creation | isolated workspace and experiment remote | 3 |
| `agent:investigate` | upper model (`gpt-5.6-sol` / `medium` initially) | read-only repository investigation and evidence-based Issue decomposition | repository read-only; scoped Issue writes | 4 |
| `agent:proposed` | none | generated Issue requiring a decision or dependency resolution | none | none |
| `agent:review` | upper model (`gpt-5.6-sol` / `medium` initially) | standalone design or review | read-only | 1 |
| `agent:working` | none | implementation currently active | none | none |
| `agent:ready` | none | independent review passed; human decision pending | none | none |
| `agent:blocked` | none | operator attention; never dispatches | none | none |

実装queueでは`agent:run`と`agent:implement`を組み合わせる。調査は`agent:investigate`だけでread-only実行する。実装Issueは原則として新Issueを作らず、調査workflowだけが根拠に基づくIssue分解を担当する。

モデルはラベルではなく役割で分ける。routine modelは明確なIssueの実装とfocused verificationを担当する。upper modelは曖昧な調査とIssue分解、実装agentが停止条件に当たった診断、commit後の独立review、standalone設計・reviewだけを担当する。通常実装の親agentを途中で上位モデルへ置換せず、必要な局面だけ別セッションのupper-model subagentへ渡す。

既定値は`SYMPHONY_ROUTINE_MODEL=gpt-5.6-luna`（`max`）と`SYMPHONY_UPPER_MODEL=gpt-5.6-sol`（`medium`）である。新モデルへ変更するときはIssueラベルや3つのworkflowを編集せず、起動時の`-RoutineModel`または`-UpperModel`だけを変更する。

調査workflowのfilesystemはread-onlyだが、Symphonyの`github_api`はIssue作成・コメント・ラベル操作に使える。workflowは許可操作を現在の調査Issueとそこから生成するIssueに限定する。ただしこれはagentへの実行契約であり、Symphony本体のREST path allowlistではない。強制境界は、fine-grained tokenをこの実験repoだけに限定し、Contentsをread-only、Issuesをread/writeにすることで作る。

## Before starting

1. `D:/GitHub/wire-symphony-test`の`WORKFLOW.md`、`WORKFLOW.investigate.md`、`WORKFLOW.review.md`が意図したbranchにあることを確認する。
2. GitHubのfine-grained tokenをPowerShellの`GITHUB_TOKEN`へ設定する。対象repoは`wire-symphony-test`だけに絞り、Issues、Contents、Pull requestsをread/writeにする。tokenをrepoやworkflowへ書かない。
3. `codex login status`が成功することを確認する。
4. GitHub Issueフォームから、調査、単独実装、standalone reviewのいずれか1つを作る。OWNER、MEMBER、COLLABORATORがフォームを送信するとrouting workflowが対応ラベルを自動付与する。外部ユーザーのIssueは自動実行しない。

## Start

通常運用はPowerShellで次を1回だけ実行する。実装、調査、reviewは別Symphony processだが、このlauncherが同じterminalから3つを起動・監督する。

```powershell
Set-Location D:\GitHub\wire-symphony-test
.\tools\start_symphony.ps1 all -AcknowledgePreviewRisk
```

`-AcknowledgePreviewRisk`は、Symphony engineering previewが通常のguardrailなしで動くことへの明示確認である。terminalを開いたままにし、停止時は`Ctrl+C`を押す。dashboardは実装queueが`http://localhost:4000/`、調査queueが`http://localhost:4001/`、review queueが`http://localhost:4002/`である。

launcherは`codex.exe`をPATHから探し、見つからない場合はCodex appの`%LOCALAPPDATA%\OpenAI\Codex\bin`から最新版を解決して、hidden child processとSymphonyへ絶対パスで渡す。Symphony内部のshellにはWSLの`bash.exe`ではなくGit for Windowsの`bash.exe`を使う。

実装queueだけはCodexの`permissions.symphony-codex` profileを使う。このprofileはworkspace直下とその`.git`だけを書き込み可能にし、GitHub API、git push、必要なlocal bindのためnetworkを許可する。`danger-full-access`は使わない。調査とreview queueはread-only sandboxのままとする。

Serenaはユーザー環境へ`uv tool install -p 3.13 serena-agent`で導入し、`serena setup codex`で`--project-from-cwd`のMCPとして登録する。launcherは`%USERPROFILE%\.local\bin`をPATHへ追加し、Serenaが見つからない場合は起動を止める。review queueだけはMCP引数を`--mode=planning`で上書きし、Serenaの編集toolを無効化する。Serenaがworkspaceに生成する`.serena/`はlocal index/cacheとしてignoreし、PRへ含めない。

モデルを更新するときは、起動時の`-UpperModel <model-id>`または`-RoutineModel <model-id>`だけを変える。

個別queueの診断時だけ`implement`、`investigate`、`review`を直接指定する。通常の実装後reviewもDraft PR作成後にreview queueが別sessionで行う。

```powershell
Set-Location D:\GitHub\wire-symphony-test
.\tools\start_symphony.ps1 review -AcknowledgePreviewRisk
```

standalone review queueのdashboardは`http://localhost:4002/`である。

## Issue lifecycle

1. 信頼済みユーザーが調査フォームを送信すると、GitHub Actionsが`agent:investigate`を付け、read-only調査が始まる。
2. 調査agentは確認済みevidenceからIssueを分解する。フォームで自動実行を許可した場合、独立して安全に実装できるIssueだけ`[Agent implement]`で作成し、自動queueへ入れる。依存または未決定事項があるものは`agent:proposed`に留める。
3. 信頼済みユーザーが単独実装フォームを送信した場合も、GitHub Actionsが`agent:run`と`agent:implement`を付ける。手動ラベル操作は不要である。
4. 実装agentはIssueごとの隔離workspaceで`agent/*` branchを作り、変更・検証・commitする。開始時に`agent:working`を付ける。
5. commit後、隔離repoへpushしてDraft PRを作り、`agent:run`と`agent:working`を外して`agent:review`を付ける。PR作成によりCI結果と差分がGitHubから見える。
6. review queueの別sessionが`docs/engineering/review_policy.md`に従いPR全体をread-onlyでreviewする。
7. findingがあれば`agent:review`を外して`agent:run`へ戻し、同じbranchとPRを更新する。findingがなければ`agent:ready`となる。
8. IssueとDraft PRで結果を確認し、採用するcommitだけを本体`D:/GitHub/wire`へcherry-pickする。自動mergeは行わない。

通常のpost-implementation reviewに別Issueは作らない。既存commitの単独auditや実装前設計だけをstandalone review Issueとして登録する。

## Failure handling

- Issueのラベルが残ったままなら、dashboardとlogsを確認する。原因を直す前に同じIssueを重複作成しない。
- Issue本文に未決定事項があり結果が変わる場合、Symphonyは実装せずblockedとして返す。
- workspace内のcommitを確認するまでworkspaceを削除しない。
- GitHub token、Codex認証、repo、label、workflow pathのいずれかが欠けると起動またはpollingに失敗する。
