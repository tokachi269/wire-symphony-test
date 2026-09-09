# command_cheatsheet.md

workdir: `D:\GitHub\wire`

## Core test

```cmd
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build-vs18-coretests -G "Visual Studio 18 2026" -A x64 -DWIRE_BUILD_VIEWER=OFF
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build-vs18-coretests --config Debug --target wire_core_tests
build-vs18-coretests\domains\wire\Debug\wire_core_tests.exe
```

## Viewer

Viewer dependencyのlocal source treeがまだない場合は取得する。

```cmd
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -S . -B build-vs18-viewer-fetch -G "Visual Studio 18 2026" -A x64 -DWIRE_BUILD_VIEWER=ON -DWIRE_VIEWER_FETCH_DEPS=ON
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build-vs18-viewer-fetch --config Debug --target wire_viewer
build-vs18-viewer-fetch\viewer\Debug\wire_viewer.exe
```

`build-viewer\_deps\raylib-src`、`build-viewer\_deps\imgui-src`、`build-viewer\_deps\rlimgui-src`がすでにある場合は、`WIRE_VIEWER_FETCH_DEPS=OFF`でlocal source directoryを使える。

## 注意事項

- 通常のbuild/test経路では`Ninja`は不要。
- 最初に`Developer Command Prompt`または`Developer PowerShell for VS`を開いた場合、`vcvars64.bat`は不要。
- 通常のPowerShellを使う場合は、初期化した同じshell内でbuildと実行を行うか、`cmd.exe /c`で囲む。
- `wire_core_tests`のbuild時にはarchitecture lintとtest-family lint targetも実行される。

## lint

```cmd
python tools\harness\test_architecture_lint.py
python tools\harness\test_architecture_observation.py
python tools\test_arch_lint.py
python tools\arch_lint.py
python tools\test_family_lint.py
git diff --check
```

## CI / delivery

`.github/workflows/ci.yml`はpush、pull request、手動実行でarchitecture contract、Core、Web/WASMを検証する。co-change/DSMはartifactとして保存するが、その数値は合否条件にしない。

タグpushまたは手動実行では、検証後の`web/dist`もGitHub Actions artifactとして保存する。公開先は未定義なので、自動deployは行わない。

## Architecture observation

`graph`のdefault scopeはproduction sourceである。`--scope tests`または`--scope tools`で別scopeを確認できる。DSM、co-change、hotspotの数値はreview sensorでありquality gateではない。Reflexionのdivergence判定は既存architecture lintを再利用する。

```cmd
python tools\architecture_observation.py graph --format markdown
python tools\architecture_observation.py graph --scope tests --format markdown
python tools\architecture_observation.py reflexion --base <task-start-sha> --format markdown
python tools\architecture_observation.py delta --base <task-start-sha> --format markdown
python tools\architecture_observation.py history --window-commits 50 --format markdown
python tools\architecture_observation.py history --window-commits 50 --recent-days 30 --format markdown
python tools\architecture_observation.py hotspot --format markdown
```

`reflexion`は包括的なallowed-dependency graphを仮定せず、既存required/forbidden contract、既存lint結果、unmodeled relationを分けて表示する。`delta`は自動確定できるstructural factsとhuman review candidatesを分離し、pathやtokenからsemantics変更を断定しない。`history`はfirst-parent chain上で各commitをfirst parentとの差分として一度だけ数え、mass-changeをinclusive/exclusiveで表示する。

`history`の既定比較は、最新50 commitsとその直前50 commitsである。`recently strengthened`は両期間のsupportと方向別confidenceの差分を示すが、増加自体を問題とは判定しない。静的関係はdirectの有無だけでなく、両方向の最短dependency pathを`direct`、`1_intermediate`、`2_intermediates`、`3_plus_intermediates`、`unreachable`に分ける。`without a direct static edge`は間接依存まで存在しないという意味ではない。`--recent-days`は日数窓も併記したい場合だけ指定する。

## clang-tidy

`compile_commands.json`を出力するgeneratorでconfigureし、helper targetを実行する。

```cmd
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build-clang-tidy -G Ninja -DWIRE_BUILD_VIEWER=OFF -DWIRE_ENABLE_PCH=OFF -DWIRE_ENABLE_FORMAT_TARGETS=OFF -DWIRE_ENABLE_UML_TARGETS=OFF
cmake --build build-clang-tidy --target tidy-check
```

調整中に小さいsubtreeだけを検査する場合:

```cmd
python tools\clang_tidy_check.py --build-dir build-clang-tidy --path domains/wire/src/state
```

## clang-uml

```cmd
powershell -NoProfile -ExecutionPolicy Bypass -File tools\prepare_clang_uml_compile_db.ps1 -WorkspaceRoot .
"C:\Program Files\clang-uml\bin\clang-uml.exe" -c .clang-uml -l
"C:\Program Files\clang-uml\bin\clang-uml.exe" -c .clang-uml -n core_packages -g plantuml -p
```

## Web viewer WASM

`emcc` / `emcmake` が PATH にある Emscripten shell で実行する。

```powershell
Set-Location web
npm install
npm run wasm:build
npm test
npm run build
```

`npm test` は wasm 成果物を前提に実行し、結果報告では skip が 0 であることを確認する。

個別に wasm だけを再構築する場合:

```powershell
Set-Location web
npm run wasm:build
```

開発 server:

```powershell
Set-Location web
npm run dev
```

`npm run dev`は起動時にCore/road/WASM bindingのsource fingerprintを確認する。
生成済みWASMが同じsourceならそのまま起動し、古い場合だけ一度自動buildする。
起動中もCore/road/WASM bindingの変更を監視し、source fingerprintが変わった場合はWASMを自動buildしてブラウザをfull reloadする。
Git commit、docs、test、Web UIだけの変更ではWASMを再buildしない。
