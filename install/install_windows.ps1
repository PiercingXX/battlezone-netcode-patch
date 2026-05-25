$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoSlug = "PiercingXX/battlezone-netcode-patch"
$steamAppId = "301650"
$gameExeName = "battlezone98redux.exe"
$defaultInstallDir = "Battlezone 98 Redux"
$ref = if ($env:BZNET_REF) { $env:BZNET_REF } else { "master" }
$gamePath = if ($args.Count -ge 1 -and $args[0]) { [string]$args[0] } elseif ($env:BZNET_GAME_PATH) { $env:BZNET_GAME_PATH } else { "" }
$dllUrl = if ($env:BZNET_DLL_URL) { $env:BZNET_DLL_URL } else { "https://raw.githubusercontent.com/$repoSlug/$ref/prebuilt/windows/winmm.dll" }
$expectedHash = if ($env:BZNET_WINMM_SHA256) { $env:BZNET_WINMM_SHA256.ToLowerInvariant() } else { "29f9555c8ef6fb1e7600c4e953b3637d6489b54db324041957e068717a367acb" }

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

function Find-GamePath {
    Find-InstalledGamePath
}

function Assert-Hash {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedSha256
    )

    $actual = (Get-FileHash -Algorithm SHA256 -Path $FilePath).Hash.ToLowerInvariant()
    if ($actual -ne $ExpectedSha256) {
        throw "Downloaded winmm.dll hash mismatch. Expected $ExpectedSha256 but got $actual"
    }
}

if (-not $gamePath) {
    $gamePath = Find-GamePath
}

if (-not $gamePath) {
    throw "Could not find Battlezone 98 Redux automatically. Set BZNET_GAME_PATH and run again."
}

$exePath = Join-Path $gamePath $gameExeName
if (-not (Test-Path $exePath)) {
    throw "Game executable not found in: $gamePath"
}

$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
$downloadedDll = Join-Path $tempRoot "winmm.dll"

try {
    New-Item -ItemType Directory -Path $tempRoot | Out-Null
    Write-Host "Downloading known-good winmm.dll from $dllUrl"
    Invoke-WebRequest -Uri $dllUrl -OutFile $downloadedDll
    Assert-Hash -FilePath $downloadedDll -ExpectedSha256 $expectedHash

    $destPath = Join-Path $gamePath "winmm.dll"
    if (Test-Path $destPath) {
        Write-Host "Deleting existing winmm.dll before install"
        Remove-Item -Force $destPath
    }

    Write-Host "Installing patch to $destPath"
    Copy-Item -Force $downloadedDll $destPath
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $gamePath "winmm_proxy.log")

    Write-Host ""
    Write-Host "Install complete." -ForegroundColor Green
    Write-Host "Installed to: $destPath"
    Write-Host "No Steam launch option changes are needed on Windows."
}
finally {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $tempRoot
}