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
# Default source is Microslop\winmm.dll (known-good artifact in this repo).
# You can always override with -DllPath.

[CmdletBinding()]
param (
    [string]$GamePath = "",
    [string]$DllPath  = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$steamAppId = "301650"
$gameExeName = "battlezone98redux.exe"
$defaultInstallDir = "Battlezone 98 Redux"

function Get-SteamRoots {
    $roots = New-Object System.Collections.Generic.List[string]

    foreach ($location in @(
        @{ Path = "HKCU:\Software\Valve\Steam"; Names = @("SteamPath", "Path") },
        @{ Path = "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam"; Names = @("InstallPath") },
        @{ Path = "HKLM:\SOFTWARE\Valve\Steam"; Names = @("InstallPath") }
    )) {
        try {
            $item = Get-ItemProperty -Path $location.Path -ErrorAction Stop
            foreach ($name in $location.Names) {
                $value = [string]$item.$name
                if ($value) {
                    $roots.Add($value)
                }
            }
        }
        catch {
        }
    }

    foreach ($fallback in @(
        (Join-Path ${env:ProgramFiles(x86)} "Steam"),
        (Join-Path $env:PROGRAMFILES "Steam")
    )) {
        if ($fallback) {
            $roots.Add($fallback)
        }
    }

    $roots | Where-Object { $_ } | Select-Object -Unique
}

function Get-SteamLibraryRoots {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SteamRoot
    )

    $libraryRoots = New-Object System.Collections.Generic.List[string]
    $libraryRoots.Add($SteamRoot)

    $libraryVdf = Join-Path $SteamRoot "steamapps\libraryfolders.vdf"
    if (Test-Path $libraryVdf) {
        foreach ($line in Get-Content -Path $libraryVdf) {
            $match = [regex]::Match($line, '"path"\s+"([^"]+)"')
            if (-not $match.Success) {
                $match = [regex]::Match($line, '^\s*"\d+"\s+"([^"]+)"')
            }

            if ($match.Success) {
                $libraryRoots.Add($match.Groups[1].Value.Replace('\\', '\'))
            }
        }
    }

    $libraryRoots | Where-Object { $_ } | Select-Object -Unique
}

function Find-InstalledGamePath {
    foreach ($steamRoot in Get-SteamRoots) {
        foreach ($libraryRoot in Get-SteamLibraryRoots -SteamRoot $steamRoot) {
            $steamApps = Join-Path $libraryRoot "steamapps"
            $manifest = Join-Path $steamApps "appmanifest_$steamAppId.acf"
            if (Test-Path $manifest) {
                $installDir = $defaultInstallDir
                foreach ($line in Get-Content -Path $manifest) {
                    $match = [regex]::Match($line, '"installdir"\s+"([^"]+)"')
                    if ($match.Success) {
                        $installDir = $match.Groups[1].Value
                        break
                    }
                }

                $candidate = Join-Path $steamApps (Join-Path "common" $installDir)
                if (Test-Path (Join-Path $candidate $gameExeName)) {
                    return $candidate
                }
            }

            $fallbackCandidate = Join-Path $steamApps (Join-Path "common" $defaultInstallDir)
            if (Test-Path (Join-Path $fallbackCandidate $gameExeName)) {
                return $fallbackCandidate
            }
        }
    }

    return ""
}

# ---------------------------------------------------------------------------
# Resolve game path
# ---------------------------------------------------------------------------
if (-not $GamePath) {
    $GamePath = Find-InstalledGamePath
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

if (-not $DllPath) {
    $DllPath = $repoDll
}

if (-not (Test-Path $DllPath)) {
    Write-Host ""
    Write-Host "ERROR: winmm.dll not found at:" -ForegroundColor Red
    Write-Host "  $DllPath"
    Write-Host ""
    Write-Host "Use install\\install_windows.ps1 to fetch the known-good prebuilt winmm.dll."
    Write-Host "Building winmm.dll locally is not the recommended Windows path."
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
