# Symphony sandbox operations

Symphonyは`tokachi269/wire-symphony-test`のIssueだけを監視し、canonical Wire repositoryを変更しない。ラベルはモデル名ではなく仕事の種類を表す。具体的なモデル割当はworkflowだけで管理する。

## Routing

| Label | Model | Purpose | Write access | Max turns |
|---|---|---|---|---:|
| `agent:run` | none | dispatch gate paired with `agent:implement` | follows implementation workflow | none |
| `agent:implement` | routine model (`gpt-5.6-luna` / `max` initially) | scoped implementation, verification, and Draft PR creation | isolated workspace and experiment remote | 3 |
| `agent:investigate` | upper model (`gpt-5.6-sol` / `medium` initially) | read-only repository investigation and evidence-based Issue decomposition | repository read-only; scoped Issue writes | 4 |
| `agent:decision` | upper model (`gpt-5.6-sol` / `medium` initially) | settle owner, invariants, forbidden changes, and verification before implementation | read-only | 1 |
| `agent:planned` | none | upper-model decision is available as an Implementation brief | none | none |
| `agent:review-required` | none | residual risk requires scoped upper-model review after implementation | none | none |
| `agent:proposed` | none | generated Issue requiring a decision or dependency resolution | none | none |
| `agent:review` | upper model (`gpt-5.6-sol` / `medium` initially) | standalone design or review | read-only | 1 |
| `agent:working` | none | implementation currently active | none | none |
| `agent:ready` | none | independent review passed; human decision pending | none | none |
| `agent:blocked` | none | operator attention; never dispatches | none | none |

実装queueでは`agent:run`と`agent:implement`を組み合わせる。調査は`agent:investigate`だけでread-only実行する。実装Issueは原則として新Issueを作らず、調査workflowだけが根拠に基づくIssue分解を担当する。

モデルは工程ではなく判断コストで分ける。routine modelは探索、定型変更、閉じた仕様の実装、focused verificationを担当する。upper modelは曖昧な調査とIssue分解、owner・invariant・禁止事項の確定、残余riskがある変更のscoped reviewを担当する。通常実装の親agentを途中で上位モデルへ置換せず、実装前のtask境界でdecision queueへ渡す。

すべてのDraft PRをupper modelでreviewしない。通常経路は`Luna -> CI -> agent:ready`である。判断が必要な経路は`Sol decision -> Luna implementation -> CI`とし、decision後にもsecurity、persistence、外部入力、権限境界、authority、複数domain、重大な不確実性が残る場合だけ`-> Sol scoped review`を追加する。`Luna -> Sol full review -> Luna rework -> Sol full review`の反復を通常経路にしない。

既定値は`SYMPHONY_ROUTINE_MODEL=gpt-5.6-luna`（`max`）と`SYMPHONY_UPPER_MODEL=gpt-5.6-sol`（`medium`）である。launcherは親terminalに残った同名のmodel環境変数を引き継がず、この既定値または明示された起動引数を子queueへ渡す。新モデルへ変更するときはIssueラベルや4つのworkflowを編集せず、起動時の`-RoutineModel`または`-UpperModel`だけを変更する。

調査workflowのfilesystemはread-onlyだが、Symphonyの`github_api`はIssue作成・コメント・ラベル操作に使える。workflowは許可操作を現在の調査Issueとそこから生成するIssueに限定する。ただしこれはagentへの実行契約であり、Symphony本体のREST path allowlistではない。強制境界は、fine-grained tokenをこの実験repoだけに限定し、Contentsをread-only、Issuesをread/writeにすることで作る。

## Before starting

1. 実験repoの`WORKFLOW.md`、`WORKFLOW.investigate.md`、`WORKFLOW.review.md`が意図したbranchにあることを確認する。
2. GitHubのfine-grained tokenをPowerShellの`GITHUB_TOKEN`へ設定する。対象repoは`wire-symphony-test`だけに絞り、Issues、Contents、Pull requestsをread/writeにする。tokenをrepoやworkflowへ書かない。
3. `codex login status`が成功することを確認する。
4. GitHub Issueフォームから、調査、単独実装、standalone reviewのいずれか1つを作る。OWNER、MEMBER、COLLABORATORがフォームを送信するとrouting workflowが対応ラベルを自動付与する。外部ユーザーのIssueは自動実行しない。

## Start

通常運用はPowerShellで次を1回だけ実行する。実装、調査、decision、reviewは別Symphony processだが、このlauncherが同じterminalから4つを起動・監督する。

```powershell
Set-Location <wire-symphony-test checkout>
.\tools\start_symphony.ps1 all -AcknowledgePreviewRisk
```

`-AcknowledgePreviewRisk`は、Symphony engineering previewが通常のguardrailなしで動くことへの明示確認である。terminalを開いたままにし、停止時は`Ctrl+C`を押す。dashboardは実装queueが`http://localhost:4000/`、調査queueが`http://localhost:4001/`、review queueが`http://localhost:4002/`、decision queueが`http://localhost:4003/`である。

