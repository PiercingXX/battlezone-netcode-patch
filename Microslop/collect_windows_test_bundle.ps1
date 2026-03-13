[CmdletBinding()]
param(
    [string]$GamePath = "",
    [string]$TesterName = "",
    [string]$TesterRole = "",
    [string]$MatchType = "",
    [string]$MapName = "",
    [string]$Notes = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$outRoot = Join-Path $repoRoot "test_bundles"
New-Item -ItemType Directory -Path $outRoot -Force | Out-Null

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

if (-not $TesterName) { $TesterName = Read-Host "Tester name (optional)" }
if (-not $TesterRole) { $TesterRole = Read-Host "Role (host/client, optional)" }
if (-not $MatchType) { $MatchType = Read-Host "Match type (1v1/2v2/etc, optional)" }
if (-not $MapName) { $MapName = Read-Host "Map name (optional)" }
if (-not $Notes) { $Notes = Read-Host "Notes (optional)" }

$utcStamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
$hostName = $env:COMPUTERNAME
$bundleName = "windows_${hostName}_${utcStamp}"
$bundleDir = Join-Path $outRoot $bundleName
New-Item -ItemType Directory -Path $bundleDir -Force | Out-Null

Write-Host ""
Write-Host "=== Battlezone Windows test bundle ==="
Write-Host "Game folder: $GamePath"
Write-Host "Bundle dir : $bundleDir"

# Run verifier and capture output.
$verifyOut = Join-Path $bundleDir "verify_output.txt"
"# verify command`n.\Microslop\verify_windows.ps1 -GamePath \"$GamePath\"`n" | Out-File -FilePath $verifyOut -Encoding utf8
try {
    & (Join-Path $PSScriptRoot "verify_windows.ps1") -GamePath $GamePath *>&1 | Tee-Object -FilePath $verifyOut -Append | Out-Null
} catch {
    $_ | Out-File -FilePath $verifyOut -Append -Encoding utf8
}

# Collect expected logs if present.
$copyList = @(
    @{ src = Join-Path $GamePath "BZLogger.txt"; dst = "BZLogger.txt" },
    @{ src = Join-Path $GamePath "winmm_proxy.log"; dst = "winmm_proxy.log" },
    @{ src = Join-Path $GamePath "dsound_proxy.log"; dst = "dsound_proxy.log" },
    @{ src = Join-Path $GamePath "multi.ini"; dst = "multi.ini" }
)

foreach ($item in $copyList) {
    if (Test-Path $item.src) {
        Copy-Item -Force $item.src (Join-Path $bundleDir $item.dst)
    }
}

# Environment + session metadata.
$sessionInfo = @(
    "timestamp_local=$((Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))",
    "timestamp_utc=$utcStamp",
    "tester_name=$TesterName",
    "tester_role=$TesterRole",
    "match_type=$MatchType",
    "map_name=$MapName",
    "notes=$Notes",
    "host_name=$hostName",
    "user_name=$env:USERNAME",
    "game_path=$GamePath",
    "repo_root=$repoRoot",
    "powershell_version=$($PSVersionTable.PSVersion)",
    "os_caption=$((Get-CimInstance Win32_OperatingSystem).Caption)",
    "os_version=$((Get-CimInstance Win32_OperatingSystem).Version)"
)
$sessionInfo | Out-File -FilePath (Join-Path $bundleDir "session_info.txt") -Encoding utf8

# Create zip archive.
$zipPath = Join-Path $outRoot "$bundleName.zip"
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
Compress-Archive -Path (Join-Path $bundleDir "*") -DestinationPath $zipPath

Write-Host ""
if ((Get-Content $verifyOut -Raw) -match "RESULT: PASS") {
    Write-Host "Verifier result: PASS" -ForegroundColor Green
} else {
    Write-Host "Verifier result: FAIL or inconclusive (check verify_output.txt)" -ForegroundColor Yellow
}
Write-Host "Bundle created: $zipPath"
Write-Host "Send this file back to the test coordinator."
