[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Start", "Stop")]
    [string]$Action,
    [string]$GamePath = "",
    [int]$PayloadBytes = 32,
    [int]$RingRecords = 65536,
    [string]$PeerFilter = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$steamAppId = "301650"
$steamGameExeName = "battlezone98redux.exe"
$defaultInstallDir = "Battlezone 98 Redux"

$repoRoot = Split-Path -Parent $PSScriptRoot
$stateRoot = Join-Path $repoRoot "test_bundles\buffer_log_state"
$currentFile = Join-Path $stateRoot "windows_current_session.txt"
New-Item -ItemType Directory -Path $stateRoot -Force | Out-Null

function Get-ResolvedGamePath {
    param([string]$Candidate)

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
        param([string]$SteamRoot)

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

    if ($Candidate -and (Test-Path (Join-Path $Candidate $steamGameExeName))) {
        return $Candidate
    }

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

                $candidatePath = Join-Path $steamApps (Join-Path "common" $installDir)
                if (Test-Path (Join-Path $candidatePath $steamGameExeName)) {
                    return $candidatePath
                }
            }

            $fallbackPath = Join-Path $steamApps (Join-Path "common" $defaultInstallDir)
            if (Test-Path (Join-Path $fallbackPath $steamGameExeName)) {
                return $fallbackPath
            }
        }
    }

    return ""
}

function Collect-File {
    param(
        [string]$Source,
        [string]$DestinationDir,
        [string]$StatusFile
    )

    $base = Split-Path -Leaf $Source
    if (Test-Path $Source) {
        Copy-Item -Force $Source (Join-Path $DestinationDir $base)
        $bytes = (Get-Item $Source).Length
        Add-Content -Path $StatusFile -Value "found $base bytes=$bytes"
    } else {
        Add-Content -Path $StatusFile -Value "missing $base"
    }
}

function Write-LaunchOptions {
    param(
        [string]$OutFile,
        [int]$PayloadBytesValue,
        [int]$RingRecordsValue,
        [string]$PeerFilterValue
    )

    $line = "set BZ_BUFFER_LOG=1 && set BZ_BUFFER_LOG_BYTES=$PayloadBytesValue && set BZ_BUFFER_LOG_RING=$RingRecordsValue"
    if ($PeerFilterValue) {
        $line += " && set BZ_BUFFER_LOG_PEER=$PeerFilterValue"
    }
    $line += " && %command%"

    @(
        "Windows Steam launch options for buffer logging:",
        "",
        $line
    ) | Out-File -FilePath $OutFile -Encoding utf8
}

function Start-Session {
    $resolvedGamePath = Get-ResolvedGamePath -Candidate $GamePath
    if (-not $resolvedGamePath) {
        Write-Host "ERROR: game folder not found. Pass -GamePath explicitly." -ForegroundColor Red
        exit 1
    }

    if (Test-Path $currentFile) {
        $existing = (Get-Content $currentFile -Raw).Trim()
        if ($existing -and (Test-Path $existing)) {
            Write-Host "ERROR: buffer logging session already active: $existing" -ForegroundColor Red
            exit 1
        }
    }

    $utcStamp = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
    $sessionDir = Join-Path $repoRoot "test_bundles\buffer_windows_$env:COMPUTERNAME`_$utcStamp"
    New-Item -ItemType Directory -Path $sessionDir -Force | Out-Null

    $sessionDir | Out-File -FilePath $currentFile -Encoding utf8
    $resolvedGamePath | Out-File -FilePath (Join-Path $sessionDir "game_path.txt") -Encoding utf8
    $PayloadBytes | Out-File -FilePath (Join-Path $sessionDir "payload_bytes.txt") -Encoding utf8
    $RingRecords | Out-File -FilePath (Join-Path $sessionDir "ring_records.txt") -Encoding utf8
    $PeerFilter | Out-File -FilePath (Join-Path $sessionDir "peer_filter.txt") -Encoding utf8
    (Get-Date).ToUniversalTime().ToString("o") | Out-File -FilePath (Join-Path $sessionDir "start_utc.txt") -Encoding utf8

    Write-LaunchOptions -OutFile (Join-Path $sessionDir "launch_options.txt") -PayloadBytesValue $PayloadBytes -RingRecordsValue $RingRecords -PeerFilterValue $PeerFilter

    @(
        "1. Copy the Steam launch options from launch_options.txt.",
        "2. Start Battlezone 98 Redux.",
        "3. Reproduce the packet-order issue.",
        "4. Run .\buffer-logging\buffer_logger_windows.ps1 -Action Stop",
        "",
        "Expected lightweight outputs from the game folder:",
        "- winmm_proxy.log",
        "- bz_buffer_log.bin",
        "- bz_buffer_log.meta.txt",
        "- BZLogger.txt"
    ) | Out-File -FilePath (Join-Path $sessionDir "README_NEXT_STEPS.txt") -Encoding utf8

    Write-Host "Buffer logging session started."
    Write-Host "Session dir: $sessionDir"
    Write-Host "Launch options saved to: $(Join-Path $sessionDir 'launch_options.txt')"
}

