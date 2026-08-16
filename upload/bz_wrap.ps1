# bz_wrap.ps1 - wrap the game launch, bundle the session, upload it to Discord.
#
# The Windows counterpart of upload/bz_wrap.sh. Read that file's header for why
# this is a launch-option wrapper rather than something inside the DLL; the
# reasoning is identical and is not repeated here.
#
# Steam launch options (Windows):
#   cmd /c ""%LOCALAPPDATA%\bz-netcode\bz_wrap.bat" %command%"
#
# The doubled outer quotes are load-bearing, not a typo. cmd /? : quotes are
# preserved only when there are EXACTLY TWO of them on the line; otherwise cmd
# strips the first and the last. %command% expands to a quoted path, so the
# obvious form -- cmd /c "...bz_wrap.bat" %command% -- has four, and cmd hands
# itself `...bz_wrap.bat" "...game.exe`, then fails with "The filename, directory
# name, or volume label syntax is incorrect." Nothing launches, nothing logs,
# no crash is recorded. The extra pair gives cmd two throwaway quotes to strip
# and leaves the real ones intact.
#
# This is the Windows install's first launch option ever. It is opt-in tooling
# for the test crew, not part of the player install - and that opt-in IS the
# consent step: no wrapper in the launch options, nothing ever uploads.
#
# THE WEBHOOK URL IS NEVER COMMITTED. Run `-Setup` once; it writes
# %APPDATA%\bz-netcode\upload.conf.
#
# PRIVACY: a bundle contains every peer's public IP. The destination must be a
# private channel. See docs\TESTING.md.

[CmdletBinding()]
param(
    [switch]$Setup,
    [switch]$Status,
    [switch]$Retry,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Command
)

$ErrorActionPreference = "Continue"

$ConfDir  = Join-Path $env:APPDATA "bz-netcode"
$ConfFile = Join-Path $ConfDir "upload.conf"
$DataDir  = Join-Path $env:LOCALAPPDATA "bz-netcode"
$Outbox   = Join-Path $DataDir "outbox"
$Work     = Join-Path $DataDir "work"

# One definition, used by meta.txt, -Status and the installer's staleness
# check - the same rule bz_wrap.sh adopted on 2026-08-12. This was previously
# inlined in the meta.txt block only, and it cost exactly what that comment
# predicted: on 2026-08-15 both testers uploaded bundles stamped
# V4.91-harvest while this repo shipped V4.92-arms, and the drift was only
# visible after reading a bundle's meta.txt.
$WrapperVersion = "V5.2-shipped-20260815"

# Discord's webhook attachment cap is ~10 MB unboosted. Leave room for the
# multipart envelope.
$MaxPartBytes = 9 * 1024 * 1024

# Write-Host alone was useless in the field: Steam runs this through
# `cmd /c bz_wrap.bat`, whose console dies with the process, so a launch that
# went wrong left NOTHING behind -- no game, no BZLogger, no error, nothing to
# ask a tester for. "It crashes on boot" was unfalsifiable. Tee every line to a
# file that outlives the console, timestamped, appended across runs.
$WrapLog = Join-Path $DataDir "bz_wrap.log"
function Write-WrapLog {
    param([string]$Message)
    $line = "[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Message
    Write-Host "[bz_wrap] $Message"
    try {
        if (-not (Test-Path $DataDir)) { New-Item -ItemType Directory -Force -Path $DataDir | Out-Null }
        Add-Content -Path $WrapLog -Value $line -Encoding utf8 -ErrorAction Stop
    } catch {
        # Logging must never be the reason someone cannot play.
    }
}

