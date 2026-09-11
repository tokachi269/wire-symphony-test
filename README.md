# wire

電柱・電線networkを生成・編集・検証するnative C++ coreと、開発用viewer。

## 現在の構成

- `core`: data model、backbone generation、post-edit derive、validation
- `viewer`: core出力を表示・操作する開発用consumer
- `tools`: architecture/test family lint
- `docs`: 現行設計、残存legacy、merge条件、test、実行手順

`SavedBackboneGraph`をtopology authorityとし、rules、layout、geom、drawを一方向に派生する。
viewerは不足したtopologyやplacementを推測しない。

## ドキュメント

1. [repository architecture](docs/architecture.md)
2. [wire architecture](docs/wire/architecture.md)
3. [merge readiness](docs/merge_readiness.md)
4. [testing](docs/testing.md)
5. [portable agent engineering harness](docs/engineering/agent_harness.md)
6. [command cheatsheet](docs/command_cheatsheet.md)
7. [wire documents](docs/wire/README.md)
8. [road documents](docs/road/README.md)

## Quick check

```powershell
python tools\arch_lint.py
python tools\test_family_lint.py
& .\build-vs18-coretests\domains\wire\Debug\wire_core_tests.exe
& .\build-vs18-coretests\domains\wire\Debug\wire_core_tests.exe backbone
```

詳細なbuild/viewerコマンドは[command_cheatsheet.md](docs/command_cheatsheet.md)を参照する。
