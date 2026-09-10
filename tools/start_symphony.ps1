[CmdletBinding()]
param(
    [ValidateSet("all", "implement", "investigate", "review")]
    [string]$Mode = "all",
    [string]$RoutineModel = "",
    [string]$UpperModel = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
    throw "GITHUB_TOKEN is required. Use a repo-scoped fine-grained token with Issues read/write permission."
}

if (-not [string]::IsNullOrWhiteSpace($RoutineModel)) {
    $env:SYMPHONY_ROUTINE_MODEL = $RoutineModel
} elseif ([string]::IsNullOrWhiteSpace($env:SYMPHONY_ROUTINE_MODEL)) {
    $env:SYMPHONY_ROUTINE_MODEL = "gpt-5.6-luna"
}

if (-not [string]::IsNullOrWhiteSpace($UpperModel)) {
    $env:SYMPHONY_UPPER_MODEL = $UpperModel
} elseif ([string]::IsNullOrWhiteSpace($env:SYMPHONY_UPPER_MODEL)) {
    $env:SYMPHONY_UPPER_MODEL = "gpt-5.6-sol"
}

if ([string]::IsNullOrWhiteSpace($env:SYMPHONY_ROUTINE_EFFORT)) {
    $env:SYMPHONY_ROUTINE_EFFORT = "max"
}
if ([string]::IsNullOrWhiteSpace($env:SYMPHONY_UPPER_EFFORT)) {
    $env:SYMPHONY_UPPER_EFFORT = "medium"
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$symphonyRoot = "D:\GitHub\symphony\elixir"

if ($Mode -eq "all") {
    $powershellPath = (Get-Process -Id $PID).Path
    $launcherLogs = "D:\GitHub\wire-symphony-logs\launcher"
    New-Item -ItemType Directory -Force -Path $launcherLogs | Out-Null

    $children = foreach ($childMode in @("implement", "investigate")) {
        $stdout = Join-Path $launcherLogs "$childMode.stdout.log"
        $stderr = Join-Path $launcherLogs "$childMode.stderr.log"
        Start-Process `
            -FilePath $powershellPath `
            -ArgumentList @("-NoProfile", "-File", $PSCommandPath, $childMode) `
            -WindowStyle Hidden `
            -RedirectStandardOutput $stdout `
            -RedirectStandardError $stderr `
            -PassThru
    }

    Write-Host "Symphony implement and investigation queues started."
    Write-Host "Routine model: $env:SYMPHONY_ROUTINE_MODEL ($env:SYMPHONY_ROUTINE_EFFORT)"
    Write-Host "Upper model:   $env:SYMPHONY_UPPER_MODEL ($env:SYMPHONY_UPPER_EFFORT)"
    Write-Host "Dashboards: http://localhost:4000/ and http://localhost:4001/"
    Write-Host "Logs: $launcherLogs"
    Write-Host "Keep this terminal open. Press Ctrl+C to stop both queues."

    try {
        while ($true) {
            Start-Sleep -Seconds 2
            $exited = @($children | Where-Object HasExited)
            if ($exited.Count -gt 0) {
                $details = ($exited | ForEach-Object { "PID $($_.Id), exit $($_.ExitCode)" }) -join "; "
                throw "A Symphony queue stopped unexpectedly: $details. Check $launcherLogs."
            }
        }
    } finally {
        foreach ($child in $children) {
            if (-not $child.HasExited) {
                & taskkill.exe /PID $child.Id /T /F | Out-Null
            }
        }
    }
}

$workflow = switch ($Mode) {
    "investigate" { Join-Path $repoRoot "WORKFLOW.investigate.md" }
    "review" { Join-Path $repoRoot "WORKFLOW.review.md" }
    default { Join-Path $repoRoot "WORKFLOW.md" }
}
$port = switch ($Mode) {
    "investigate" { 4001 }
    "review" { 4002 }
    default { 4000 }
}
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
