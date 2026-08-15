# Remove the Battlezone netcode patch from a Windows install.
#
# There was no uninstaller for either platform until V4.9. Everything the
# installer writes is removed here, and nothing else:
#
#   <game>\winmm.dll                            the proxy
#   <game>\packaged_mods\9990001\net.ini        the tuning mod
#
# Logs and captures the game or the proxy produced are left alone: they are
# research data, and deleting someone's session logs to uninstall a DLL would
# be a rude surprise. -PurgeLogs removes them if you actually want that.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File install\uninstall_windows.ps1
#   ... -GamePath "D:\Steam\steamapps\common\Battlezone 98 Redux"
#   ... -PurgeLogs -Yes

[CmdletBinding()]
param(
    [string]$GamePath = "",
    [switch]$PurgeLogs,
    [switch]$Yes
)

$ErrorActionPreference = "Stop"

function Get-DetectedGamePath {
    $candidates = @()
    foreach ($drive in (Get-PSDrive -PSProvider FileSystem | ForEach-Object { $_.Root })) {
        $candidates += (Join-Path $drive "Program Files (x86)\Steam\steamapps\common\Battlezone 98 Redux")
        $candidates += (Join-Path $drive "Steam\steamapps\common\Battlezone 98 Redux")
        $candidates += (Join-Path $drive "SteamLibrary\steamapps\common\Battlezone 98 Redux")
    }
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c "battlezone98redux.exe")) { return $c }
    }
    return ""
}

function Confirm-Action {
    param([string]$Message)
    if ($Yes) { return $true }
    $answer = Read-Host "$Message [y/N]"
    return ($answer -eq "y" -or $answer -eq "Y")
}

if (-not $GamePath) { $GamePath = Get-DetectedGamePath }
if (-not $GamePath -or -not (Test-Path (Join-Path $GamePath "battlezone98redux.exe"))) {
    Write-Host "Could not find the game. Pass -GamePath." -ForegroundColor Red
    exit 1
}

Write-Host "Game folder: $GamePath"
Write-Host ""

$removed = 0
function Remove-PatchFile {
    param([string]$Path)
    if (Test-Path $Path) {
        Remove-Item -Force $Path
        Write-Host "  removed $Path"
        $script:removed++
    } else {
        Write-Host "  (absent) $Path"
    }
}

Write-Host "Removing patch files:"
# Other mods ship proxy DLLs under the same name. Only delete a winmm.dll that
# carries this patch's own marker string; anything else is not ours to remove.
$dllPath = Join-Path $GamePath "winmm.dll"
$isOurs = $true
if (Test-Path $dllPath) {
    $raw = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($dllPath))
    $isOurs = $raw.Contains("BZ_GOV_START")
}
if ($isOurs) {
    Remove-PatchFile $dllPath
} else {
    Write-Host "  KEEPING $dllPath : it does not look like this patch's proxy"
    Write-Host "  (no BZ_GOV_START marker) - another mod's DLL? Remove it by"
    Write-Host "  hand if you are certain."
}
Remove-PatchFile (Join-Path $GamePath "packaged_mods\9990001\net.ini")

# Only if the installer's own directory is now empty; never recursive.
$modDir = Join-Path $GamePath "packaged_mods\9990001"
if ((Test-Path $modDir) -and -not (Get-ChildItem -Force $modDir)) {
    Remove-Item -Force $modDir
    Write-Host "  removed empty $modDir"
}

Write-Host ""
if ($PurgeLogs) {
    if (Confirm-Action "Delete session logs and captures? They are research data and cannot be recovered.") {
        Write-Host "Removing logs and captures (-PurgeLogs):"
        foreach ($f in @("winmm_proxy.log", "dsound_proxy.log", "bz_buffer_log.bin",
                         "bz_buffer_log.meta.txt", "BZLogger.txt")) {
            Remove-PatchFile (Join-Path $GamePath $f)
        }
    } else {
        Write-Host "Logs kept."
    }
} else {
    Write-Host "Leaving logs and captures in place (pass -PurgeLogs to delete them)."
}

Write-Host ""
if ($removed -eq 0) {
    Write-Host "Nothing to remove - the patch does not appear to be installed here."
} else {
    Write-Host "Uninstall complete ($removed item(s) removed)." -ForegroundColor Green
}

Write-Host ""
Write-Host "The Windows install needs no launch options, so there is nothing to"
Write-Host "clear in Steam - unless you added the upload wrapper from upload\."
Write-Host "If you did, remove that line from Launch Options as well."