function Get-Conf {
    # UploadMenu defaults ON. It used to default off, on the reasoning that a
    # menu-only session teaches nothing about the netcode - true, but it made
    # "no bundle arrived" ambiguous: skipped-on-purpose and silently-broken
    # look identical from the channel, and two testers reported the uploader
    # as broken when it had correctly decided their session was uninteresting.
    # Only the test crew has an uploader at all (it needs a webhook), and a
    # menu-only bundle is a few kB. Cheap noise beats an unfalsifiable gap.
    # Set BZ_UPLOAD_MENU='0' in upload.conf to restore the old behaviour.
    $conf = @{ Webhook = ""; Player = ""; IncludeProton = $false; UploadMenu = $true }
    if (Test-Path $ConfFile) {
        foreach ($line in (Get-Content $ConfFile)) {
            if ($line -match '^\s*#') { continue }
            if ($line -match '^\s*([A-Za-z_]+)\s*=\s*(.*)$') {
                $k = $Matches[1]; $v = $Matches[2].Trim().Trim("'").Trim('"')
                switch ($k) {
                    "BZ_WEBHOOK" { $conf.Webhook = $v }
                    "BZ_PLAYER"  { $conf.Player = $v }
                    "BZ_INCLUDE_PROTON" { $conf.IncludeProton = ($v -eq "1") }
                    "BZ_UPLOAD_MENU" { $conf.UploadMenu = ($v -eq "1") }
                }
            }
        }
    }
    if ($env:BZ_UPLOAD_WEBHOOK) { $conf.Webhook = $env:BZ_UPLOAD_WEBHOOK }
    return $conf
}

