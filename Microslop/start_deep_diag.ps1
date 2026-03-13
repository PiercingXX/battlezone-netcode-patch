[CmdletBinding()]
param(
    [string]$GamePath = "",
    [string]$PingTarget = "1.1.1.1",
    [string]$GameExeName = "BZ98R.exe"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$stateRoot = Join-Path $repoRoot "test_bundles\deep_diag_state"
New-Item -ItemType Directory -Path $stateRoot -Force | Out-Null

$currentFile = Join-Path $stateRoot "windows_current_session.txt"
if (Test-Path $currentFile) {
    $existing = Get-Content $currentFile -Raw
    if ($existing -and (Test-Path $existing.Trim())) {
        Write-Host "ERROR: deep diagnostics already running: $existing" -ForegroundColor Red
        Write-Host "Run .\Microslop\stop_deep_diag.ps1 first." -ForegroundColor Yellow
        exit 1
    }
}

if (-not $GamePath) {
    $defaultPaths = @(
        "C:\Program Files (x86)\Steam\steamapps\common\Battlezone 98 Redux",
        "C:\Program Files\Steam\steamapps\common\Battlezone 98 Redux",
        "$env:PROGRAMFILES\Steam\steamapps\common\Battlezone 98 Redux"
    )
    foreach ($p in $defaultPaths) {
        if (Test-Path $p) { $GamePath = $p; break }
    }
}

if (-not $GamePath -or -not (Test-Path $GamePath)) {
    Write-Host "ERROR: game folder not found. Pass -GamePath explicitly." -ForegroundColor Red
    exit 1
}

$utcStamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
$startIso = (Get-Date).ToUniversalTime().ToString("o")
$hostName = $env:COMPUTERNAME
$sessionDir = Join-Path $repoRoot "test_bundles\deep_windows_${hostName}_${utcStamp}"
New-Item -ItemType Directory -Path $sessionDir -Force | Out-Null

$sessionDir | Out-File -FilePath $currentFile -Encoding utf8
$GamePath | Out-File -FilePath (Join-Path $sessionDir "game_path.txt") -Encoding utf8
$PingTarget | Out-File -FilePath (Join-Path $sessionDir "ping_target.txt") -Encoding utf8
$startIso | Out-File -FilePath (Join-Path $sessionDir "start_utc.txt") -Encoding utf8

$sessionInfo = @(
    "start_utc=$startIso",
    "host_name=$hostName",
    "user_name=$env:USERNAME",
    "game_path=$GamePath",
    "ping_target=$PingTarget",
    "game_exe_name=$GameExeName",
    "powershell_version=$($PSVersionTable.PSVersion)",
    "os_caption=$((Get-CimInstance Win32_OperatingSystem).Caption)",
    "os_version=$((Get-CimInstance Win32_OperatingSystem).Version)"
)
$sessionInfo | Out-File -FilePath (Join-Path $sessionDir "session_info.txt") -Encoding utf8

ipconfig /all | Out-File -FilePath (Join-Path $sessionDir "ipconfig_start.txt") -Encoding utf8
route print | Out-File -FilePath (Join-Path $sessionDir "route_start.txt") -Encoding utf8

# Continuous ping timeline.
$pingLog = Join-Path $sessionDir "ping_timeline.log"
$pingProc = Start-Process -FilePath "ping.exe" -ArgumentList @($PingTarget, "-t") -RedirectStandardOutput $pingLog -WindowStyle Hidden -PassThru

# Periodic netstat snapshots.
$netstatScript = @"
while (`$true) {
  Add-Content -Path '$($sessionDir -replace "'", "''")\\socket_timeline.log' -Value ('===== ' + [DateTime]::UtcNow.ToString('o') + ' =====')
  netstat -s | Add-Content -Path '$($sessionDir -replace "'", "''")\\socket_timeline.log'
  netstat -ano | Add-Content -Path '$($sessionDir -replace "'", "''")\\socket_timeline.log'
  Add-Content -Path '$($sessionDir -replace "'", "''")\\socket_timeline.log' -Value ''
  Start-Sleep -Seconds 5
}
"@
$encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($netstatScript))
$netstatProc = Start-Process -FilePath "powershell.exe" -ArgumentList @("-NoProfile", "-EncodedCommand", $encoded) -WindowStyle Hidden -PassThru

# Optional ProcDump crash watcher if installed.
$procdumpCandidates = @(
    (Get-Command procdump.exe -ErrorAction SilentlyContinue | ForEach-Object { $_.Source }),
    "C:\Program Files\Sysinternals\procdump.exe",
    "C:\Sysinternals\procdump.exe"
) | Where-Object { $_ -and (Test-Path $_) }

$procdumpProc = $null
$dumpDir = Join-Path $sessionDir "dumps"
New-Item -ItemType Directory -Path $dumpDir -Force | Out-Null
if ($procdumpCandidates.Count -gt 0) {
    $procdumpExe = $procdumpCandidates[0]
    $procdumpArgs = @("-accepteula", "-ma", "-e", "-w", $GameExeName, $dumpDir)
    $procdumpProc = Start-Process -FilePath $procdumpExe -ArgumentList $procdumpArgs -WindowStyle Hidden -PassThru
    "procdump_path=$procdumpExe" | Out-File -FilePath (Join-Path $sessionDir "procdump_status.txt") -Encoding utf8
} else {
    "procdump_path=not_found" | Out-File -FilePath (Join-Path $sessionDir "procdump_status.txt") -Encoding utf8
}

# Optional netsh ETW trace (requires elevated PowerShell).
$netTracePath = Join-Path $sessionDir "nettrace.etl"
try {
    netsh trace start capture=yes report=yes persistent=no maxsize=512 tracefile="$netTracePath" | Out-Null
    "netsh_trace=started" | Out-File -FilePath (Join-Path $sessionDir "netsh_trace_status.txt") -Encoding utf8
} catch {
    "netsh_trace=failed_or_not_admin" | Out-File -FilePath (Join-Path $sessionDir "netsh_trace_status.txt") -Encoding utf8
}

$state = @{
    sessionDir = $sessionDir
    startUtc = $startIso
    gamePath = $GamePath
    pingTarget = $PingTarget
    pingPid = $pingProc.Id
    netstatPid = $netstatProc.Id
    procdumpPid = if ($procdumpProc) { $procdumpProc.Id } else { 0 }
    gameExeName = $GameExeName
}
$state | ConvertTo-Json | Out-File -FilePath (Join-Path $sessionDir "state.json") -Encoding utf8

Write-Host "Deep diagnostics started."
Write-Host "Session dir: $sessionDir"
Write-Host "Ping target: $PingTarget"
Write-Host ""
Write-Host "Next:"
Write-Host "1) Run your test match."
Write-Host "2) Run .\Microslop\stop_deep_diag.ps1"