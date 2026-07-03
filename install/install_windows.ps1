$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoSlug = "PiercingXX/battlezone-netcode-patch"
$steamAppId = "301650"
$gameExeName = "battlezone98redux.exe"
$defaultInstallDir = "Battlezone 98 Redux"
$ref = if ($env:BZNET_REF) { $env:BZNET_REF } else { "master" }
$gamePath = if ($args.Count -ge 1 -and $args[0]) { [string]$args[0] } elseif ($env:BZNET_GAME_PATH) { $env:BZNET_GAME_PATH } else { "" }
$dllUrl = if ($env:BZNET_DLL_URL) { $env:BZNET_DLL_URL } else { "https://raw.githubusercontent.com/$repoSlug/$ref/prebuilt/windows/winmm.dll" }
$netIniUrl = if ($env:BZNET_NETINI_URL) { $env:BZNET_NETINI_URL } else { "https://raw.githubusercontent.com/$repoSlug/$ref/net-ini/net.ini" }
$expectedHash = if ($env:BZNET_WINMM_SHA256) { $env:BZNET_WINMM_SHA256.ToLowerInvariant() } else { "ba73fda9752116ef14834a49da1d62ce3bb40485df5d8a6e074ad8f2860bd1a1" }

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

    # net.ini send-governor tuning.  The game only loads net.ini through the
    # mod system - a copy in the game folder root is silently ignored - so it
    # is installed as a local packaged mod.  Best effort - never fail the DLL
    # install over it.
    try {
        $downloadedNetIni = Join-Path $tempRoot "net.ini"
        Invoke-WebRequest -Uri $netIniUrl -OutFile $downloadedNetIni
        $netIniModDir = Join-Path $gamePath "packaged_mods\9990001"
        if (-not (Test-Path $netIniModDir)) {
            New-Item -ItemType Directory -Path $netIniModDir | Out-Null
        }
        $netIniDest = Join-Path $netIniModDir "net.ini"
        Copy-Item -Force $downloadedNetIni $netIniDest
        Write-Host "Installed net.ini tuning mod to $netIniDest"

        # Workshop mods ship their own net.ini and win over the local file, and
        # DISABLING the mod in the in-game manager is not enough - it still loads.
        $steamApps = Split-Path (Split-Path $gamePath)
        $workshopNetIni = Get-ChildItem -Path (Join-Path $steamApps "workshop\content\$steamAppId") -Filter "net.ini" -Recurse -Depth 1 -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($workshopNetIni) {
            Write-Warning "A Workshop mod also provides net.ini and will override the local file:"
            Write-Warning "  $($workshopNetIni.FullName)"
            Write-Warning "Unsubscribe from that mod (disabling it in-game is NOT enough) if you plan to host."
        }
    }
    catch {
        Write-Warning "Could not install host-side net.ini tuning: $_"
    }

    Write-Host ""
    Write-Host "Install complete." -ForegroundColor Green
    Write-Host "Installed to: $destPath"
    Write-Host "No Steam launch option changes are needed on Windows."
    Write-Host ""
    Write-Host "One more step for the current test phase - enable outbound packet duplication:" -ForegroundColor Yellow
    Write-Host "  1. Run:  setx BZ_SEND_DUP 1"
    Write-Host "  2. Fully restart Steam (so the game inherits the variable)"
    Write-Host "It recovers packets the network genuinely loses and also helps unpatched opponents."
    Write-Host "Confirm it took: winmm_proxy.log should show 'send_dup=enabled' after the next launch."
}
finally {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $tempRoot
}