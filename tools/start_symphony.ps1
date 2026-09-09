[CmdletBinding()]
param(
    [ValidateSet("implement", "review")]
    [string]$Mode = "implement"
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
    throw "GITHUB_TOKEN is required. Use a repo-scoped fine-grained token with Issues read/write permission."
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$symphonyRoot = "D:\GitHub\symphony\elixir"
$workflow = if ($Mode -eq "review") {
    Join-Path $repoRoot "WORKFLOW.review.md"
} else {
    Join-Path $repoRoot "WORKFLOW.md"
}
$port = if ($Mode -eq "review") { 4001 } else { 4000 }
$logsRoot = Join-Path "D:\GitHub\wire-symphony-logs" $Mode

if (-not (Test-Path -LiteralPath $symphonyRoot -PathType Container)) {
    throw "Symphony checkout not found: $symphonyRoot"
}
if (-not (Test-Path -LiteralPath $workflow -PathType Leaf)) {
    throw "Workflow not found: $workflow"
}
if (-not (Get-Command mise -ErrorAction SilentlyContinue)) {
    throw "mise is not available on PATH."
}
if (-not (Get-Command codex -ErrorAction SilentlyContinue)) {
    throw "codex is not available on PATH."
}

& codex login status
if ($LASTEXITCODE -ne 0) {
    throw "Codex login is not ready."
}

New-Item -ItemType Directory -Force -Path $logsRoot | Out-Null

Push-Location $symphonyRoot
try {
    & mise exec -- escript .\bin\symphony $workflow --logs-root $logsRoot --port $port
    if ($LASTEXITCODE -ne 0) {
        throw "Symphony exited with code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}
