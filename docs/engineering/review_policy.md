# Independent review policy

この文書はagent実装後の独立review手順を定める。product semanticsの正本ではない。意味論は`docs/architecture.md`、該当domain architecture、operation semantics、`docs/testing.md`を参照する。

## Independence

- 実装を行った親agentとは別セッションでreviewする。
- reviewerはread-onlyとし、ファイル変更、commit、Issue操作を行わない。
- 実装者の説明だけでなく、Issue、変更前base commit、完全なdiff、現在のHEAD、検証出力を直接確認する。
- 通常reviewは実装会話を参照してよい。architecture、security、persistence、外部入力、権限境界を変える場合は、可能なら実装理由を先に与えずdiffと正本文書から確認する。

## Review order

1. Acceptance: Issueの受け入れ条件を満たし、範囲外変更を含まないか。
2. Correctness: failure path、境界値、state transition、error propagationを含めてbehaviorが成立するか。
3. Architecture: decision owner、authority、dependency方向、operation semanticsを守り、第二の判定やspecial pathを増やしていないか。
4. Regression proof: `docs/testing.md`に沿うfocused proofがあり、testの削除・弱体化・skip・実装詳細への過結合がないか。
5. Security: Issue本文などの外部入力、secret、権限、network、shell command、pathのscopeが拡大していないか。
6. Performance and scale: hot path、計算量、allocation、I/O、buildまたはtest時間に説明不能な悪化がないか。
7. Maintainability: 既存共有実装を再利用し、互換分岐、不要なbool、重複定義、曖昧なownerを増やしていないか。

Architecture observation、co-change、hotspotなどのsensorは調査材料であり、数値だけをfailure判定にしない。

## Findings

findingは重大度順に並べ、次を含める。

- 対象ファイルと位置
- 破綻する入力、状態、操作、または将来変更
- diffまたは正本文書から確認できる根拠
- 必要な修正または不足している検証

styleだけの指摘は、bug、誤解、責務逸脱を生む場合を除いてfindingにしない。問題がなければ、確認したdiff、実行または確認した検証、残る未確認riskを明記する。

## Completion

親agentはactionable findingを修正し、focused verificationとlocal commitを更新してから同じreviewerへ再確認を依頼する。reviewerを起動できない、review対象を特定できない、またはfindingを解消できない場合は完了とせずblockedとする。
