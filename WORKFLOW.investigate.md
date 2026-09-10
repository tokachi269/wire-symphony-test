---
tracker:
  kind: github
  provider:
    repo: "tokachi269/wire-symphony-test"
    token: $GITHUB_TOKEN
  required_labels:
    - "agent:investigate"
  active_states:
    - open
  terminal_states:
    - closed
polling:
  interval_ms: 30000
workspace:
  root: "D:/GitHub/wire-symphony-investigation-workspaces"
hooks:
  after_create: |
    git clone --no-hardlinks --branch symphony/experiment --single-branch "D:/GitHub/wire-symphony-test" .
    git remote set-url --push origin "disabled://wire-symphony-test"
  timeout_ms: 120000
agent:
  max_concurrent_agents: 1
  max_turns: 4
  max_retry_backoff_ms: 300000
codex:
  command: '"${SYMPHONY_CODEX_PATH:-codex}" --model ${SYMPHONY_UPPER_MODEL:-gpt-5.6-sol} --config model_reasoning_effort=${SYMPHONY_UPPER_EFFORT:-medium} --config agents.max_concurrent_threads_per_session=2 --config agents.default_subagent_model=${SYMPHONY_UPPER_MODEL:-gpt-5.6-sol} --config agents.default_subagent_reasoning_effort=${SYMPHONY_UPPER_EFFORT:-medium} --config shell_environment_policy.inherit=all app-server'
  approval_policy: never
  thread_sandbox: read-only
  turn_sandbox_policy:
    type: readOnly
    networkAccess: false
  turn_timeout_ms: 3600000
  stall_timeout_ms: 300000
---

GitHub Issue `{{ issue.identifier }}` をread-onlyで調査し、確認できた事実から実装可能なIssueへ分解する。

Title: {{ issue.title }}

{% if issue.description %}
Description:
{{ issue.description }}
{% else %}
Descriptionは未記載。
{% endif %}

## 調査規約

1. 最初に`AGENTS.md`、`docs/architecture.md`、`docs/testing.md`、`docs/engineering/agent_harness.md`、該当domainのarchitectureとoperation semanticsを読む。
2. production file、test、文書、設定を変更しない。commit、push、deploy、release、本体`D:/GitHub/wire`への変更を行わない。
3. IssueのQuestion、Scope、Out of scopeを境界とし、事実、推論、未確認事項を分ける。
4. コード、履歴、既存Issue、test ownership、architecture evidenceを調査する。co-changeやhotspotなどのsensorだけから欠陥や必要実装を断定しない。
5. 強いモデルのsubagentは、複数domainのowner判定、矛盾するevidence、security境界の確認に必要な場合だけread-onlyで使う。常時並列起動しない。

## GitHub操作境界

`github_api`はこのrepoのIssue調査と、下記の操作だけに使う。

- open/closed Issueの検索と読取り。
- 現在の調査Issueへの完了コメント、`agent:investigate`の削除、必要時の`agent:blocked`追加。
- この調査から生成するIssueの作成と、そのIssueへの`agent:proposed`追加。

ほかのrepo、Issue、pull request、Actions、contents、branches、releases、repository settingsには書き込まない。生成Issueをclose、delete、編集しない。`PATCH`、`PUT`、`DELETE`は現在の調査Issueのラベル操作以外に使わない。

## Issue分解

Issue本文の`Generated issue handling`に従う。

- `Report only; do not create issues`: 調査結果だけを現在のIssueへ記録し、新Issueを作らない。
- `Create proposed issues without running them`: 作成Issueへ`agent:proposed`だけを付ける。
- `Create and queue independent issues automatically`: 未決定事項や別の生成Issueへの依存がなく、現在のbaseから独立して実装・検証できるIssueだけ、`[Agent implement]`タイトルで作成する。routing workflowが`agent:run`と`agent:implement`を付ける。依存または未決定事項があるIssueは`agent:proposed`に留める。

新Issueを作る前に、open/closed両方の既存Issueを検索し、重複を作らない。各Issueはファイル単位ではなく、同じdecision owner、受け入れ条件、検証方法で完了する変更単位とする。

各Issue本文に次を含める。

- Origin investigation: 現在のIssue URL
- Goal
- Confirmed evidence
- Acceptance criteria
- Scope and out of scope
- Canonical references
- Dependencies or `None`

1調査で作成できるIssueは最大20件とする。20件を超える候補がある場合、雑に切り分けずfamilyまたはphaseへ分類し、作成済み件数、未作成分類、次に必要な調査を報告する。生成Issueから孫Issueを作るよう指示しない。

## 完了報告

現在のIssueへ、確認した範囲、確認済み事実、未確認事項、作成または既存流用したIssue一覧、自動queueに入れたIssue、proposalに留めたIssueを1回だけ記録する。

GitHub操作が利用できる場合、再実行を止めるため`agent:investigate`ラベルを外す。調査不能または安全に分解できない場合は`agent:blocked`を付ける。現在の調査Issueと、この調査から作成したIssue以外には書き込まない。