function Invoke-Setup {
    New-Item -ItemType Directory -Force -Path $ConfDir | Out-Null
    $conf = Get-Conf

    Write-Host ""
    Write-Host "bz_wrap setup"
    Write-Host "-------------"
    Write-Host "The webhook URL is pinned in the private Discord channel the bundles land"
    Write-Host "in. It is a weak secret: anyone holding it can post to that one channel,"
    Write-Host "nothing more. It is never written to the repo, and it must not be pasted"
    Write-Host "anywhere public - Discord auto-revokes webhook URLs that appear on GitHub."
    Write-Host ""

    $suffix = if ($conf.Webhook) { " [keep existing]" } else { "" }
    $url = Read-Host "Discord webhook URL$suffix"
    if (-not $url) { $url = $conf.Webhook }
    if (-not $url) { Write-WrapLog "no webhook URL given; nothing saved"; return 1 }
    if ($url -notmatch '^https://discord(app)?\.com/api/webhooks/') {
        Write-WrapLog "that does not look like a Discord webhook URL"
        Write-WrapLog "expected https://discord.com/api/webhooks/<id>/<token>"
        return 1
    }

    $suffix = if ($conf.Player) { " [$($conf.Player)]" } else { "" }
    $player = Read-Host "Your player name (empty = your in-game name, read from BZLogger at upload time)$suffix"
    if (-not $player) { $player = $conf.Player }

    @(
        "# Written by bz_wrap.ps1 -Setup. Do not commit this file."
        "BZ_WEBHOOK='$url'"
        "BZ_PLAYER='$player'"
        "BZ_INCLUDE_PROTON=0"
    ) | Out-File -FilePath $ConfFile -Encoding utf8

    Write-WrapLog "saved $ConfFile"
    Write-Host ""
    Write-Host "Now set your Steam launch options for Battlezone 98 Redux to:"
    Write-Host ""
    Write-Host "  cmd /c `"$(Join-Path $DataDir 'bz_wrap.bat')`" %command%"
    Write-Host ""
    if ((Split-Path -Parent $PSCommandPath) -ne $DataDir) {
        Write-Host "Copy this script and bz_wrap.bat into $DataDir first."
        Write-Host ""
    }
    Write-Host "What gets sent: the proxy log, BZLogger (before and after), multi.ini,"
    Write-Host "and a meta file. Bundles contain every peer's public IP, which is why"
    Write-Host "the destination is a private channel."
    return 0
}

function Show-Status {
    $conf = Get-Conf
    $present = if (Test-Path $ConfFile) { "(present)" } else { "(MISSING - run -Setup)" }
    Write-Host "wrapper     : $WrapperVersion"
    Write-Host "config file : $ConfFile $present"
    Write-Host "webhook     : $(if ($conf.Webhook) { 'configured' } else { 'NOT SET' })"
    Write-Host "player      : $(if ($conf.Player) { $conf.Player } else { '<auto: in-game name, read from BZLogger at upload time>' })"
    Write-Host "outbox      : $Outbox"
    if (Test-Path $Outbox) {
        $n = @(Get-ChildItem -File $Outbox -ErrorAction SilentlyContinue |
               Where-Object { $_.Name -like "*.zip*" -and $_.Name -notlike "*.msg" }).Count
        Write-Host "              $n bundle(s) waiting"
    } else {
        Write-Host "              empty"
    }
}

# Steam substitutes %command% without quoting, so a game path with spaces
# ("...\Battlezone 98 Redux\battlezone98redux.exe") can arrive split across
# arguments. Greedily rejoin leading tokens until they name a file that
# exists; if the first token already does, nothing changes.
function Resolve-SplitCommand {
    param([string[]]$CommandArgs)
    if ($CommandArgs.Count -le 1 -or (Test-Path -LiteralPath $CommandArgs[0] -PathType Leaf)) { return ,$CommandArgs }
    for ($n = 2; $n -le $CommandArgs.Count; $n++) {
        $candidate = ($CommandArgs[0..($n - 1)] -join " ")
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $rest = if ($n -lt $CommandArgs.Count) { $CommandArgs[$n..($CommandArgs.Count - 1)] } else { @() }
            return ,([string[]](@($candidate) + $rest))
        }
    }
    return ,$CommandArgs
}

# The dir this returns is where logs are HARVESTED from, so it must be the
# game root (the folder holding battlezone98redux.exe), not wherever the exe
# Steam named happens to live. Steam's %command% names Launcher\BZLauncher.exe,
# and returning that exe's parent uncorrected is why the first real Windows
# bundle (2026-08-02, a full hosted evening) came back holding only meta.txt:
# every log was harvested from ...\launcher, where the game writes nothing.
function Resolve-GameRoot {
    param([string]$Dir)
    if (-not $Dir) { return $Dir }
    if (Test-Path -LiteralPath (Join-Path $Dir "battlezone98redux.exe")) { return $Dir }
    $up = Split-Path -Parent $Dir
    if ($up -and (Test-Path -LiteralPath (Join-Path $up "battlezone98redux.exe"))) { return $up }
    return $Dir
}

function Get-GameDirFromCommand {
    param([string[]]$CommandArgs)
    foreach ($a in $CommandArgs) {
        if ($a -match 'battlezone98redux\.exe$') { return (Split-Path -Parent $a) }
    }
    # -LiteralPath: a Steam library path containing [ or ] would silently fail
    # a wildcard-interpreting Test-Path and lose the bundle for that tester.
    foreach ($a in $CommandArgs) {
        if (Test-Path -LiteralPath $a -PathType Leaf) {
            return (Resolve-GameRoot (Split-Path -Parent $a))
        }
    }
    return ""
}

# What would catch a crash dump on this machine, if anything. The wrapper only
# REPORTS this (arming procdump is tester_diag's job; WER LocalDumps needs
# admin) - but the 2026-08-03 map-load crash produced no dump and nobody knew
# none could exist until the bundle was already the only evidence. Now the
# bundle says so itself.
function Get-CrashCaptureStatus {
    $wer = Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps" -ErrorAction SilentlyContinue
    if ($wer) { return "wer-localdumps" }
    $candidates = @(
        (Get-Command procdump.exe -ErrorAction SilentlyContinue | ForEach-Object { $_.Source }),
        "C:\Program Files\Sysinternals\procdump.exe",
        "C:\Sysinternals\procdump.exe"
    ) | Where-Object { $_ -and (Test-Path $_) }
    if ($candidates.Count -gt 0) { return "procdump-available-not-armed (run tester_diag to arm it)" }
    return "NONE (a crash leaves no dump; enable WER LocalDumps or install procdump + tester_diag)"
}

# A BZLogger that never wrote "Exiting Game With Return Code" ended abruptly.
# Same rule tools\analyze_drops.py uses.
function Test-Crashed {
    param([string]$LogPath)
    if (-not (Test-Path $LogPath)) { return $false }
    $tail = Get-Content $LogPath -Tail 2000 -ErrorAction SilentlyContinue
    return -not ($tail -match 'Exiting Game With Return Code')
}

function Get-MapName {
    param([string]$LogPath)
    if (-not (Test-Path $LogPath)) { return "unknown" }
    $hit = Select-String -Path $LogPath -Pattern 'Launching Network Game .*, Map ([^,]+)' `
                         -ErrorAction SilentlyContinue | Select-Object -Last 1
    if ($hit) { return $hit.Matches[0].Groups[1].Value.Trim() }
    return "unknown"
}

