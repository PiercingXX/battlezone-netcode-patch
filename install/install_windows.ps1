$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoSlug = "PiercingXX/battlezone-netcode-testing"
$ref = if ($env:BZNET_REF) { $env:BZNET_REF } else { "main" }
$gamePath = if ($args.Count -ge 1 -and $args[0]) { [string]$args[0] } elseif ($env:BZNET_GAME_PATH) { $env:BZNET_GAME_PATH } else { "" }
$dllUrl = if ($env:BZNET_DLL_URL) { $env:BZNET_DLL_URL } else { "https://github.com/$repoSlug/raw/$ref/prebuilt/windows/winmm.dll" }
$expectedHash = if ($env:BZNET_WINMM_SHA256) { $env:BZNET_WINMM_SHA256.ToLowerInvariant() } else { "29f9555c8ef6fb1e7600c4e953b3637d6489b54db324041957e068717a367acb" }

function Find-GamePath {
    $candidates = @(
        "C:\Program Files (x86)\Steam\steamapps\common\Battlezone 98 Redux",
        "C:\Program Files\Steam\steamapps\common\Battlezone 98 Redux",
        (Join-Path $env:PROGRAMFILES "Steam\steamapps\common\Battlezone 98 Redux")
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate "battlezone98redux.exe"))) {
            return $candidate
        }
    }

    return ""
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

$exePath = Join-Path $gamePath "battlezone98redux.exe"
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