# Did the capture actually run with the settings we asked for?
#
# The one successful capture of 2026-07-26 asked for BZ_BUFFER_LOG_RING=500000
# and ran with 65,536 -- the default -- discarding 48% of its events including
# the entire match start. Nothing said so, and it took reading two files side
# by side days later to notice. The proxy now records what it was asked for in
# the meta file; this compares that against the request and refuses to be quiet
# about a mismatch, while the tester can still re-run it.
function Test-Capture {
    param([string]$SessionDir)

    $meta = Join-Path $SessionDir "bz_buffer_log.meta.txt"
    $report = Join-Path $SessionDir "capture_verify.txt"
    $lines = @()
    $ok = $true

    if (-not (Test-Path $meta) -or (Get-Item $meta).Length -eq 0) {
        $lines += "meta=MISSING"
        $lines += "The game did not flush the ring. An unclean exit never writes it."
        $ok = $false
    } else {
        $kv = @{}
        foreach ($line in (Get-Content $meta)) {
            if ($line -match '^\s*([^=]+)=(.*)$') { $kv[$Matches[1].Trim()] = $Matches[2].Trim() }
        }
        $wantRing  = (Get-Content (Join-Path $SessionDir "ring_records.txt") -Raw).Trim()
        $wantBytes = (Get-Content (Join-Path $SessionDir "payload_bytes.txt") -Raw).Trim()
        $gotRing   = $kv["ring_records"]
        $gotBytes  = $kv["payload_bytes"]

        $lines += "requested_ring=$wantRing effective_ring=$gotRing"
        $lines += "requested_payload=$wantBytes effective_payload=$gotBytes"
        if ($kv.ContainsKey("ring_env")) { $lines += "ring_env=$($kv['ring_env'])" }

        if ($wantRing -and $gotRing -and $wantRing -ne $gotRing) {
            $lines += "MISMATCH: ring"; $ok = $false
        }
        if ($wantBytes -and $gotBytes -and $wantBytes -ne $gotBytes) {
            $lines += "MISMATCH: payload"; $ok = $false
        }
        $seen  = 0; $wrote = 0
        [void][int]::TryParse($kv["total_events_seen"], [ref]$seen)
        [void][int]::TryParse($kv["records_written"], [ref]$wrote)
        if ($seen -gt 0 -and $wrote -gt 0 -and $seen -gt $wrote) {
            $pct = [int](($seen - $wrote) * 100 / $seen)
            $lines += "ring wrapped: $seen events seen, last $wrote kept ($pct% discarded)"
            $ok = $false
        }
    }
    $lines += "ok=$ok"
    $lines | Out-File -FilePath $report -Encoding utf8

    Write-Host ""
    if ($ok) {
        Write-Host "  capture verified: ran with the settings you asked for, ring did not wrap"
        return
    }
    Write-Host "  ####################################################################" -ForegroundColor Red
    Write-Host "  #  CAPTURE IS NOT CLEAN - read it before you rely on it            #" -ForegroundColor Red
    Write-Host "  ####################################################################" -ForegroundColor Red
    $lines | ForEach-Object { Write-Host "    $_" }
    Write-Host ""
    Write-Host "    A ring that wrapped, or settings that did not take, means the"
    Write-Host "    capture is missing events - most likely the earliest ones, which"
    Write-Host "    is where the match start lives."
    Write-Host "    If ring_env says NOT SET, the game was launched before the launch"
    Write-Host "    options were pasted. Close the game, paste them, run this again."
    Write-Host ""
}

function Stop-Session {
    if (-not (Test-Path $currentFile)) {
        Write-Host "ERROR: no active Windows buffer logging session found." -ForegroundColor Red
        exit 1
    }

    $sessionDir = (Get-Content $currentFile -Raw).Trim()
    if (-not $sessionDir -or -not (Test-Path $sessionDir)) {
        Write-Host "ERROR: session directory missing: $sessionDir" -ForegroundColor Red
        Remove-Item -Force $currentFile -ErrorAction SilentlyContinue
        exit 1
    }

    $gamePathResolved = (Get-Content (Join-Path $sessionDir "game_path.txt") -Raw).Trim()
    $statusFile = Join-Path $sessionDir "collection_status.txt"
    Set-Content -Path $statusFile -Value ""

    Collect-File -Source (Join-Path $gamePathResolved "BZLogger.txt") -DestinationDir $sessionDir -StatusFile $statusFile
    Collect-File -Source (Join-Path $gamePathResolved "winmm_proxy.log") -DestinationDir $sessionDir -StatusFile $statusFile
    Collect-File -Source (Join-Path $gamePathResolved "bz_buffer_log.bin") -DestinationDir $sessionDir -StatusFile $statusFile
    Collect-File -Source (Join-Path $gamePathResolved "bz_buffer_log.meta.txt") -DestinationDir $sessionDir -StatusFile $statusFile
    Collect-File -Source (Join-Path $gamePathResolved "multi.ini") -DestinationDir $sessionDir -StatusFile $statusFile

    Test-Capture -SessionDir $sessionDir

    $zipPath = "$sessionDir.zip"
    if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
    Compress-Archive -Path (Join-Path $sessionDir "*") -DestinationPath $zipPath

    Remove-Item -Force $currentFile -ErrorAction SilentlyContinue
    Write-Host "Buffer logging session stopped."
    Write-Host "Bundle directory: $sessionDir"
    Write-Host "Archive created: $zipPath"
}

switch ($Action) {
    "Start" { Start-Session }
    "Stop" { Stop-Session }
}