# Built on HttpClient, NOT Invoke-RestMethod -Form: the .bat runs `powershell`,
# which is Windows PowerShell 5.1 on every stock machine, and -Form only
# exists in PowerShell 6+. HttpClient is identical in both, and the explicit
# quotes on the part names force the RFC-style quoted Content-Disposition
# Discord expects (pwsh's -Form emits them unquoted).
function Send-Bundle {
    param([string]$FilePath, [string]$Message)
    $conf = Get-Conf
    if (-not $conf.Webhook) { return $false }
    $client = $null
    try {
        Add-Type -AssemblyName System.Net.Http -ErrorAction SilentlyContinue
        $client = New-Object System.Net.Http.HttpClient
        $client.Timeout = [TimeSpan]::FromSeconds(300)
        $mp = New-Object System.Net.Http.MultipartFormDataContent
        $payload = @{ content = $Message } | ConvertTo-Json -Compress
        $sc = New-Object System.Net.Http.StringContent($payload, [System.Text.Encoding]::UTF8)
        $mp.Add($sc, '"payload_json"')
        $bytes = [System.IO.File]::ReadAllBytes($FilePath)
        $bc = New-Object System.Net.Http.ByteArrayContent(@(,$bytes))
        $bc.Headers.ContentType = New-Object System.Net.Http.Headers.MediaTypeHeaderValue("application/octet-stream")
        $name = [System.IO.Path]::GetFileName($FilePath)
        # Set the disposition by hand: the Add(name, fileName) overload also
        # emits an RFC 5987 filename* pair that double-encodes the quotes.
        $bc.Headers.ContentDisposition = New-Object System.Net.Http.Headers.ContentDispositionHeaderValue("form-data")
        $bc.Headers.ContentDisposition.Name = '"file1"'
        $bc.Headers.ContentDisposition.FileName = ('"{0}"' -f $name)
        $mp.Add($bc)
        $resp = $client.PostAsync($conf.Webhook, $mp).GetAwaiter().GetResult()
        if (-not $resp.IsSuccessStatusCode) {
            Write-WrapLog "upload failed: HTTP $([int]$resp.StatusCode)"
            return $false
        }
        return $true
    } catch {
        Write-WrapLog "upload failed: $($_.Exception.Message)"
        return $false
    } finally {
        if ($client) { $client.Dispose() }
    }
}

function Move-ToOutbox {
    param([string]$FilePath, [string]$Message)
    New-Item -ItemType Directory -Force -Path $Outbox | Out-Null
    $dest = Join-Path $Outbox (Split-Path -Leaf $FilePath)
    Move-Item -Force $FilePath $dest
    $Message | Out-File -FilePath "$dest.msg" -Encoding utf8
    Write-WrapLog "upload failed; parked in $Outbox (retried on the next wrapped launch)"
}

