$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoSlug = "PiercingXX/battlezone-netcode-testing"
$ref = if ($env:BZNET_REF) { $env:BZNET_REF } else { "main" }
$gamePath = if ($args.Count -ge 1 -and $args[0]) { [string]$args[0] } elseif ($env:BZNET_GAME_PATH) { $env:BZNET_GAME_PATH } else { "" }
$archiveUrl = if ($env:BZNET_ARCHIVE_URL) { $env:BZNET_ARCHIVE_URL } else { "https://github.com/$repoSlug/archive/$ref.zip" }
$assumeYes = $env:BZNET_ASSUME_YES -eq "1"

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

function Confirm-InstallStep {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Explanation
    )

    Write-Host $Explanation

    if ($assumeYes) {
        return
    }

    $answer = Read-Host "Proceed? [Y/n]"
    if ($answer -and $answer -notmatch '^(?i:y|yes)$') {
        throw "Dependency installation cancelled."
    }
}

function Get-MsysRoot {
    $candidates = @(
        "C:\msys64",
        (Join-Path ${env:ProgramFiles} "MSYS2"),
        (Join-Path ${env:LOCALAPPDATA} "Programs\MSYS2")
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate "usr\bin\bash.exe"))) {
            return $candidate
        }
    }

    return $null
}

function Ensure-Msys2 {
    $msysRoot = Get-MsysRoot
    if ($msysRoot) {
        return $msysRoot
    }

    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "MSYS2 is not installed and winget is unavailable. Install MSYS2 manually, then run the installer again."
    }

    Confirm-InstallStep -Explanation "This installer needs MSYS2 so it can build the 32-bit winmm.dll locally from source. MSYS2 provides the bash shell, pacman package manager, and GNU build tools used only for compiling the patch."
    winget install --id MSYS2.MSYS2 --accept-package-agreements --accept-source-agreements --disable-interactivity

    $msysRoot = Get-MsysRoot
    if (-not $msysRoot) {
        throw "MSYS2 installation completed, but the install path could not be detected."
    }

    return $msysRoot
}

function Ensure-BuildDependencies {
    param(
        [Parameter(Mandatory = $true)]
        [string]$MsysRoot
    )

    $bashExe = Join-Path $MsysRoot "usr\bin\bash.exe"
    $checkCommand = 'export PATH=/mingw32/bin:/usr/bin:$PATH; command -v i686-w64-mingw32-g++ >/dev/null 2>&1 && command -v make >/dev/null 2>&1'
    $null = & $bashExe -lc $checkCommand 2>$null
    if ($LASTEXITCODE -eq 0) {
        return
    }

    Confirm-InstallStep -Explanation "This installer needs the MSYS2 MinGW 32-bit compiler and make so it can compile winmm.dll locally from source on this machine. It will install packages: mingw-w64-i686-gcc and make."
    & $bashExe -lc 'pacman -Sy --noconfirm --needed mingw-w64-i686-gcc make'
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install MSYS2 build dependencies."
    }
}

function Expand-SourceArchive {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ArchivePath,

        [Parameter(Mandatory = $true)]
        [string]$Destination
    )

    Expand-Archive -Path $ArchivePath -DestinationPath $Destination -Force

    $root = Get-ChildItem -Path $Destination -Directory | Select-Object -First 1
    if (-not $root) {
        throw "Failed to locate extracted source directory."
    }

    return $root.FullName
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
$archivePath = Join-Path $tempRoot "source.zip"
$extractPath = Join-Path $tempRoot "src"

try {
    New-Item -ItemType Directory -Path $tempRoot | Out-Null
    $msysRoot = Ensure-Msys2
    Ensure-BuildDependencies -MsysRoot $msysRoot

    Write-Host "Downloading source archive from $archiveUrl"
    Invoke-WebRequest -Uri $archiveUrl -OutFile $archivePath

    New-Item -ItemType Directory -Path $extractPath | Out-Null
    $sourceRoot = Expand-SourceArchive -ArchivePath $archivePath -Destination $extractPath

    $bashExe = Join-Path $msysRoot "usr\bin\bash.exe"
    $msysSourceRoot = $sourceRoot -replace '\\', '/'

    Write-Host "Building winmm.dll from source"
    & $bashExe -lc "export PATH=/mingw32/bin:/usr/bin:`$PATH; cd '$msysSourceRoot/Microslop/winmm_proxy' && make clean && make"
    if ($LASTEXITCODE -ne 0) {
        throw "Source build failed."
    }

    $builtDll = Join-Path $sourceRoot "Microslop\winmm_proxy\build\winmm.dll"
    if (-not (Test-Path $builtDll)) {
        throw "Build completed, but winmm.dll was not produced."
    }

    $destPath = Join-Path $gamePath "winmm.dll"
    if (Test-Path $destPath) {
        Write-Host "Deleting existing winmm.dll before install"
        Remove-Item -Force $destPath
    }

    Write-Host "Installing patch to $destPath"
    Copy-Item -Force $builtDll $destPath
    Remove-Item -Force -ErrorAction SilentlyContinue (Join-Path $gamePath "winmm_proxy.log")

    Write-Host ""
    Write-Host "Install complete." -ForegroundColor Green
    Write-Host "Installed to: $destPath"
    Write-Host "No Steam launch option changes are needed on Windows."
}
finally {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $tempRoot
}