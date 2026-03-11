$ErrorActionPreference = "Stop"

$LogFile = "BZLogger.txt"
$ExeFile = "battlezone98redux.exe"
$ExpectedRecv = "2097152"
$ExpectedSend = "524288"

if (-not (Test-Path -LiteralPath $LogFile)) {
    Write-Host "Missing $LogFile"
    exit 1
}

if (-not (Test-Path -LiteralPath $ExeFile)) {
    Write-Host "Missing $ExeFile"
    exit 1
}

$lines = Get-Content -LiteralPath $LogFile
$startMatches = Select-String -InputObject $lines -Pattern "Starting BattleZone 98 Redux"
if (-not $startMatches) {
    Write-Host "No startup marker found in $LogFile"
    exit 1
}

$startLine = $startMatches[-1].LineNumber
$session = $lines[($startLine - 1)..($lines.Length - 1)]

Write-Host "Latest startup marker:"
$startMatches[-1].ToString() | Write-Host

Write-Host "Latest loaded net.ini source:"
$srcMatches = Select-String -InputObject $session -Pattern "MOD FOUND net.ini"
if ($srcMatches) {
    $srcMatches[-1].ToString() | Write-Host
}

Write-Host "Latest socket buffer line:"
$bufMatches = Select-String -InputObject $session -Pattern "BZRNet P2P Socket Opened With"
if ($bufMatches) {
    $bufMatches[-1].ToString() | Write-Host
}

$expectedLine = "BZRNet P2P Socket Opened With $ExpectedRecv received buffer, $ExpectedSend send buffer"
$bufOk = $false
if ($bufMatches) {
    foreach ($m in $bufMatches) {
        if ($m.Line -like "*$expectedLine*") {
            $bufOk = $true
            break
        }
    }
}

$exeBytes = [System.IO.File]::ReadAllBytes($ExeFile)
$sendBytes = $exeBytes[0x52d96a..0x52d96d]
$recvBytes = $exeBytes[0x52db5e..0x52db61]
$sendHex = (($sendBytes | ForEach-Object { $_.ToString("x2") }) -join "")
$recvHex = (($recvBytes | ForEach-Object { $_.ToString("x2") }) -join "")
$exeOk = ($sendHex -eq "00000800" -and $recvHex -eq "00002000")

Write-Host "Executable patch bytes:"
Write-Host "- send @0x52d96a: $sendHex (expected 00000800)"
Write-Host "- recv @0x52db5e: $recvHex (expected 00002000)"

if ($bufOk) {
    Write-Host "VERIFY RESULT: PASS"
    exit 0
}

Write-Host "VERIFY RESULT: FAIL"
Write-Host "- socket buffer line mismatch"
if ($exeOk) {
    Write-Host "- executable is patched; launch the game once after patching to generate a fresh log session"
} else {
    Write-Host "- executable patch bytes not detected"
}
exit 2
