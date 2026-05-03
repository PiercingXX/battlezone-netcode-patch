[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Start", "Stop")]
    [string]$Action,
    [string]$GamePath = "",
    [int]$PayloadBytes = 32,
    [int]$RingRecords = 65536,
    [string]$PeerFilter = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$stateRoot = Join-Path $repoRoot "test_bundles\buffer_log_state"
$currentFile = Join-Path $stateRoot "windows_current_session.txt"
New-Item -ItemType Directory -Path $stateRoot -Force | Out-Null

function Get-ResolvedGamePath {
    param([string]$Candidate)
    if ($Candidate -and (Test-Path $Candidate)) { return $Candidate }
    $defaultPaths = @(
        "C:\Program Files (x86)\Steam\steamapps\common\Battlezone 98 Redux",
        "C:\Program Files\Steam\steamapps\common\Battlezone 98 Redux",
        "$env:PROGRAMFILES\Steam\steamapps\common\Battlezone 98 Redux"
    )
    foreach ($p in $defaultPaths) {
        if (Test-Path $p) { return $p }
    }
    return ""
}

function Collect-File {
    param(
        [string]$Source,
        [string]$DestinationDir,
        [string]$StatusFile
    )

    $base = Split-Path -Leaf $Source
    if (Test-Path $Source) {
        Copy-Item -Force $Source (Join-Path $DestinationDir $base)
        $bytes = (Get-Item $Source).Length
        Add-Content -Path $StatusFile -Value "found $base bytes=$bytes"
    } else {
        Add-Content -Path $StatusFile -Value "missing $base"
    }
}

function Write-LaunchOptions {
    param(
        [string]$OutFile,
        [int]$PayloadBytesValue,
        [int]$RingRecordsValue,
        [string]$PeerFilterValue
    )

    $line = "set BZ_BUFFER_LOG=1 && set BZ_BUFFER_LOG_BYTES=$PayloadBytesValue && set BZ_BUFFER_LOG_RING=$RingRecordsValue"
    if ($PeerFilterValue) {
        $line += " && set BZ_BUFFER_LOG_PEER=$PeerFilterValue"
    }
    $line += " && %command%"

    @(
        "Windows Steam launch options for buffer logging:",
        "",
        $line
    ) | Out-File -FilePath $OutFile -Encoding utf8
}

function Start-Session {
    $resolvedGamePath = Get-ResolvedGamePath -Candidate $GamePath
    if (-not $resolvedGamePath) {
        Write-Host "ERROR: game folder not found. Pass -GamePath explicitly." -ForegroundColor Red
        exit 1
    }

    if (Test-Path $currentFile) {
        $existing = (Get-Content $currentFile -Raw).Trim()
        if ($existing -and (Test-Path $existing)) {
            Write-Host "ERROR: buffer logging session already active: $existing" -ForegroundColor Red
            exit 1
        }
    }

    $utcStamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
    $sessionDir = Join-Path $repoRoot "test_bundles\buffer_windows_$env:COMPUTERNAME`_$utcStamp"
    New-Item -ItemType Directory -Path $sessionDir -Force | Out-Null

    $sessionDir | Out-File -FilePath $currentFile -Encoding utf8
    $resolvedGamePath | Out-File -FilePath (Join-Path $sessionDir "game_path.txt") -Encoding utf8
    $PayloadBytes | Out-File -FilePath (Join-Path $sessionDir "payload_bytes.txt") -Encoding utf8
    $RingRecords | Out-File -FilePath (Join-Path $sessionDir "ring_records.txt") -Encoding utf8
    $PeerFilter | Out-File -FilePath (Join-Path $sessionDir "peer_filter.txt") -Encoding utf8
    (Get-Date).ToUniversalTime().ToString("o") | Out-File -FilePath (Join-Path $sessionDir "start_utc.txt") -Encoding utf8

    Write-LaunchOptions -OutFile (Join-Path $sessionDir "launch_options.txt") -PayloadBytesValue $PayloadBytes -RingRecordsValue $RingRecords -PeerFilterValue $PeerFilter

    @(
        "1. Copy the Steam launch options from launch_options.txt.",
        "2. Start Battlezone 98 Redux.",
        "3. Reproduce the packet-order issue.",
        "4. Run .\buffer-logging\buffer_logger_windows.ps1 -Action Stop",
        "",
        "Expected lightweight outputs from the game folder:",
        "- winmm_proxy.log",
        "- bz_buffer_log.bin",
        "- bz_buffer_log.meta.txt",
        "- BZLogger.txt"
    ) | Out-File -FilePath (Join-Path $sessionDir "README_NEXT_STEPS.txt") -Encoding utf8

    Write-Host "Buffer logging session started."
    Write-Host "Session dir: $sessionDir"
    Write-Host "Launch options saved to: $(Join-Path $sessionDir 'launch_options.txt')"
}

function Stop-Session {
    if (-not (Test-Path $currentFile)) {
        Write-Host "ERROR: no active Windows buffer logging session found." -ForegroundColor Red
        exit 1
    }

    $sessionDir = (Get-Content $currentFile -Raw).Trim()
    if (-not $sessionDir -or -not (Test-Path $sessionDir)) {
        Write-Host "ERROR: session directory missing: $sessionDir" -ForegroundColor Red
        Remove-Item -Force $currentFile -ErrorAction SilentlyContinue
        exit 1
    }

    $gamePathResolved = (Get-Content (Join-Path $sessionDir "game_path.txt") -Raw).Trim()
    $statusFile = Join-Path $sessionDir "collection_status.txt"
    Set-Content -Path $statusFile -Value ""

    Collect-File -Source (Join-Path $gamePathResolved "BZLogger.txt") -DestinationDir $sessionDir -StatusFile $statusFile
    Collect-File -Source (Join-Path $gamePathResolved "winmm_proxy.log") -DestinationDir $sessionDir -StatusFile $statusFile
    Collect-File -Source (Join-Path $gamePathResolved "bz_buffer_log.bin") -DestinationDir $sessionDir -StatusFile $statusFile
    Collect-File -Source (Join-Path $gamePathResolved "bz_buffer_log.meta.txt") -DestinationDir $sessionDir -StatusFile $statusFile
    Collect-File -Source (Join-Path $gamePathResolved "multi.ini") -DestinationDir $sessionDir -StatusFile $statusFile

    $zipPath = "$sessionDir.zip"
    if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
    Compress-Archive -Path (Join-Path $sessionDir "*") -DestinationPath $zipPath

    Remove-Item -Force $currentFile -ErrorAction SilentlyContinue
    Write-Host "Buffer logging session stopped."
    Write-Host "Bundle directory: $sessionDir"
    Write-Host "Archive created: $zipPath"
}

switch ($Action) {
    "Start" { Start-Session }
    "Stop" { Stop-Session }
}