function Clear-Outbox {
    if (-not (Test-Path $Outbox)) { return }
    $parked = Get-ChildItem -File $Outbox -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -like "*.zip" -or ($_.Name -like "*.zip.part*" -and $_.Name -notlike "*.msg") }
    foreach ($f in $parked) {
        $msgFile = "$($f.FullName).msg"
        $msg = if (Test-Path $msgFile) { Get-Content $msgFile -Raw } else { "(retry) $($f.Name)" }
        if (Send-Bundle -FilePath $f.FullName -Message $msg) {
            Write-WrapLog "uploaded parked bundle $($f.Name)"
            Remove-Item -Force $f.FullName, $msgFile -ErrorAction SilentlyContinue
        } else {
            Write-WrapLog "parked bundle still not uploadable; leaving it"
            return
        }
    }
}

# The name the bundle carries must be the name the game itself uses -- the
# one every peer's BZLogger prints in its Adding Player lines. The session's
# own log states it outright ("Authenticated to BZRNet As S<steamid>:<name>"),
# and unlike the Adding Player lines that one only ever names the LOCAL
# player. The Steam persona in loginusers.vdf proved to be a login-time
# snapshot that matched nobody's in-game name.
function Get-GamePlayerName([string]$GameDir) {
    if (-not $GameDir) { return $null }
    $log = Join-Path $GameDir "BZLogger.txt"
    if (-not (Test-Path $log)) { return $null }
    $hit = Select-String -Path $log -Pattern 'Authenticated to BZRNet As S\d+:(.+)$' |
           Select-Object -Last 1
    if ($hit) { return $hit.Matches[0].Groups[1].Value.Trim() }
    return $null
}

# Fallback only, for a session that died before authenticating: the Steam
# persona of the "MostRecent" "1" block in Steam's config/loginusers.vdf.
function Get-SteamPlayerName([string]$GameDir) {
    if (-not $GameDir) { return $null }
    $m = [regex]::Match($GameDir, '^(.*?)[\\/]steamapps[\\/]')
    if (-not $m.Success) { return $null }
    $vdf = Join-Path $m.Groups[1].Value "config/loginusers.vdf"
    if (-not (Test-Path $vdf)) { return $null }
    $name = $null
    foreach ($line in (Get-Content $vdf -ErrorAction SilentlyContinue)) {
        if ($line -match '"PersonaName"\s+"([^"]+)"') { $name = $Matches[1] }
        elseif ($name -and $line -match '"MostRecent"\s+"1"') { return $name }
    }
    return $name
}

