---
tracker:
  kind: github
  provider:
    repo: "tokachi269/wire"
    token: $GITHUB_TOKEN
  required_labels:
    - symphony-test
  active_states:
    - open
  terminal_states:
    - closed
polling:
  interval_ms: 30000
workspace:
  root: "D:/GitHub/wire-symphony-workspaces"
hooks:
  after_create: |
    git clone --no-hardlinks --branch symphony/experiment --single-branch "D:/GitHub/wire-symphony-test" .
    git remote set-url --push origin "disabled://wire-symphony-test"
  timeout_ms: 120000
agent:
  max_concurrent_agents: 1
  max_turns: 10
  max_retry_backoff_ms: 300000
codex:
  command: "codex --config shell_environment_policy.inherit=all app-server"
  approval_policy:
    reject:
      sandbox_approval: true
      rules: true
      mcp_elicitations: true
  thread_sandbox: workspace-write
  turn_sandbox_policy:
    type: workspaceWrite
    networkAccess: true
  turn_timeout_ms: 3600000
  stall_timeout_ms: 300000
---

GitHub Issue `{{ issue.identifier }}` を、現在の隔離workspace内だけで処理する。

Title: {{ issue.title }}

{% if issue.description %}
Description:
{{ issue.description }}
{% else %}
Descriptionは未記載。
{% endif %}

{% if attempt %}
これは継続または再試行 #{{ attempt }}。現在のworkspace状態を確認し、完了済み作業を最初からやり直さない。
{% endif %}

## 作業規約

1. 最初に `AGENTS.md` を読み、そこから指定される正本文書と手順に従う。
2. production変更前に `docs/architecture.md`、`docs/testing.md`、該当domainのarchitectureとoperation semanticsを読む。agent-development workflowでは `docs/engineering/agent_harness.md` も読む。
3. Issueの記載を要求範囲として扱う。不明点を想像で補わず、結果が変わる未決定事項があれば変更せずblockedとして報告する。
4. 変更前に現状、再現条件、既存の共有実装、作業ツリーを確認する。ユーザー所有の差分を上書きまたは削除しない。
5. 局所修正で足りる場合は局所で直す。同じ意味の再判定、互換分岐、不要な抽象化を増やさない。
6. 検証は `docs/testing.md` と `docs/command_cheatsheet.md` に従い、変更範囲に見合う最小十分なものを実行する。未実行、失敗、skipを成功として報告しない。
7. 検証済みの作業単位ごとにlocal commitを作る。push、deploy、release、本体 `D:/GitHub/wire` への変更は禁止する。
8. 現在のIssue以外を変更しない。範囲外の改善を実装しない。

## 完了報告

最終報告には、観測したこと、変更内容、検証結果、commit hash、残る不確実性またはblockerを簡潔に含める。

GitHub操作が利用できる場合、完了またはblockedの要約を現在のIssueへ1回だけ記録し、再実行を止めるため `symphony-test` ラベルを外す。Issueはcloseしない。ほかのIssue、PR、ラベルには触れない。
