$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoSlug = "PiercingXX/battlezone-netcode-patch"
$steamAppId = "301650"
$gameExeName = "battlezone98redux.exe"
$defaultInstallDir = "Battlezone 98 Redux"
# The default ref is baked per branch: fetching this script from a branch
# and running it plain installs that branch. Field-tested the hard way - a
# tester pasted the old
# cmd-flavored command into PowerShell, the outer shell expanded
# $env:BZNET_REF to nothing, and the installer silently fell back to master.
$ref = if ($env:BZNET_REF) { $env:BZNET_REF } else { "master" }
# Same rule as validate_ref in install_linux.sh: the ref lands in three raw
# URLs, and .NET's Uri normalization collapses dot-segments, so a ref like
# `../../other/repo/main` silently retargets the download — including the
# .sha256 sidecar, which would then self-satisfy against the wrong DLL.
if ($ref -notmatch '^[A-Za-z0-9._/-]+$' -or $ref -match '^-|\.\.|//|/$') {
    throw "Refusing malformed git ref '$ref': only letters, digits, dot, underscore, slash and dash; no leading dash, '..', '//' or trailing slash."
}
$gamePath = if ($args.Count -ge 1 -and $args[0]) { [string]$args[0] } elseif ($env:BZNET_GAME_PATH) { $env:BZNET_GAME_PATH } else { "" }
$dllUrl = if ($env:BZNET_DLL_URL) { $env:BZNET_DLL_URL } else { "https://raw.githubusercontent.com/$repoSlug/$ref/prebuilt/windows/winmm.dll" }
$netIniUrl = if ($env:BZNET_NETINI_URL) { $env:BZNET_NETINI_URL } else { "https://raw.githubusercontent.com/$repoSlug/$ref/net-ini/net.ini" }
$shaUrl = if ($env:BZNET_WINMM_SHA256_URL) { $env:BZNET_WINMM_SHA256_URL } else { "https://raw.githubusercontent.com/$repoSlug/$ref/prebuilt/windows/winmm.dll.sha256" }
# The expected hash is read from the sidecar next to the DLL, not baked in
# here.  A hardcoded pin goes stale the moment the prebuilt is refreshed, and
# because this script gets cached (by raw.githubusercontent's CDN, and by
# anyone who saved it to disk) a refresh broke installs for people running a
# copy from before it.  That happened on 2026-07-26 and cost a round of
# support.  The sidecar always travels with the binary, so the two cannot
# disagree.
#
# This is a corruption/truncation check, not a defence against a compromised
# repo: the sidecar shares an origin with the DLL, and `irm | iex` already
# grants that origin arbitrary code execution, so a hardcoded pin bought very
# little.  Anyone who does want strict pinning can set BZNET_WINMM_SHA256 to
# a known value and it wins over the sidecar.
$expectedHash = if ($env:BZNET_WINMM_SHA256) { $env:BZNET_WINMM_SHA256.ToLowerInvariant() } else { "" }

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

