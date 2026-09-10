[CmdletBinding()]
param(
    [string]$LogsRoot,
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($LogsRoot)) {
    if (-not [string]::IsNullOrWhiteSpace($env:SYMPHONY_LOGS_ROOT)) {
        $LogsRoot = $env:SYMPHONY_LOGS_ROOT
    }
    else {
        $repoRoot = Split-Path -Parent $PSScriptRoot
        $LogsRoot = Join-Path (Split-Path -Parent $repoRoot) "wire-symphony-logs"
    }
}

if (-not (Test-Path -LiteralPath $LogsRoot -PathType Container)) {
    throw "Symphony logs root was not found: $LogsRoot"
}

$usagePattern = [regex]::new(
    '^(?<timestamp>\S+)\s+info:\s+Trial usage:\s+' +
    'queue=(?<queue>\S+)\s+' +
    'issue_id=(?<issue_id>\S+)\s+' +
    'issue_identifier=(?<identifier>\S+)\s+' +
    'input_tokens=(?<input_tokens>\d+)\s+' +
    'output_tokens=(?<output_tokens>\d+)\s+' +
    'total_tokens=(?<total_tokens>\d+)\s+' +
    'runtime_seconds=(?<runtime_seconds>\d+)\s+' +
    'turns=(?<turns>\d+)\s+' +
    'session_id=(?<session_id>\S+)$'
)

$rows = foreach ($queueName in @("implement", "investigate", "decision", "review")) {
    $logDirectory = Join-Path (Join-Path $LogsRoot $queueName) "log"
    if (-not (Test-Path -LiteralPath $logDirectory -PathType Container)) {
        continue
    }

    $logFiles = Get-ChildItem -LiteralPath $logDirectory -Filter "symphony.log*" -File |
        Where-Object { $_.Extension -notin @(".idx", ".siz") }

    foreach ($logFile in $logFiles) {
        foreach ($line in Get-Content -LiteralPath $logFile.FullName) {
            $match = $usagePattern.Match($line)
            if (-not $match.Success) {
                continue
            }

            [pscustomobject]@{
                Timestamp      = [datetimeoffset]::Parse($match.Groups["timestamp"].Value)
                Queue          = $queueName
                Workflow       = $match.Groups["queue"].Value
                IssueId        = $match.Groups["issue_id"].Value
                Identifier     = $match.Groups["identifier"].Value
                InputTokens    = [long]$match.Groups["input_tokens"].Value
                OutputTokens   = [long]$match.Groups["output_tokens"].Value
                TotalTokens    = [long]$match.Groups["total_tokens"].Value
                RuntimeSeconds = [long]$match.Groups["runtime_seconds"].Value
                Turns          = [int]$match.Groups["turns"].Value
                SessionId      = $match.Groups["session_id"].Value
            }
        }
    }
}

$rows = @($rows | Sort-Object Timestamp, Queue, Identifier)

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $rows
}
else {
    $outputDirectory = Split-Path -Parent $OutputPath
    if (-not [string]::IsNullOrWhiteSpace($outputDirectory)) {
        New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    }
    $rows | Export-Csv -LiteralPath $OutputPath -NoTypeInformation -Encoding utf8
}