function New-BundleAndUpload {
    param([string]$GameDir, [string]$Snapshot, [string]$StartUtc, [int]$ExitCode)

    $conf = Get-Conf
    # An explicit configured name still wins; otherwise the in-game name
    # from this session's own log, then the Steam persona, and the OS
    # account name only as a last resort. Sanitized because it lands in the
    # bundle filename.
    $player = $conf.Player
    if (-not $player) { $player = Get-GamePlayerName $GameDir }
    if (-not $player) { $player = Get-SteamPlayerName $GameDir }
    if (-not $player) { $player = $env:USERNAME }
    $player = $player -replace '[^A-Za-z0-9._-]', '-'
    $host_  = $env:COMPUTERNAME
    $stamp  = (Get-Date).ToUniversalTime().ToString("yyyyMMddTHHmmssZ")
    $bundle = Join-Path $Work "bz_${player}_${stamp}"
    New-Item -ItemType Directory -Force -Path $bundle | Out-Null

    $offset = [int](Get-Date).Subtract((Get-Date).ToUniversalTime()).TotalSeconds
    @(
        "player=$player"
        "hostname=$host_"
        "start_utc=$StartUtc"
        "overrides=$($SessionOverrides -join ' ')"
        "crash_capture=$(Get-CrashCaptureStatus)"
        "end_utc=$((Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))"
        "end_local=$((Get-Date).ToString('yyyy-MM-ddTHH:mm:sszzz'))"
        "utc_offset_seconds=$offset"
        "game_exit_code=$ExitCode"
        "game_dir=$GameDir"
        "platform=windows"
        "wrapper_version=$WrapperVersion"
    ) | Out-File -FilePath (Join-Path $bundle "meta.txt") -Encoding utf8

    # The pre-launch snapshot is the whole point: this is the previous
    # session's log, which this launch has already overwritten.
    if ($Snapshot -and (Test-Path $Snapshot)) {
        Copy-Item -Force $Snapshot (Join-Path $bundle "BZLogger.prelaunch.txt")
    }

    foreach ($f in @("BZLogger.txt", "winmm_proxy.log", "dsound_proxy.log",
                     "multi.ini")) {
        $src = Join-Path $GameDir $f
        if (Test-Path $src) { Copy-Item -Force $src (Join-Path $bundle $f) }
    }
    # Capture files persist in the game dir after the capture that made them,
    # so with logging off a bundle would ship YESTERDAY'S ring looking current
    # (happened 2026-08-03). Only take them if written during this session.
    foreach ($f in @("bz_buffer_log.bin", "bz_buffer_log.meta.txt",
                     "capture_verify.txt")) {
        $src = Join-Path $GameDir $f
        if (-not (Test-Path $src)) { continue }
        if ((Get-Item $src).LastWriteTimeUtc -lt $SessionStart) {
            Write-WrapLog "skipping stale $f (predates this session)"
            continue
        }
        Copy-Item -Force $src (Join-Path $bundle $f)
    }

    # The wrapper's own log tail rides along so that a bundle that arrives
    # empty or odd explains itself instead of costing a support round-trip
    # (the 2026-08-02 meta.txt-only bundle took exactly that).
    if (Test-Path $WrapLog) {
        Get-Content $WrapLog -Tail 200 -ErrorAction SilentlyContinue |
            Out-File -FilePath (Join-Path $bundle "bz_wrap.log.tail.txt") -Encoding utf8
    }

    $crashFlag = ""
    if (Test-Crashed (Join-Path $bundle "BZLogger.txt")) {
        $crashFlag = " **CRASH** (no ``Exiting Game With Return Code``)"
    }
    $map = Get-MapName (Join-Path $bundle "BZLogger.txt")

    # Menu-only sessions (no network game, no crash) teach nothing about the
    # netcode; a crash without a map line still goes.
    if ($map -eq "unknown" -and -not $crashFlag -and -not $conf.UploadMenu) {
        Write-WrapLog "no network game this session and no crash; BZ_UPLOAD_MENU=0 is set, skipping upload"
        Remove-Item -Recurse -Force $bundle
        return
    }

    $minutes = [int](((Get-Date).ToUniversalTime() - [datetime]::Parse($StartUtc).ToUniversalTime()).TotalMinutes)
    $message = "**$player** - map ``$map``, $minutes min, exit $ExitCode$crashFlag"

    # Compress-Archive is Deflate, not xz -- less effective on a 31 MB
    # BZLogger, but it is in the box on every supported Windows.
    $zip = "$bundle.zip"
    if (Test-Path $zip) { Remove-Item -Force $zip }
    Compress-Archive -Path (Join-Path $bundle "*") -DestinationPath $zip
    Remove-Item -Recurse -Force $bundle

    $size = (Get-Item $zip).Length
    Write-WrapLog "bundle $(Split-Path -Leaf $zip) is $([int]($size / 1024)) kB"

    if ($size -le $MaxPartBytes) {
        if (Send-Bundle -FilePath $zip -Message $message) {
            Write-WrapLog "uploaded"
            Remove-Item -Force $zip
        } else {
            Move-ToOutbox -FilePath $zip -Message $message
        }
        return
    }

    # Oversized: split into raw byte-range parts. The receiver reassembles with
    # `cmd /c copy /b part00+part01 bundle.zip`, or `cat` anywhere else.
    Write-WrapLog "bundle exceeds the webhook cap; splitting"
    $bytes = [System.IO.File]::ReadAllBytes($zip)
    $total = [math]::Ceiling($bytes.Length / $MaxPartBytes)
    for ($i = 0; $i -lt $total; $i++) {
        $start = $i * $MaxPartBytes
        $len = [math]::Min($MaxPartBytes, $bytes.Length - $start)
        $part = "$zip.part{0:d2}" -f $i
        $fs = [System.IO.File]::OpenWrite($part)
        $fs.Write($bytes, $start, $len)
        $fs.Close()
        $partMsg = "$message - part $($i + 1) of $total (reassemble with copy /b)"
        if (Send-Bundle -FilePath $part -Message $partMsg) {
            Remove-Item -Force $part
        } else {
            Move-ToOutbox -FilePath $part -Message $partMsg
        }
    }
    Remove-Item -Force $zip
}