# Defender interferes at two points: Invoke-WebRequest dies with "the file
# contains a virus or potentially unwanted software" when real-time
# protection blocks the download, and the DLL can be quarantined out of the
# game folder moments AFTER a successful copy. Same cause both times: an
# unsigned, MinGW-built proxy DLL that hooks the game's networking is
# exactly the shape AV heuristics flag.
function Write-DefenderHelp {
    param([string]$DllPath)
    Write-Host ""
    Write-Warning "Windows Defender blocked the patch DLL. This is a known false positive:"
    Write-Warning "winmm.dll is an unsigned, MinGW-built proxy that hooks the game's"
    Write-Warning "networking - exactly the shape AV heuristics flag. The installer checks"
    Write-Warning "its SHA256 against the repo's published sidecar, so an allowed file is"
    Write-Warning "bit-for-bit the reviewed build."
    Write-Warning ""
    Write-Warning "Fix (keeps Defender on - do NOT disable AV globally):"
    Write-Warning "  1. Windows Security > Virus & threat protection > Protection history"
    Write-Warning "  2. Find the block (often Program:Win32/Contebrew.A!ml or a"
    Write-Warning "     'potentially unwanted/malicious app' entry) > Actions > Allow"
    Write-Warning "  3. Or allow just this one file from an admin PowerShell:"
    Write-Warning "       Add-MpPreference -ExclusionPath `"$DllPath`""
    Write-Warning "  4. Re-run the install command."
}

# Is a non-Microsoft antivirus the registered real-time provider? SecurityCenter2
# lists every registered AV product; anything whose name is not Windows Defender
# means Add-MpPreference is talking to a component that is not the one doing the
# blocking. Best-effort: if the query fails we just say nothing.
function Get-ThirdPartyAV {
    try {
        $avs = Get-CimInstance -Namespace 'root\SecurityCenter2' -ClassName AntiVirusProduct -ErrorAction Stop
        foreach ($av in $avs) {
            if ($av.displayName -and $av.displayName -notmatch 'Windows Defender|Microsoft Defender') {
                return $av.displayName
            }
        }
    } catch { }
    return $null
}

# Pre-authorize our two paths with Windows Defender BEFORE downloading, so a
# Defender machine never sees a block in the first place. This is the
# first-party, supported way to allowlist - it configures Defender and nothing
# else, and it is a no-op (or a silent failure we ignore) when a third-party AV
# owns real-time protection. It never disables anything.
function Add-DefenderExclusions {
    param([string[]]$Paths)
    if (-not (Get-Command Add-MpPreference -ErrorAction SilentlyContinue)) { return }
    foreach ($p in $Paths) {
        try { Add-MpPreference -ExclusionPath $p -ErrorAction Stop }
        catch { }  # not admin, or a third-party AV owns it - handled later
    }
}

# Read the wrapper's single version definition. Anything older than V4.94 kept
# the string inlined in the meta.txt block, so there is nothing to match and
# the honest answer is "unversioned" rather than a guess.
function Get-WrapperVersion {
    param([string]$Path)
    if (-not (Test-Path $Path)) { return "none" }
    $m = Select-String -Path $Path -Pattern '^\$WrapperVersion\s*=\s*"(.+)"' -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value }
    return "pre-V4.94 (unversioned)"
}

# Download bz_wrap.* into $WrapDir and say what was replaced with what.
# Silence here is what let two testers run V4.91-harvest against a V4.92-arms
# repo until 2026-08-15, so the version line prints on every install.
function Update-WrapperFiles {
    param([string]$WrapDir)
    $dest = Join-Path $WrapDir "bz_wrap.ps1"
    $old  = Get-WrapperVersion -Path $dest
    foreach ($wf in @("bz_wrap.ps1", "bz_wrap.bat")) {
        $wu = "https://raw.githubusercontent.com/$repoSlug/$ref/upload/$wf"
        $wfDest = Join-Path $WrapDir $wf
        # An AV quarantine can leave a locked or ACL-broken stub that
        # Invoke-WebRequest cannot overwrite ("Access to the path ... is
        # denied"). Clearing it first gives the write a fresh directory entry
        # to land in.
        Remove-Item -Force -ErrorAction SilentlyContinue $wfDest
        Invoke-WebRequest -Uri $wu -UseBasicParsing -OutFile $wfDest
    }
    $new = Get-WrapperVersion -Path $dest
    if ($old -eq $new) {
        Write-Host "Uploader wrapper: $new (already current)."
    } else {
        Write-Host "Uploader wrapper: $old -> $new."
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

# Allowlist our two paths with Defender up front so a Defender machine sails
# through untouched. Then, if a third-party AV is the real-time provider, say so
# now - before the download it is about to block - with the exact click-path,
# because Add-MpPreference above did not reach it.
$wrapDirForAv = Join-Path $env:LOCALAPPDATA "bz-netcode"
Add-DefenderExclusions -Paths @($gamePath, $wrapDirForAv)
$thirdPartyAv = Get-ThirdPartyAV
if ($thirdPartyAv) {
    Write-Host ""
    Write-Host "Heads up: $thirdPartyAv is your antivirus, not Windows Defender." -ForegroundColor Yellow
    Write-Host "It may block the patch DLL or the uploader script (an unsigned DLL that"
    Write-Host "hooks the game's networking is exactly what heuristics flag). If this"
    Write-Host "install fails or the game will not start, add these two folders to"
    Write-Host "$thirdPartyAv's own exceptions - the Add-MpPreference command only"
    Write-Host "configures Defender:"
    Write-Host "    $gamePath"
    Write-Host "    $wrapDirForAv"
    if ($thirdPartyAv -match 'Bitdefender') {
        Write-Host "  Bitdefender: Protection > Antivirus > Settings > Manage Exceptions."
        Write-Host "  Also add the game to Advanced Threat Defense's exceptions - it watches"
        Write-Host "  running programs and can stop the game mid-match otherwise."
    }
    Write-Host ""
}

try {
    New-Item -ItemType Directory -Path $tempRoot | Out-Null

    if (-not $expectedHash) {
        # Sidecar format is `sha256sum` output: "<64 hex>  winmm.dll".
        Write-Host "Fetching expected hash from $shaUrl"
        $shaText = (Invoke-WebRequest -Uri $shaUrl -UseBasicParsing).Content
        # .Content is a string for text/plain, but byte[] if a proxy strips or
        # rewrites the content type.  Normalise before matching.
        if ($shaText -is [byte[]]) {
            $shaText = [System.Text.Encoding]::ASCII.GetString($shaText)
        }
        $shaMatch = [regex]::Match($shaText, '[0-9a-fA-F]{64}')
        if (-not $shaMatch.Success) {
            throw "Could not read a SHA256 from $shaUrl. Set BZNET_WINMM_SHA256 to install anyway."
        }
        $expectedHash = $shaMatch.Value.ToLowerInvariant()
    }

    Write-Host "Downloading known-good winmm.dll from $dllUrl"
    try {
        Invoke-WebRequest -Uri $dllUrl -OutFile $downloadedDll
    }
    catch {
        if ("$_" -match 'virus|potentially unwanted|malicious') {
            Write-DefenderHelp -DllPath (Join-Path $gamePath "winmm.dll")
        }
        throw
    }
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

    # Zero prompts by design: the tester's whole job is to run one command and
    # paste one launch line. The webhook rides in on BZNET_WEBHOOK, which is
    # baked into the install command pinned in the private channel - so the
    # credential lives in that channel, never in this public repo. No
    # BZNET_WEBHOOK (i.e. a normal player) means no uploader and no questions.
    $wrapperReady = $false
    $wrapperFailed = $false
    if ($env:BZNET_WEBHOOK) {
        if ($env:BZNET_WEBHOOK -notmatch '^https://discord(app)?\.com/api/webhooks/') {
            Write-Warning "BZNET_WEBHOOK is not a Discord webhook URL; skipping upload setup."
        } else {
            try {
                $wrapDir = Join-Path $env:LOCALAPPDATA "bz-netcode"
                New-Item -ItemType Directory -Force -Path $wrapDir | Out-Null
                Update-WrapperFiles -WrapDir $wrapDir
                $confDir = Join-Path $env:APPDATA "bz-netcode"
                New-Item -ItemType Directory -Force -Path $confDir | Out-Null
                # Empty BZ_PLAYER = the wrapper reads the in-game player name from
                # BZLogger at upload time; the OS account name is not
                # who anyone is on Steam.
                $player = if ($env:BZNET_PLAYER) { $env:BZNET_PLAYER } else { "" }
                @(
                    "# Written by install_windows.ps1. Do not commit this file."
                    "BZ_WEBHOOK='$($env:BZNET_WEBHOOK)'"
                    "BZ_PLAYER='$player'"
                    "BZ_INCLUDE_PROTON=0"
                ) | Out-File -FilePath (Join-Path $confDir "upload.conf") -Encoding utf8
                $shown = if ($player) { $player } else { "your in-game name (read at upload time)" }
                Write-Host "Automatic log upload configured for '$shown'."
                $wrapperReady = $true
            } catch {
                Write-Warning "Upload wrapper setup failed: $_"
                $wrapperFailed = $true
            }
        }
    }
    else {
        # No webhook in this shell, but an uploader may already be installed
        # from an earlier run that did have one. Refreshing it here is the
        # whole fix for the 2026-08-15 drift: the credential lives in
        # upload.conf, which this branch never touches, so a tester who
        # re-runs the plain public command still lands on the current wrapper
        # instead of keeping a V4.91 copy forever.
        $wrapDir = Join-Path $env:LOCALAPPDATA "bz-netcode"
        if (Test-Path (Join-Path $wrapDir "bz_wrap.ps1")) {
            try {
                Update-WrapperFiles -WrapDir $wrapDir
                $wrapperReady = $true
            } catch {
                Write-Warning "Could not refresh the existing upload wrapper: $_"
            }
        }
    }

    if ($wrapperFailed) {
        Write-Host ""
        Write-Host "THE LOG UPLOADER DID NOT INSTALL." -ForegroundColor Red
        Write-Host "'Access denied' writing into $env:LOCALAPPDATA\bz-netcode almost always"
        Write-Host "means the antivirus is blocking the wrapper script. Fix, keeping AV on:"
        Write-Host "  1. Windows Security > Virus & threat protection > Protection history"
        Write-Host "     > find the bz_wrap block > Actions > Allow"
        Write-Host "  2. Or exclude the wrapper folder (admin PowerShell):"
        Write-Host "       Add-MpPreference -ExclusionPath `"$env:LOCALAPPDATA\bz-netcode`""
        Write-Host "  3. Then: Remove-Item -Recurse -Force `"$env:LOCALAPPDATA\bz-netcode`""
        Write-Host "     and re-run this install command."
        Write-Host "  NOTE: Add-MpPreference only configures Windows Defender. Running"
        Write-Host "  Bitdefender or another third-party AV? Add the same folder in THAT"
        Write-Host "  product's own exceptions UI instead (for Bitdefender: Protection >"
        Write-Host "  Antivirus > Settings > Manage Exceptions), and restore anything it"
        Write-Host "  quarantined."
        Write-Host ""
        Write-Host "Until that is fixed, leave the Steam launch options EMPTY - pointing"
        Write-Host "them at a wrapper that is not there stops the game from starting."
    }

    if (-not $env:BZNET_WEBHOOK) {
        Write-Host ""
        if ($wrapperReady) {
            # The wrapper was refreshed above but no webhook was supplied, so
            # upload.conf keeps whatever credential it already had. Say which
            # of the two states this is rather than claiming nothing happened.
            Write-Host "No BZNET_WEBHOOK in this shell. The existing log uploader was updated in" -ForegroundColor Yellow
            Write-Host "place and its saved webhook was left alone. Test crew: if uploads stop"
            Write-Host "arriving, re-run the pinned command from the private channel."
        } else {
            Write-Host "No BZNET_WEBHOOK in this shell, so the log uploader was NOT installed." -ForegroundColor Yellow
            Write-Host "Normal players: that is correct, ignore this. Test crew: paste the pinned"
            Write-Host "command from the private channel into a PowerShell window and run it again."
        }
    }

    # Defender's real-time scan is asynchronous: it can quarantine the DLL
    # moments after a successful copy, and "install complete" would then be
    # a lie. Give it a moment and prove the file is still there and intact.
    Start-Sleep -Seconds 3
    if (-not (Test-Path $destPath)) {
        Write-DefenderHelp -DllPath $destPath
        throw "winmm.dll vanished right after install - quarantined by the antivirus. Follow the steps above, then re-run this command."
    }
    $finalHash = (Get-FileHash -Algorithm SHA256 -Path $destPath).Hash.ToLowerInvariant()
    if ($finalHash -ne $expectedHash) {
        throw "winmm.dll changed on disk right after install (expected $expectedHash, got $finalHash)."
    }

    # Same async-quarantine risk for the wrapper scripts: if the AV eats
    # bz_wrap.ps1 after the copy, the launch-option line would point at a
    # wrapper that cannot run. (The .bat falls back to launching the game
    # plain, so play is safe either way - but the uploader would be silently
    # gone.)
    if ($wrapperReady) {
        $wrapDir = Join-Path $env:LOCALAPPDATA "bz-netcode"
        foreach ($wf in @("bz_wrap.ps1", "bz_wrap.bat")) {
            if (-not (Test-Path (Join-Path $wrapDir $wf))) {
                Write-DefenderHelp -DllPath (Join-Path $wrapDir $wf)
                throw "$wf vanished right after install - quarantined by the antivirus. Follow the steps above, then re-run this command."
            }
        }
    }

    Write-Host ""
    Write-Host "Install complete." -ForegroundColor Green
    Write-Host "Installed to: $destPath"
    # The final lines are the only ones anyone reads: the 2026-08-02 Bitdefender
    # case scrolled the red uploader failure off-screen and the green line above
    # read as all-clear. State both outcomes where the eye actually lands.
    if ($wrapperReady) {
        Write-Host "Patch DLL: OK    Log uploader: OK" -ForegroundColor Green
    } elseif ($wrapperFailed) {
        Write-Host "Patch DLL: OK    LOG UPLOADER: DID NOT INSTALL (blocked - scroll up for the fix)" -ForegroundColor Red
    } elseif ($env:BZNET_WEBHOOK) {
        Write-Host "Patch DLL: OK    Log uploader: skipped (BZNET_WEBHOOK is not a Discord webhook URL)" -ForegroundColor Yellow
    } else {
        Write-Host "Patch DLL: OK    Log uploader: not requested (no BZNET_WEBHOOK - correct for normal players)" -ForegroundColor Yellow
    }
    if ($wrapperReady) {
        Write-Host ""
        Write-Host "One step left - set the Steam launch options (Steam > Battlezone 98 Redux"
        Write-Host "> Properties > Launch Options) to:"
        Write-Host ""
        Write-Host '  cmd /c ""%LOCALAPPDATA%\bz-netcode\bz_wrap.bat" %command%"' -ForegroundColor Cyan
        Write-Host ""
        Write-Host "A console window stays open while the game runs - that is the wrapper"
        Write-Host "waiting to bundle your logs on exit. Closing it kills the upload, not the game."
    } else {
        Write-Host "No Steam launch option changes are needed on Windows."
    }
    Write-Host "The retransmit suppressor, bigger socket buffers, DSCP priority marking,"
    Write-Host "the [Net] tuning poke, the governor cold-start fix, RTT sampling and the"
    Write-Host "auto-kick tune are all active by default. Every knob has an environment"
    Write-Host "override - see the proxy README in the repo."
}
finally {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $tempRoot
}