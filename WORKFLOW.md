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
  command: '"${SYMPHONY_CODEX_PATH:-codex}" --model ${SYMPHONY_ROUTINE_MODEL:-gpt-5.6-luna} --config model_reasoning_effort=${SYMPHONY_ROUTINE_EFFORT:-max} --config agents.max_concurrent_threads_per_session=2 --config agents.default_subagent_model=${SYMPHONY_UPPER_MODEL:-gpt-5.6-sol} --config agents.default_subagent_reasoning_effort=${SYMPHONY_UPPER_EFFORT:-medium} --config shell_environment_policy.inherit=all app-server'
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
7. 最初の変更前に `git rev-parse HEAD` を記録する。検証済みの作業単位ごとにlocal commitを作る。push、deploy、release、本体 `D:/GitHub/wire` への変更は禁止する。
8. 現在のIssue以外を作成、変更、closeしない。調査結果のIssue分解は`agent:investigate` workflowの責務とする。範囲外の改善は最終報告へproposalとして残す。

## 強いモデルを使う条件

通常の調査と実装は親agent自身で行う。subagentを常時並列起動しない。

次のいずれかを観測した場合だけ、実装を一旦止め、別セッションのsubagentを1つ起動して診断を依頼してよい。subagentはread-onlyで調査し、変更してはならない。

- 再現済みの同じ失敗に対する修正案が一度失敗した。
- contractまたはdecision ownerを正本文書から一意に決められない。
- testを弱める、別のauthorityを増やす、または範囲外変更を行わないと進めない案しか残っていない。

subagentの結果は提案として扱い、親agentが根拠を確認してから実装する。診断後も安全に進められない場合は変更を増やさずblockedとする。

## 必須の独立レビュー

実装とfocused verificationを完了してlocal commitを作った後、`docs/engineering/review_policy.md`を入力契約として、別セッションのreviewer subagentを必ず1つ起動する。

- reviewerはread-onlyとし、ファイル変更、commit、Issue操作を禁止する。
- 変更前に記録したbase commitから現在のHEADまでの完全なdiff、Issueのacceptance criteria、実行済み検証を渡す。
- reviewerには重大度順のfindingsを要求する。各findingは該当箇所、破綻する条件、根拠、必要な修正または検証を含める。
- reviewerがfindingを返した場合、親agentが修正・検証・commitし、同じreviewerへ完全な更新diffの再レビューを依頼する。
- actionable findingが残っている間は完了扱いにしない。reviewerを起動できない、結果を取得できない、またはreview対象を特定できない場合はblockedとする。
- reviewが通る前に実行ラベルを外さない。

## 完了報告

最終報告には、観測したこと、変更内容、検証結果、commit hash、残る不確実性またはblockerを簡潔に含める。

GitHub操作が利用できる場合、完了またはblockedの要約を現在のIssueへ1回だけ記録し、再実行を止めるため `agent:run` ラベルを外す。blockedの場合はさらに `agent:blocked` を付ける。Issueはcloseしない。ほかのIssue、PR、ラベルには触れない。
