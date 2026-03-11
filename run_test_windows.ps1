param(
    [Parameter(Mandatory = $true)]
    [string]$GameRoot
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ExePath = Join-Path $GameRoot "battlezone98redux.exe"
$ManifestPath = Join-Path $ScriptDir "net_fix_manifest.send512k.recv2m.json"
$PatcherPath = Join-Path $ScriptDir "apply_binary_patch.py"
$VerifyPath = Join-Path $ScriptDir "verify_net_patch.ps1"

if (-not (Test-Path -LiteralPath $ExePath)) {
    Write-Host "Missing executable: $ExePath"
    exit 1
}

if (-not (Test-Path -LiteralPath $ManifestPath)) {
    Write-Host "Missing manifest: $ManifestPath"
    exit 1
}

if (-not (Test-Path -LiteralPath $PatcherPath)) {
    Write-Host "Missing patcher: $PatcherPath"
    exit 1
}

Write-Host "Windows on-disk patch test"
Write-Host "Game root: $GameRoot"

$pythonCmd = $null
if (Get-Command py -ErrorAction SilentlyContinue) {
    $pythonCmd = "py"
} elseif (Get-Command python -ErrorAction SilentlyContinue) {
    $pythonCmd = "python"
} else {
    Write-Host "Missing Python launcher (py/python). Install Python 3 first."
    exit 1
}

& $pythonCmd $PatcherPath $ExePath $ManifestPath
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Launch the game now, then host or join one multiplayer session."
Read-Host "Press Enter after entering MP once"

Push-Location $GameRoot
try {
    & $VerifyPath
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
