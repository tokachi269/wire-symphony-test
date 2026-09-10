---
tracker:
  kind: github
  provider:
    repo: "tokachi269/wire-symphony-test"
    token: $GITHUB_TOKEN
  required_labels:
    - "agent:run"
    - "agent:implement"
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
    git remote set-url --push origin "https://github.com/tokachi269/wire-symphony-test.git"
  timeout_ms: 120000
agent:
  max_concurrent_agents: 1
  max_turns: 3
  max_retry_backoff_ms: 300000
codex:
  command: '"${SYMPHONY_CODEX_PATH:-codex}" --model ${SYMPHONY_ROUTINE_MODEL:-gpt-5.6-luna} --config model_reasoning_effort=${SYMPHONY_ROUTINE_EFFORT:-max} --config agents.max_concurrent_threads_per_session=2 --config agents.default_subagent_model=${SYMPHONY_UPPER_MODEL:-gpt-5.6-sol} --config agents.default_subagent_reasoning_effort=${SYMPHONY_UPPER_EFFORT:-medium} --config shell_environment_policy.inherit=all app-server'
  approval_policy: never
  permissions_profile: symphony-codex
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
7. 最初の変更前に `git rev-parse HEAD` を記録する。`symphony/experiment`から`agent/<小文字のIssue identifier>`（例: `agent/gh-1`）を作り、検証済みの作業単位ごとにcommitする。canonical Wire repositoryへの変更、deploy、releaseは禁止する。
8. 現在のIssue以外を作成、変更、closeしない。調査結果のIssue分解は`agent:investigate` workflowの責務とする。範囲外の改善は最終報告へproposalとして残す。

## 強いモデルを使う条件

通常の調査と実装は親agent自身で行う。subagentを常時並列起動しない。

次のいずれかを観測した場合だけ、実装を一旦止め、別セッションのsubagentを1つ起動して診断を依頼してよい。subagentはread-onlyで調査し、変更してはならない。

- 再現済みの同じ失敗に対する修正案が一度失敗した。
- contractまたはdecision ownerを正本文書から一意に決められない。
- testを弱める、別のauthorityを増やす、または範囲外変更を行わないと進めない案しか残っていない。

subagentの結果は提案として扱い、親agentが根拠を確認してから実装する。診断後も安全に進められない場合は変更を増やさずblockedとする。

## Draft PRと独立レビューへの引き渡し

実装とfocused verificationを完了してcommitを作った後、隔離repoへ現在の`agent/*` branchをpushする。続いて`github_api`でbaseを`symphony/experiment`、headを現在branchとするDraft PRを1件だけ作る。既存PRがある再試行では重複作成せず、そのPRを更新する。

- PR本文へIssue、base commit、head commit、実行した検証と結果、未確認riskを書く。
- Draft PR作成後、IssueへPR URLと検証要約をコメントする。
- `agent:run`と`agent:working`を外してから`agent:review`を付け、別のupper-model review queueへ引き渡す。
- pushまたはDraft PR作成に失敗した場合は`agent:blocked`を付け、失敗箇所をIssueへ記録する。review済みとして扱わない。

## 完了報告

最終報告には、観測したこと、変更内容、検証結果、commit hash、Draft PR URL、残る不確実性またはblockerを簡潔に含める。この実装sessionはDraft PRと`agent:review`への引き渡しで終了し、review完了とは主張しない。

作業開始時に`github_api`で現在のIssueへ`agent:working`を付ける。GitHub操作は現在のIssue、そのIssue用のbranchとDraft PRだけに限定する。Issueはcloseせず、PRはmergeしない。