# -- Main ---------------------------------------------------------------------

if ($Setup)  { exit (Invoke-Setup) }
if ($Status) { Show-Status; exit 0 }
if ($Retry)  { Clear-Outbox; exit 0 }

# Log what Steam actually handed us before anything can go wrong with it. When
# a launch fails there is otherwise no way to tell a mangled %command% (bad
# quoting, a trailing flag bound to the wrong parameter, a path that no longer
# exists) from a game that started and died.
Write-WrapLog ("=== run start === argc={0}" -f $(if ($Command) { $Command.Count } else { 0 }))
if ($Command) {
    for ($i = 0; $i -lt $Command.Count; $i++) {
        Write-WrapLog ("  arg[{0}] = {1}" -f $i, $Command[$i])
    }
}

if (-not $Command -or $Command.Count -eq 0) {
    Write-WrapLog "no command passed - Steam launch options are probably malformed"
    Write-Host "Usage: bz_wrap.ps1 [-Setup|-Status|-Retry] | <game command...>"
    exit 0
}

# EVERYTHING before Start-Process is best-effort: the one job this wrapper
# must never fail at is launching the game. A tester's report of "crashing
# before the launcher opens" traced to exactly this stretch (the $Args
# parameter clobbered by PowerShell's automatic variable), so the whole
# pre-launch phase is now fenced.
$gameDir = ""
$snapshot = ""
$startUtc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
$SessionStart = (Get-Date).ToUniversalTime()
$SessionOverrides = @()
try {
    New-Item -ItemType Directory -Force -Path $Work | Out-Null

    # Per-session BZ_* overrides, for switching A/B arms without editing the
    # Steam launch line: one KEY=VALUE per line in %APPDATA%\bz-netcode\
    # bznet.env (BZ_ keys only - this file is a tuning knob, not a general
    # environment editor). The host switching BZ_GOV_START between
    # 16000/40000/80000 is the use this exists for. Every override is logged
    # and recorded in meta.txt so the bundle says which arm it was.
    $envFile = Join-Path $ConfDir "bznet.env"
    if (Test-Path -LiteralPath $envFile) {
        foreach ($line in (Get-Content $envFile)) {
            if ($line -match '^\s*(BZ_[A-Za-z0-9_]+)\s*=\s*(.*)$') {
                $k = $Matches[1]; $v = $Matches[2].Trim().Trim("'").Trim('"')
                Set-Item -Path "env:$k" -Value $v
                $SessionOverrides += "$k=$v"
                Write-WrapLog "session override: $k=$v (from bznet.env)"
            }
        }
    }

    $resolved = Resolve-SplitCommand -CommandArgs $Command
    if ($resolved -and $resolved.Count -gt 0) { $Command = $resolved }
    $gameDir = Get-GameDirFromCommand -CommandArgs $Command
    if (-not $gameDir) {
        Write-WrapLog "could not work out the game directory from the command line"
        Write-WrapLog "launching anyway; no bundle will be collected"
    }

    # Pre-launch: snapshot BZLogger before the game truncates it.
    if ($gameDir -and (Test-Path (Join-Path $gameDir "BZLogger.txt"))) {
        $snapshot = Join-Path $Work "BZLogger.prelaunch.$PID.txt"
        Copy-Item -Force (Join-Path $gameDir "BZLogger.txt") $snapshot
        Write-WrapLog "snapshotted the previous session's BZLogger"
    }

    # Anything parked from a previous run goes now, before the game saturates
    # the uplink.
    Clear-Outbox
} catch {
    Write-WrapLog "pre-launch wrapper step failed: $($_.Exception.Message)"
    Write-WrapLog "launching the game anyway"
}

