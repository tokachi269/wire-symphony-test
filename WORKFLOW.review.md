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
  root: "D:/GitHub/wire-symphony-review-workspaces"
hooks:
  after_create: |
    git clone --no-hardlinks --branch symphony/experiment --single-branch "D:/GitHub/wire-symphony-test" .
    git remote set-url --push origin "disabled://wire-symphony-test"
  timeout_ms: 120000
agent:
  max_concurrent_agents: 1
  max_turns: 2
  max_retry_backoff_ms: 300000
codex:
  command: '"${SYMPHONY_CODEX_PATH:-codex}" --model ${SYMPHONY_UPPER_MODEL:-gpt-5.6-sol} --config model_reasoning_effort=${SYMPHONY_UPPER_EFFORT:-medium} --config shell_environment_policy.inherit=all app-server'
  approval_policy:
    reject:
      sandbox_approval: true
      rules: true
      mcp_elicitations: true
  thread_sandbox: read-only
  turn_sandbox_policy:
    type: readOnly
    networkAccess: false
  turn_timeout_ms: 1800000
  stall_timeout_ms: 300000
---

GitHub Issue `{{ issue.identifier }}` の設計検討または独立レビューだけを行う。

Title: {{ issue.title }}

{% if issue.description %}
Description:
{{ issue.description }}
{% else %}
Descriptionは未記載。
{% endif %}

## 制約

1. 最初に `AGENTS.md`、`docs/engineering/review_policy.md`、Issueが指定する正本文書・差分・commitを読む。
2. ファイル変更、commit、push、deploy、releaseは禁止する。調査と評価だけを行う。
3. Issueの問いに直接答え、事実、推論、未確認事項を分ける。範囲外の再設計を提案しない。
4. 設計依頼では、決定事項、未決定事項、owner、影響範囲、受け入れ条件、実装Issueへ渡す具体的な指示をまとめる。
5. レビュー依頼では `docs/engineering/review_policy.md` に従い、重大度順のfindingsをファイルと根拠つきで示す。問題がなければ、確認した範囲と残るriskを明記する。

## 完了報告

GitHub操作が利用できる場合、結果を現在のIssueへ1回だけ記録し、再実行を止めるため `agent:review` ラベルを外す。判断不能の場合は `agent:blocked` を付ける。Issueはcloseしない。ほかのIssue、PR、ラベルには触れない。
