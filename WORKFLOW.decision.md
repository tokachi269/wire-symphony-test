---
tracker:
  kind: github
  provider:
    repo: "tokachi269/wire-symphony-test"
    token: $GITHUB_TOKEN
  required_labels:
    - "agent:decision"
    - "agent:implement"
  active_states: [open]
  terminal_states: [closed]
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

GitHub Issue `{{ issue.identifier }}` の実装前decisionだけを行う。

Title: {{ issue.title }}

{% if issue.description %}
Description:
{{ issue.description }}
{% else %}
Descriptionは未記載。
{% endif %}

## 制約

1. `AGENTS.md`、`docs/testing.md`、該当domainのarchitectureとoperation semantics、Issueの全コメントを読む。
2. tracked file、Issue本文、PR、commit、branchを変更しない。実装、push、deploy、releaseを行わない。
3. 実装案を広げず、今回変更してよいownerと境界、守るinvariant、禁止事項、focused verificationを確定する。
4. 既存decisionコメントがある場合はゼロから再調査せず、今回追加された未決定事項だけを扱う。
5. 実装後にも独立した上位reviewが必要なのは、security、persistence、外部入力、権限境界、authority変更、複数domainにまたがる変更、またはdecision後にも重大な不確実性が残る場合だけとする。

## 完了報告

現在のIssueへ、Decision owner、Allowed change、Required components、Invariants、Forbidden changes、Acceptance criteria、Focused verification、Remaining uncertainty、Post-implementation upper reviewの要否と理由を`Implementation brief`として1回だけコメントする。trial用にupper model roleとdecision roundも記録する。

安全に閉じたbriefを作れた場合、`agent:decision`と`agent:working`を外し、`agent:planned`と`agent:run`を付けてLuna実装queueへ戻す。上位reviewが必要な場合だけ`agent:review-required`も付ける。判断不能なら`agent:decision`と`agent:working`を外し、`agent:blocked`を付ける。他のIssueを変更しない。
