[CmdletBinding()]
param(
    [ValidateSet("all", "implement", "investigate", "decision", "review")]
    [string]$Mode = "all",
    [string]$RoutineModel = "gpt-5.6-luna",
    [string]$UpperModel = "gpt-5.6-sol",
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

function Assert-SymphonyPortsAvailable {
    param([int[]]$Ports)

    $occupied = foreach ($portNumber in $Ports) {
        foreach ($listener in @(Get-NetTCPConnection -State Listen -LocalPort $portNumber -ErrorAction SilentlyContinue)) {
            $process = Get-Process -Id $listener.OwningProcess -ErrorAction SilentlyContinue
            [pscustomobject]@{
                Port = $portNumber
                Pid = $listener.OwningProcess
                Process = if ($null -ne $process) { $process.ProcessName } else { "unknown" }
            }
        }
    }

    if (@($occupied).Count -gt 0) {
        $details = ($occupied | ForEach-Object {
            "port $($_.Port) (PID $($_.Pid), $($_.Process))"
        }) -join "; "
        throw "A Symphony dashboard port is already in use: $details. Stop the existing queue before starting another launcher."
    }
}

if ([string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
    throw "GITHUB_TOKEN is required. Use a repo-scoped fine-grained token with Issues, Contents, and Pull requests read/write permission."
}
if (-not $AcknowledgePreviewRisk) {
    throw "Symphony requires explicit acknowledgement that this engineering preview runs without the usual guardrails. Re-run with -AcknowledgePreviewRisk."
}

$env:SYMPHONY_ROUTINE_MODEL = $RoutineModel
$env:SYMPHONY_UPPER_MODEL = $UpperModel

if ([string]::IsNullOrWhiteSpace($env:SYMPHONY_ROUTINE_EFFORT)) {
    $env:SYMPHONY_ROUTINE_EFFORT = "max"
}
if ([string]::IsNullOrWhiteSpace($env:SYMPHONY_UPPER_EFFORT)) {
    $env:SYMPHONY_UPPER_EFFORT = "medium"
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$defaultSiblingRoot = Split-Path -Parent $repoRoot
$symphonyRoot = if ([string]::IsNullOrWhiteSpace($env:SYMPHONY_ROOT)) {
    Join-Path $defaultSiblingRoot "symphony\elixir"
} else {
    $env:SYMPHONY_ROOT
}
$workspaceRoot = if ([string]::IsNullOrWhiteSpace($env:SYMPHONY_WORKSPACE_ROOT)) {
    Join-Path $defaultSiblingRoot "wire-symphony-workspaces"
} else {
    $env:SYMPHONY_WORKSPACE_ROOT
}
$logsRootBase = if ([string]::IsNullOrWhiteSpace($env:SYMPHONY_LOGS_ROOT)) {
    Join-Path $defaultSiblingRoot "wire-symphony-logs"
} else {
    $env:SYMPHONY_LOGS_ROOT
}
$env:SYMPHONY_ROOT = (Resolve-Path -LiteralPath $symphonyRoot).Path
$resolvedWorkspaceRoot = Resolve-Path -LiteralPath $workspaceRoot -ErrorAction SilentlyContinue
if ($null -ne $resolvedWorkspaceRoot) {
    $env:SYMPHONY_WORKSPACE_ROOT = $resolvedWorkspaceRoot.Path
} else {
    $env:SYMPHONY_WORKSPACE_ROOT = $workspaceRoot
}
$env:SYMPHONY_LOGS_ROOT = $logsRootBase
$codexPath = Resolve-CodexExecutable
$gitBashPath = Resolve-GitBashExecutable
$gitBashDirectory = Split-Path -Parent $gitBashPath
$env:PATH = "$gitBashDirectory;$env:PATH"
$serenaBin = Join-Path $env:USERPROFILE ".local\bin"
if (Test-Path -LiteralPath $serenaBin -PathType Container) {
    $env:PATH = "$serenaBin;$env:PATH"
}
if (-not (Get-Command serena -ErrorAction SilentlyContinue)) {
    throw "Serena is not available. Install it with: uv tool install -p 3.13 serena-agent"
}
$env:GIT_TERMINAL_PROMPT = "0"
$env:GCM_INTERACTIVE = "Never"
$env:SYMPHONY_CODEX_PATH = $codexPath.Replace("\", "/")

if ($Mode -eq "all") {
    Assert-SymphonyPortsAvailable -Ports @(4000, 4001, 4002, 4003)

    $powershellPath = (Get-Process -Id $PID).Path
    $launcherLogs = Join-Path $logsRootBase "launcher"
    New-Item -ItemType Directory -Force -Path $launcherLogs | Out-Null

    $children = foreach ($childMode in @("implement", "investigate", "decision", "review")) {
        $stdout = Join-Path $launcherLogs "$childMode.stdout.log"
        $stderr = Join-Path $launcherLogs "$childMode.stderr.log"
        $process = Start-Process `
            -FilePath $powershellPath `
            -ArgumentList @(
                "-NoProfile", "-File", $PSCommandPath, $childMode,
                "-RoutineModel", $RoutineModel,
                "-UpperModel", $UpperModel,
                "-AcknowledgePreviewRisk"
            ) `
            -WindowStyle Hidden `
            -RedirectStandardOutput $stdout `
            -RedirectStandardError $stderr `
            -PassThru
        [pscustomobject]@{
            Mode = $childMode
            Process = $process
        }
    }

    Write-Host "Symphony implement, investigation, decision, and review queues started."
    Write-Host "Routine model: $env:SYMPHONY_ROUTINE_MODEL ($env:SYMPHONY_ROUTINE_EFFORT)"
    Write-Host "Upper model:   $env:SYMPHONY_UPPER_MODEL ($env:SYMPHONY_UPPER_EFFORT)"
    Write-Host "Dashboards: implement 4000, investigate 4001, review 4002, decision 4003"
    Write-Host "Logs: $launcherLogs"
    Write-Host "Keep this terminal open. Press Ctrl+C to stop all queues."

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
    "decision" { Join-Path $repoRoot "WORKFLOW.decision.md" }
    "review" { Join-Path $repoRoot "WORKFLOW.review.md" }
    default { Join-Path $repoRoot "WORKFLOW.md" }
}
$port = switch ($Mode) {
    "investigate" { 4001 }
    "decision" { 4003 }
    "review" { 4002 }
    default { 4000 }
}
$logsRoot = Join-Path $logsRootBase $Mode

Assert-SymphonyPortsAvailable -Ports @($port)

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
