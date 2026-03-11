param(
    [Parameter(Mandatory = $true)]
    [string]$GameRoot
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ExePath = Join-Path $GameRoot "battlezone98redux.exe"
$RuntimePatcherPath = Join-Path $ScriptDir "runtime_patch_windows.ps1"
$VerifyPath = Join-Path $ScriptDir "verify_net_patch.ps1"

if (-not (Test-Path -LiteralPath $ExePath)) {
    Write-Host "Missing executable: $ExePath"
    exit 1
}

if (-not (Test-Path -LiteralPath $RuntimePatcherPath)) {
    Write-Host "Missing runtime patcher: $RuntimePatcherPath"
    exit 1
}

if (-not (Test-Path -LiteralPath $VerifyPath)) {
    Write-Host "Missing verifier: $VerifyPath"
    exit 1
}

Write-Host "Windows runtime patch test"
Write-Host "Game root: $GameRoot"

Write-Host "Launch the game now and wait at in-game main menu."
Read-Host "Press Enter when game is at main menu"

& $RuntimePatcherPath
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Write-Host "Now host or join one multiplayer session."
Read-Host "Press Enter after entering MP once"

Push-Location $GameRoot
try {
    $env:VERIFY_RUNTIME_ONLY = "1"
    & $VerifyPath
    exit $LASTEXITCODE
}
finally {
    Remove-Item Env:VERIFY_RUNTIME_ONLY -ErrorAction SilentlyContinue
    Pop-Location
}
