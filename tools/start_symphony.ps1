[CmdletBinding()]
param(
    [ValidateSet("all", "implement", "investigate", "review")]
    [string]$Mode = "all",
    [string]$RoutineModel = "",
    [string]$UpperModel = "",
    [switch]$AcknowledgePreviewRisk
)

$ErrorActionPreference = "Stop"

function Resolve-CodexExecutable {
    if (-not [string]::IsNullOrWhiteSpace($env:SYMPHONY_CODEX_PATH) -and
        (Test-Path -LiteralPath $env:SYMPHONY_CODEX_PATH -PathType Leaf)) {
        return (Resolve-Path -LiteralPath $env:SYMPHONY_CODEX_PATH).Path
    }

    $command = Get-Command codex -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $command) {
        return $command.Source
    }

    $codexBinRoot = Join-Path $env:LOCALAPPDATA "OpenAI\Codex\bin"
    if (Test-Path -LiteralPath $codexBinRoot -PathType Container) {
        $candidate = Get-ChildItem -LiteralPath $codexBinRoot -Filter "codex.exe" -File -Recurse |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($null -ne $candidate) {
            return $candidate.FullName
        }
    }

    throw "codex.exe was not found on PATH or under $codexBinRoot. Start Codex once or reinstall the Codex app."
}

function Resolve-GitBashExecutable {
    $git = Get-Command git -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $git) {
        throw "git.exe is not available on PATH. Install Git for Windows."
    }

    $gitRoot = Split-Path -Parent (Split-Path -Parent $git.Source)
    foreach ($relativePath in @("bin\bash.exe", "usr\bin\bash.exe")) {
        $candidate = Join-Path $gitRoot $relativePath
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw "Git for Windows bash.exe was not found under $gitRoot."
}

if ([string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
    throw "GITHUB_TOKEN is required. Use a repo-scoped fine-grained token with Issues read/write permission."
}
if (-not $AcknowledgePreviewRisk) {
    throw "Symphony requires explicit acknowledgement that this engineering preview runs without the usual guardrails. Re-run with -AcknowledgePreviewRisk."
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
$codexPath = Resolve-CodexExecutable
$gitBashPath = Resolve-GitBashExecutable
$gitBashDirectory = Split-Path -Parent $gitBashPath
$env:PATH = "$gitBashDirectory;$env:PATH"
$env:SYMPHONY_CODEX_PATH = $codexPath.Replace("\", "/")

if ($Mode -eq "all") {
    $powershellPath = (Get-Process -Id $PID).Path
    $launcherLogs = "D:\GitHub\wire-symphony-logs\launcher"
    New-Item -ItemType Directory -Force -Path $launcherLogs | Out-Null

    $children = foreach ($childMode in @("implement", "investigate")) {
        $stdout = Join-Path $launcherLogs "$childMode.stdout.log"
        $stderr = Join-Path $launcherLogs "$childMode.stderr.log"
        $process = Start-Process `
            -FilePath $powershellPath `
            -ArgumentList @("-NoProfile", "-File", $PSCommandPath, $childMode, "-AcknowledgePreviewRisk") `
            -WindowStyle Hidden `
            -RedirectStandardOutput $stdout `
            -RedirectStandardError $stderr `
            -PassThru
        [pscustomobject]@{
            Mode = $childMode
            Process = $process
        }
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
            $exited = @($children | Where-Object { $_.Process.HasExited })
            if ($exited.Count -gt 0) {
                $details = ($exited | ForEach-Object {
                    $_.Process.WaitForExit()
                    $_.Process.Refresh()
                    "$($_.Mode) PID $($_.Process.Id), exit $($_.Process.ExitCode)"
                }) -join "; "
                throw "A Symphony queue stopped unexpectedly: $details. Check $launcherLogs."
            }
        }
    } finally {
        foreach ($child in $children) {
            if (-not $child.Process.HasExited) {
                & taskkill.exe /PID $child.Process.Id /T /F | Out-Null
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
& $codexPath login status
if ($LASTEXITCODE -ne 0) {
    throw "Codex login is not ready."
}

New-Item -ItemType Directory -Force -Path $logsRoot | Out-Null

Push-Location $symphonyRoot
try {
    & mise exec -- escript .\bin\symphony --i-understand-that-this-will-be-running-without-the-usual-guardrails $workflow --logs-root $logsRoot --port $port
    if ($LASTEXITCODE -ne 0) {
        throw "Symphony exited with code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}
