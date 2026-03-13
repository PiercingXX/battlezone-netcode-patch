[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$stateRoot = Join-Path $repoRoot "test_bundles\deep_diag_state"
$currentFile = Join-Path $stateRoot "windows_current_session.txt"

if (-not (Test-Path $currentFile)) {
    Write-Host "ERROR: no active Windows deep diagnostics session found." -ForegroundColor Red
    Write-Host "Run .\Microslop\start_deep_diag.ps1 first." -ForegroundColor Yellow
    exit 1
}

$sessionDir = (Get-Content $currentFile -Raw).Trim()
if (-not $sessionDir -or -not (Test-Path $sessionDir)) {
    Write-Host "ERROR: session directory missing: $sessionDir" -ForegroundColor Red
    Remove-Item -Force $currentFile -ErrorAction SilentlyContinue
    exit 1
}

$statePath = Join-Path $sessionDir "state.json"
if (-not (Test-Path $statePath)) {
    Write-Host "ERROR: session state missing: $statePath" -ForegroundColor Red
    Remove-Item -Force $currentFile -ErrorAction SilentlyContinue
    exit 1
}

$state = Get-Content $statePath -Raw | ConvertFrom-Json

function Stop-PidIfAlive {
    param([int]$PidToStop)
    if ($PidToStop -le 0) { return }
    try {
        $p = Get-Process -Id $PidToStop -ErrorAction SilentlyContinue
        if ($p) {
            Stop-Process -Id $PidToStop -Force -ErrorAction SilentlyContinue
        }
    } catch {
    }
}

Stop-PidIfAlive -PidToStop ([int]$state.pingPid)
Stop-PidIfAlive -PidToStop ([int]$state.netstatPid)
Stop-PidIfAlive -PidToStop ([int]$state.procdumpPid)

# Stop netsh trace if it was started.
try {
    netsh trace stop | Out-File -FilePath (Join-Path $sessionDir "netsh_trace_stop.txt") -Encoding utf8
} catch {
    "netsh_trace_stop_failed_or_not_running" | Out-File -FilePath (Join-Path $sessionDir "netsh_trace_stop.txt") -Encoding utf8
}

ipconfig /all | Out-File -FilePath (Join-Path $sessionDir "ipconfig_end.txt") -Encoding utf8
route print | Out-File -FilePath (Join-Path $sessionDir "route_end.txt") -Encoding utf8

$gamePath = "$($state.gamePath)"
if ($gamePath -and (Test-Path $gamePath)) {
    $copyList = @(
        "BZLogger.txt",
        "winmm_proxy.log",
        "dsound_proxy.log",
        "multi.ini"
    )
    foreach ($name in $copyList) {
        $src = Join-Path $gamePath $name
        if (Test-Path $src) {
            Copy-Item -Force $src (Join-Path $sessionDir $name)
        }
    }
}

# Export event window for lag/crash correlation.
$startUtc = Get-Date $state.startUtc
$endUtc = Get-Date
try {
    Get-WinEvent -FilterHashtable @{ LogName = "Application"; StartTime = $startUtc; EndTime = $endUtc } |
        Select-Object TimeCreated, Id, LevelDisplayName, ProviderName, Message |
        Format-List | Out-File -FilePath (Join-Path $sessionDir "events_application.txt") -Encoding utf8
} catch {
    "application_event_export_failed" | Out-File -FilePath (Join-Path $sessionDir "events_application.txt") -Encoding utf8
}

try {
    Get-WinEvent -FilterHashtable @{ LogName = "System"; StartTime = $startUtc; EndTime = $endUtc } |
        Select-Object TimeCreated, Id, LevelDisplayName, ProviderName, Message |
        Format-List | Out-File -FilePath (Join-Path $sessionDir "events_system.txt") -Encoding utf8
} catch {
    "system_event_export_failed" | Out-File -FilePath (Join-Path $sessionDir "events_system.txt") -Encoding utf8
}

# Re-run verifier at stop for current patch state.
$verifyOut = Join-Path $sessionDir "verify_output.txt"
try {
    & (Join-Path $PSScriptRoot "verify_windows.ps1") -GamePath $gamePath *>&1 | Tee-Object -FilePath $verifyOut | Out-Null
} catch {
    $_ | Out-File -FilePath $verifyOut -Encoding utf8
}

$zipPath = "$sessionDir.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path (Join-Path $sessionDir "*") -DestinationPath $zipPath

Remove-Item -Force $currentFile -ErrorAction SilentlyContinue

Write-Host "Deep diagnostics stopped."
Write-Host "Bundle created: $zipPath"
Write-Host "Send this file back to the test coordinator."