# Run the game. Never let a wrapper failure stop someone playing.
$rc = 1
try {
    $exe = $Command[0]
    # Start the game with its own folder as the working directory: BZLogger.txt
    # is written relative to the cwd, so without this the game runs fine but
    # drops its log wherever the wrapper happened to be -- %LOCALAPPDATA%\
    # bz-netcode via `cmd /c bz_wrap.bat` -- and the bundle we upload contains
    # no BZLogger at all. Measured on Windows 11 2026-08-01: launched from
    # C:\Users the process starts but the game dir gets no BZLogger; with the
    # working directory set it appears immediately. Steam sets the directory
    # itself, which is why this only bites through the wrapper.
    # %command% can name Launcher\BZLauncher.exe rather than the game exe, and
    # the launcher's own folder is the wrong place to run from -- the game root
    # (the folder holding battlezone98redux.exe, and our winmm.dll) is right.
    # $gameDir is already root-corrected by Resolve-GameRoot; the fallback for
    # an unresolved $gameDir gets the same correction.
    $workDir = $gameDir
    if (-not $workDir) { $workDir = Resolve-GameRoot (Split-Path -Parent $exe) }
    if ($workDir -and -not (Test-Path -LiteralPath $workDir)) { $workDir = "" }
    if (-not $workDir) { Write-WrapLog "no working directory resolved; the game may not start" }
    else { Write-WrapLog "working directory: $workDir" }

    # Start-Process rejects an empty -ArgumentList, and %command% is often
    # just the exe path.
    $spArgs = @{ FilePath = $exe; PassThru = $true; Wait = $true }
    if ($Command.Count -gt 1) { $spArgs.ArgumentList = $Command[1..($Command.Count - 1)] }
    if ($workDir) { $spArgs.WorkingDirectory = $workDir }
    $proc = Start-Process @spArgs
    if ($proc) { $rc = $proc.ExitCode }
} catch {
    Write-WrapLog "FAILED TO LAUNCH THE GAME: $($_.Exception.Message)"
    Write-WrapLog "command was: $($Command -join ' ')"
}

# Steam's %command% is Launcher\BZLauncher.exe, which spawns
# battlezone98redux.exe and exits within seconds. -Wait therefore returns while
# the session is only just starting, so the wrapper would bundle a BZLogger
# with no games in it, find no "Exiting Game With Return Code", flag it a
# CRASH, upload that, and finish long before the real session ended. That is
# why testers saw crash-shaped bundles arrive and never got a normal one.
# Waiting on the game process itself is correct whichever exe Steam names: if
# %command% already was the game, it has exited by now and this returns at
# once.
$waited = 0
while ((Get-Process -Name battlezone98redux -ErrorAction SilentlyContinue) -and $waited -lt 43200) {
    if ($waited -eq 0) { Write-WrapLog "launcher returned; game still running - waiting for it to exit" }
    Start-Sleep -Seconds 5
    $waited += 5
}
if ($waited -gt 0) { Write-WrapLog "game process ended after a further $waited s" }

Write-WrapLog "game exited with $rc"

if ($gameDir) {
    try {
        New-BundleAndUpload -GameDir $gameDir -Snapshot $snapshot -StartUtc $startUtc -ExitCode $rc
    } catch {
        Write-WrapLog "bundle collection failed: $($_.Exception.Message)"
        Write-WrapLog "the game session itself was unaffected"
    }
}
if ($snapshot -and (Test-Path $snapshot)) { Remove-Item -Force $snapshot }

exit $rc
