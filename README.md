# Battlezone 98 Redux — Netcode Patch

**V5.0** · [CHANGELOG](CHANGELOG.md)

Fixes multiplayer lag in Battlezone 98 Redux. A small proxy DLL sits between
the game and the network — no game files are modified, and uninstalling
removes it completely.

What it does:

- **Kills retransmit storms.** The engine resends unacknowledged messages
  every frame, so one player's traffic spike snowballs into a flood that
  lags everyone. The patch suppresses the redundant copies at the socket,
  sized to the live round-trip time it measures itself.
- **Fixes the slow match start.** The game hardcodes a 4 KB/s send rate at
  every match start; the patch raises it so the opening world sync doesn't
  crawl.
- **Tunes the network settings** the game never exposed: bigger socket
  buffers, packet priority marking (DSCP), a saner bandwidth governor, and
  auto-kick thresholds relaxed to roughly double stock — enough to forgive a
  lag spike without keeping dead connections around.

## Install

### Windows

Press Start, type `powershell`, Enter — the blue window, not Command Prompt.
Paste this in:

```powershell
irm https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_windows.ps1 | iex
```

That's it. No launch options needed — just start the game.

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

```text
# Windows
cmd /c ""%LOCALAPPDATA%\bz-netcode\bz_wrap.bat" %command%"

# Linux
WINEDLLOVERRIDES=dsound=n,b "${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode/bz_wrap.sh" %command% -nointro
```

No wrapper in the launch options = nothing ever uploads. Bundles contain
every peer's public IP, which is why the destination is a private channel.

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
