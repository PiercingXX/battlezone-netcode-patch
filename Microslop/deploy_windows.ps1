# deploy_windows.ps1
# Battlezone 98 Redux - Windows netcode patch
#
# Copies the pre-built winmm.dll proxy into the Steam game folder.
# Run this from the repo root on the Windows machine where the game is installed,
# or edit $GamePath below.
#
# Usage (from a PowerShell prompt):
#   cd "path\to\repo"
#   .\Microslop\deploy_windows.ps1
#
# Default source is Microslop\winmm.dll (ready-to-copy artifact in this repo).
# If that file is missing, the script falls back to Microslop\winmm_proxy\build\winmm.dll.
# You can always override with -DllPath.

[CmdletBinding()]
param (
    [string]$GamePath = "",
    [string]$DllPath  = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Resolve game path
# ---------------------------------------------------------------------------
$defaultPaths = @(
    "C:\Program Files (x86)\Steam\steamapps\common\Battlezone 98 Redux",
    "C:\Program Files\Steam\steamapps\common\Battlezone 98 Redux",
    "$env:PROGRAMFILES\Steam\steamapps\common\Battlezone 98 Redux"
)

if (-not $GamePath) {
    foreach ($p in $defaultPaths) {
        if (Test-Path $p) { $GamePath = $p; break }
    }
}

if (-not $GamePath -or -not (Test-Path $GamePath)) {
    Write-Host ""
    Write-Host "ERROR: Steam game folder not found." -ForegroundColor Red
    Write-Host "Pass the path explicitly:"
    Write-Host '  .\Microslop\deploy_windows.ps1 -GamePath "D:\Steam\steamapps\common\Battlezone 98 Redux"'
    exit 1
}

Write-Host "Game folder : $GamePath"

# ---------------------------------------------------------------------------
# Resolve DLL source
# ---------------------------------------------------------------------------
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoDll    = Join-Path $scriptDir "winmm.dll"
$buildDll   = Join-Path $scriptDir "winmm_proxy\build\winmm.dll"

if (-not $DllPath) {
    if (Test-Path $repoDll) {
        $DllPath = $repoDll
    } else {
        $DllPath = $buildDll
    }
}

if (-not (Test-Path $DllPath)) {
    Write-Host ""
    Write-Host "ERROR: winmm.dll not found at:" -ForegroundColor Red
    Write-Host "  $DllPath"
    Write-Host ""
    Write-Host "Build it first on Linux:"
    Write-Host "  cd Microslop/winmm_proxy && make"
    Write-Host "Then copy build/winmm.dll to Microslop/winmm.dll"
    exit 1
}

Write-Host "DLL source  : $DllPath"

# ---------------------------------------------------------------------------
# Back up any existing winmm.dll in the game folder
# ---------------------------------------------------------------------------
$dest = Join-Path $GamePath "winmm.dll"

if (Test-Path $dest) {
    $backup = Join-Path $GamePath "winmm.dll.bak"
    Write-Host "Backing up existing $dest -> $backup"
    Copy-Item -Force $dest $backup
}

# ---------------------------------------------------------------------------
# Deploy
# ---------------------------------------------------------------------------
Copy-Item -Force $DllPath $dest
Write-Host ""
Write-Host "Deployed: $dest" -ForegroundColor Green
Write-Host ""
Write-Host "--- NO Steam launch option changes needed ---"
Write-Host "The game will load winmm.dll from the game folder automatically."
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. Launch Battlezone 98 Redux via Steam"
Write-Host "  2. Start a multiplayer session"
Write-Host "  3. Quit the game"
Write-Host "  4. Run: .\Microslop\verify_windows.ps1 -GamePath `"$GamePath`""