launcherは`codex.exe`をPATHから探し、見つからない場合はCodex appの`%LOCALAPPDATA%\OpenAI\Codex\bin`から最新版を解決して、hidden child processとSymphonyへ絶対パスで渡す。Symphony内部のshellにはWSLの`bash.exe`ではなくGit for Windowsの`bash.exe`を使う。

launcherの既定値は、実験repoの親ディレクトリにある`symphony\elixir`、`wire-symphony-workspaces`、`wire-symphony-logs`である。別の配置では`SYMPHONY_ROOT`、`SYMPHONY_WORKSPACE_ROOT`、`SYMPHONY_LOGS_ROOT`を起動前に設定する。これらはworkflowへ絶対パスを書き込む代わりの実行時設定である。

実装queueだけはCodexの`permissions.symphony-codex` profileを使う。このprofileはworkspace直下とその`.git`だけを書き込み可能にし、GitHub API、git push、必要なlocal bindのためnetworkを許可する。`danger-full-access`は使わない。調査とreview queueはread-only sandboxのままとする。

Serenaはユーザー環境へ`uv tool install -p 3.13 serena-agent`で導入し、`serena setup codex`で`--project-from-cwd`のMCPとして登録する。launcherは`%USERPROFILE%\.local\bin`をPATHへ追加し、Serenaが見つからない場合は起動を止める。review queueだけはMCP引数を`--mode=planning`で上書きし、Serenaの編集toolを無効化する。Serenaがworkspaceに生成する`.serena/`はlocal index/cacheとしてignoreし、PRへ含めない。

モデルを更新するときは、起動時の`-UpperModel <model-id>`または`-RoutineModel <model-id>`だけを変える。

個別queueの診断時だけ`implement`、`investigate`、`decision`、`review`を直接指定する。実装後reviewは`agent:review-required`があるDraft PRだけを別sessionで行う。

```powershell
Set-Location <wire-symphony-test checkout>
.\tools\start_symphony.ps1 review -AcknowledgePreviewRisk
```

standalone review queueのdashboardは`http://localhost:4002/`である。

## Issue lifecycle

1. 信頼済みユーザーが調査フォームを送信すると、GitHub Actionsが`agent:investigate`を付け、read-only調査が始まる。
2. 調査agentは確認済みevidenceからowner、invariant、禁止事項、検証まで閉じた`[Agent planned]` Issueへ分解し、自動queueへ入れる。依存または未決定事項があるものは`agent:proposed`に留める。
3. 信頼済みユーザーが単独実装フォームを送信した場合も、GitHub Actionsが`agent:run`と`agent:implement`を付ける。手動ラベル操作は不要である。
4. 直接登録した実装Issueに`agent:planned`がなく判断条件へ該当した場合、Lunaは変更前に`agent:decision`へ渡す。SolはImplementation briefをIssueへ記録し、`agent:planned`と`agent:run`を付けて戻す。
5. 実装agentはIssueごとの隔離workspaceで`agent/*` branchを作り、briefに限定して変更・検証・commitする。開始時に`agent:working`を付ける。
6. commit後、隔離repoへpushしてDraft PRを作る。`agent:review-required`がなければ`agent:ready`、あれば`agent:review`へ進む。
7. review queueはbriefとdiffの不一致を中心に確認する。findingがあれば`agent:run`へ戻し、なければ`agent:ready`となる。
8. Issue、Draft PR、CIで結果を確認し、採用するcommitだけをcanonical Wire repositoryへcherry-pickする。自動mergeは行わない。

## Trial metrics

最初の20 Issueでは、Issueごとにroutine/upper model token、decisionの有無、reviewの有無、review findingの有無、修正round数、CI結果、最終採否を記録する。20件は成功目標ではなくrouting境界を調整する標本である。特に`review findingなし`が続くIssue familyは次回からreview対象外候補とし、Luna失敗後にSolへ渡る回数が多いfamilyは事前decision対象へ寄せる。

各queueはsession終了時にIssue単位のtoken、実行時間、turn数を`Trial usage`としてlogへ残す。集計はrepository相対の既定log rootを使って次を実行する。CSVを保存する場合だけ`-OutputPath`を指定する。

```powershell
.\tools\export_symphony_trial_metrics.ps1
.\tools\export_symphony_trial_metrics.ps1 -OutputPath .\symphony-trial.csv
```

このlogで分かるのはqueue sessionごとの消費量である。decision有無、review finding、CI結果、最終採否はIssueのlabel、コメント、PRから同じIdentifierへ結合する。token量だけで品質を判定しない。

通常のpost-implementation reviewに別Issueは作らない。既存commitの単独auditや実装前設計だけをstandalone review Issueとして登録する。

## Failure handling

- Issueのラベルが残ったままなら、dashboardとlogsを確認する。原因を直す前に同じIssueを重複作成しない。
- Issue本文に未決定事項があり結果が変わる場合、Symphonyは実装せずblockedとして返す。
- workspace内のcommitを確認するまでworkspaceを削除しない。
- GitHub token、Codex認証、repo、label、workflow pathのいずれかが欠けると起動またはpollingに失敗する。
