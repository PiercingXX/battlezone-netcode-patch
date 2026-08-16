# Battlezone 98 Redux — Netcode Patch

**V5.3** · [CHANGELOG](CHANGELOG.md)

Fixes multiplayer lag in Battlezone 98 Redux. A small proxy DLL sits between
the game and the network — no game files are modified, and uninstalling
removes it completely.

What it does:

- **Fixes the slow match start.** The game hardcodes a 4 KB/s send rate at
  every match start; the patch raises it so the opening world sync doesn't
  crawl.
- **Tunes the network settings** the game never exposed: bigger socket
  buffers, packet priority marking (DSCP), a saner bandwidth governor, and
  relaxed auto-kick thresholds — enough to forgive a lag spike without
  keeping dead connections around.
- **Measures everything.** Per-peer round-trip time, send rates, burst
  shape — the telemetry that lets a lag report be diagnosed instead of
  guessed at.

## What changed in V5.3

**This release makes the game behave like V4.9 on the wire — the
best-performing build so far. Everyone should update, especially whoever
hosts.**

The V5.0 line changed two things on the traffic path, and the 2026-08-15/16
session — the worst on record — implicated both:

1. **The duplicate suppressor is off by default again.** V5.0 turned it on
   for everyone and fed it live RTT. It drops what it judges to be redundant
   retransmits, but the engine's per-frame resend is its *only* way of
   recovering a lost packet, and during the storms it was dropping up to 63%
   of that traffic — suppressing the recovery exactly when links were lossy.
   It goes back to what it was in V4.9: built in, off, `BZ_SEND_DAMPEN=1`
   for experiments.
2. **The bandwidth ceiling goes back up (64 KB/s → 320 KB/s).** V5.0 cut it
   based on quiet-match measurements; the storm traffic peaked at 86–224
   KB/s, above the new ceiling, so the governor spent whole matches
   throttling. Restored to the V4.9 value.

Auto-kick thresholds already went back to V4.9 in V5.2 and stay there. Net
effect of V5.2 + V5.3: **the wire behavior is V4.9's**, and what remains of
the V5 line is the build stamping, the RTT/burst telemetry, and the
installer fixes.

Hosts matter most (the tuning applies from the host), but the suppressor ran
on every machine — so this update is for the whole crew.

## Install

### Windows

Press Start, type `powershell`, Enter — the blue window, not Command Prompt.
Paste this in:

```powershell
irm https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_windows.ps1 | iex
```

That's it. No launch options needed — just start the game.

<details>
<summary>If the command fails with an "empty string" error</summary>

`Cannot bind argument to parameter 'Command'` means `irm` downloaded nothing —
something on your machine emptied the response (an ad-blocker or corporate
DNS blackholing `raw.githubusercontent.com`, or antivirus HTTPS inspection).
Download to a file first to see the real failure:

```powershell
$dst = "$env:TEMP\bznet_install.ps1"
Invoke-WebRequest -UseBasicParsing -Uri 'https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_windows.ps1' -OutFile $dst
(Get-Item $dst).Length   # expect a few thousand bytes, not 0
powershell -NoProfile -ExecutionPolicy Bypass -File $dst
```
</details>

<details>
<summary>If Defender flags <code>winmm.dll</code></summary>

A known false positive (often `Program:Win32/Contebrew.A!ml`) — unsigned DLL
proxies that hook networking are exactly the shape AV heuristics flag. The
installer verifies the file's SHA256 against the repo's published hash.
Restore it from Protection History and add an exception for that one file.
Don't disable AV globally.
</details>

### Linux (Proton)

```bash
curl -fsSL https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_linux.sh | bash
```

Then set the Steam launch options once (Steam → Battlezone 98 Redux →
Properties → Launch Options):

```text
WINEDLLOVERRIDES=dsound=n,b %command% -nointro
```

That same line is correct for every Steam flavour — native, Flatpak and Snap.
The installer patches **every** Steam install it finds, so if you have more
than one, set the launch options in each of them; whichever one you launch
from is the one that has to carry the override.

## Uninstall

```bash
# Linux
curl -fsSL https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/uninstall_linux.sh | bash

# Windows (PowerShell)
irm https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/uninstall_windows.ps1 | iex
```

## For hosts

The host's settings matter most — the tuning mod (`net.ini`) installs
automatically and applies when you host. Nothing to configure.

## Test crew: session logging (opt-in)

Logging is off unless you opt in. Members of the test crew can have each
session's logs bundled and uploaded automatically to the private channel:

1. Install with the pinned command from the private Discord channel (it's
   the normal install command with the webhook included).
2. Use the wrapper launch option instead of the plain one:

Windows:

```text
cmd /c ""%LOCALAPPDATA%\bz-netcode\bz_wrap.bat" %command%"
```

Linux — native or Flatpak Steam:

```text
WINEDLLOVERRIDES=dsound=n,b "${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode/bz_wrap.sh" %command% -nointro
```

Linux — Snap Steam:

```text
WINEDLLOVERRIDES=dsound=n,b "$SNAP_USER_COMMON/.local/share/bz-netcode/bz_wrap.sh" %command% -nointro
```

The installer prints the right one for your machine on its last lines — copy
that. Don't guess between them: the wrapper is the launch target, so a path
that doesn't resolve inside the sandbox kills the launch instead of starting
the game.

No wrapper in the launch options = nothing ever uploads. Bundles contain
every peer's public IP, which is why the destination is a private channel.

### Snap Steam

Snap needs its own line because snapd remaps `HOME` into
`~/snap/steam/common/` and its home interface hides the host's dot-directories
outright, so the `XDG_DATA_HOME` path above can never exist inside the
sandbox. `$SNAP_USER_COMMON` is guaranteed by snapd and points at the mirrored
copy the installer places there.

The Steam snap's runtime also ships neither `curl` nor `python3`, so nothing
inside the sandbox can send a bundle. Sessions are parked in an outbox and a
host-side systemd user unit drains them — the installer enables
`bz-netcode-retry.path` (fires within seconds of the game exiting) plus a
10-minute timer as a backstop. Nothing to do; a parked bundle is not a lost
one. If the installer says it couldn't enable those units, send by hand:

```bash
"$HOME/snap/steam/common/.local/share/bz-netcode/bz_wrap.sh" --retry
```

## Advanced

Every behavior has an environment-variable override — see the proxy READMEs
([Windows](Microslop/winmm_proxy/README.md),
[Linux](Linux/proton_dsound_proxy/README.md)). Protocol research and
investigation writeups live in [resources/](resources/). Diagnostic tooling
(packet capture, log analysis) lives in [buffer-logging/](buffer-logging/)
and [tools/](tools/).

Building from source: `make` in `Microslop/winmm_proxy/` or
`Linux/proton_dsound_proxy/` (needs 32-bit MinGW), tests via
`make -C tests run`.
