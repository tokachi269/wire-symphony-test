---
tracker:
  kind: github
  provider:
    repo: "tokachi269/wire-symphony-test"
    token: $GITHUB_TOKEN
  required_labels:
    - "agent:review"
  active_states:
    - open
  terminal_states:
    - closed
polling:
  interval_ms: 30000
workspace:
  root: "$SYMPHONY_WORKSPACE_ROOT"
hooks:
  after_create: |
    git clone --no-hardlinks --branch symphony/experiment --single-branch "https://github.com/tokachi269/wire-symphony-test.git" .
    git remote set-url --push origin "disabled://wire-symphony-test"
  timeout_ms: 120000
agent:
  max_concurrent_agents: 1
  max_turns: 1
  max_retry_backoff_ms: 300000
codex:
  command: '"${SYMPHONY_CODEX_PATH:-codex}" --model ${SYMPHONY_UPPER_MODEL:-gpt-5.6-sol} --config model_reasoning_effort=${SYMPHONY_UPPER_EFFORT:-medium} --config "mcp_servers.serena.args=[''start-mcp-server'',''--context=codex'',''--mode=planning'',''--project-from-cwd'']" --config shell_environment_policy.inherit=all app-server'
  approval_policy: never
  thread_sandbox: read-only
  turn_sandbox_policy:
    type: readOnly
    networkAccess: false
  turn_timeout_ms: 1800000
  stall_timeout_ms: 300000
---

GitHub Issue `{{ issue.identifier }}` の設計検討またはDraft PR独立レビューだけを行う。

Title: {{ issue.title }}

{% if issue.description %}
Description:
{{ issue.description }}
{% else %}
Descriptionは未記載。
{% endif %}

## 制約

1. 最初に `AGENTS.md`、`docs/engineering/review_policy.md`、Issueと紐付くDraft PR、指定された正本文書を読む。実装workspaceが存在する場合はそのbaseからHEADまでの完全なdiffを直接確認する。
2. tracked fileの変更、commit、push、deploy、releaseは禁止する。Serenaが生成するignored local metadata以外は作成せず、調査と評価だけを行う。
3. Issueの問いに直接答え、事実、推論、未確認事項を分ける。範囲外の再設計を提案しない。
4. 設計依頼では、決定事項、未決定事項、owner、影響範囲、受け入れ条件、実装Issueへ渡す具体的な指示をまとめる。
5. レビュー依頼では `docs/engineering/review_policy.md` に従い、PR全体を重大度順のfindingsとして示す。問題がなければ、確認したdiff、検証、残るriskを明記する。

## 完了報告

結果をDraft PRと現在のIssueへ1回ずつ記録する。actionable findingがある場合は`agent:review`を外してから`agent:run`を付け、同じworkspaceを実装queueへ戻す。findingがない場合は`agent:review`を外して`agent:ready`を付ける。判断不能の場合は`agent:review`を外して`agent:blocked`を付ける。Issueはcloseせず、PRをready化またはmergeしない。ほかのIssue、PR、ラベルには触れない。
