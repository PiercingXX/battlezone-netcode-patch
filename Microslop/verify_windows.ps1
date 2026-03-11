# verify_windows.ps1
# Battlezone 98 Redux - Windows netcode patch
#
# Checks that the winmm.dll proxy deployed correctly and that the socket
# buffer hook fired.  Run this after the game has been launched and a
# multiplayer session has been started.
#
# Usage:
#   .\Microslop\verify_windows.ps1
#   .\Microslop\verify_windows.ps1 -GamePath "D:\Steam\steamapps\common\Battlezone 98 Redux"

[CmdletBinding()]
param (
    [string]$GamePath = ""
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
    Write-Host "ERROR: game folder not found. Pass -GamePath explicitly." -ForegroundColor Red
    exit 1
}

$pass   = $true
$issues = @()

Write-Host ""
Write-Host "=== Battlezone 98 Redux - Windows netcode patch verifier ==="
Write-Host "Game folder : $GamePath"
Write-Host ""

# ---------------------------------------------------------------------------
# Check 1: winmm.dll present in game folder
# ---------------------------------------------------------------------------
$dllPath = Join-Path $GamePath "winmm.dll"
if (Test-Path $dllPath) {
    Write-Host "[PASS] winmm.dll present in game folder" -ForegroundColor Green
} else {
    Write-Host "[FAIL] winmm.dll NOT found in game folder" -ForegroundColor Red
    $issues += "winmm.dll is missing - run deploy_windows.ps1 first"
    $pass = $false
}

# ---------------------------------------------------------------------------
# Check 2: log file exists
# ---------------------------------------------------------------------------
$logPath = Join-Path $GamePath "winmm_proxy.log"
if (-not (Test-Path $logPath)) {
    Write-Host "[FAIL] winmm_proxy.log not found" -ForegroundColor Red
    $issues += "Log file missing - the proxy DLL did not run. Check winmm.dll is in the game folder and the game was launched."
    $pass = $false
} else {
    Write-Host "[PASS] winmm_proxy.log found" -ForegroundColor Green

    $lines = Get-Content $logPath
    $startIdx = 0
    for ($i = $lines.Count - 1; $i -ge 0; $i--) {
        if ($lines[$i] -match "=== winmm_proxy.dll loaded ===") {
            $startIdx = $i
            break
        }
    }
    $sessionLines = if ($lines.Count -gt 0) { $lines[$startIdx..($lines.Count - 1)] } else { @() }
    $sessionLog = $sessionLines -join "`n"

    # Check 3: real winmm.dll loaded
    if ($sessionLog -match "real winmm loaded") {
        Write-Host "[PASS] Real winmm.dll was loaded" -ForegroundColor Green
    } else {
        Write-Host "[WARN] Could not confirm real winmm.dll was loaded" -ForegroundColor Yellow
    }

    # Check 4: IAT patch applied
    if ($sessionLog -match "WSASocketW IAT patched OK") {
        Write-Host "[PASS] WSASocketW IAT hook installed" -ForegroundColor Green
    } else {
        Write-Host "[FAIL] WSASocketW IAT patch not confirmed in log" -ForegroundColor Red
        $issues += "Socket hook did not install - check the log for errors"
        $pass = $false
    }

    # Check 5: readback confirms target values
    # Looking for: effective readback SO_SNDBUF=524288 and SO_RCVBUF=2097152
    if ($sessionLog -match "effective readback SO_SNDBUF=524288" -and
        $sessionLog -match "effective readback SO_RCVBUF=2097152") {
        Write-Host "[PASS] SO_SNDBUF=524288 confirmed via readback" -ForegroundColor Green
        Write-Host "[PASS] SO_RCVBUF=2097152 confirmed via readback" -ForegroundColor Green
    } elseif ($sessionLog -match "WSASocketW hook:") {
        Write-Host "[WARN] Socket hook fired but readback values unexpected" -ForegroundColor Yellow
        Write-Host "       Check $logPath for details"

        # Extract and display the hook line(s)
        $hookLines = $sessionLines | Where-Object { $_ -match "WSASocketW hook:" }
        foreach ($l in $hookLines) { Write-Host "       $l" }
    } else {
        Write-Host "[FAIL] Socket hook never fired - game may not have entered multiplayer" -ForegroundColor Red
        $issues += "No WSASocketW hook log entries - launch the game and join/host a multiplayer game, then re-run this script"
        $pass = $false
    }

    # Print last 20 lines of log for reference
    Write-Host ""
    Write-Host "--- Last 20 lines of winmm_proxy.log ---"
    $tail  = $sessionLines | Select-Object -Last 20
    foreach ($l in $tail) { Write-Host "  $l" }
    Write-Host "--- end of log ---"
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ""
if ($pass) {
    Write-Host "=== RESULT: PASS - netcode patch is active ===" -ForegroundColor Green
} else {
    Write-Host "=== RESULT: FAIL ===" -ForegroundColor Red
    foreach ($i in $issues) {
        Write-Host "  * $i" -ForegroundColor Yellow
    }
}
Write-Host ""
