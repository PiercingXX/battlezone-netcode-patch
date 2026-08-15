# One-Line Install

Linux builds from source on your machine.
Windows installs the known-good prebuilt DLL with hash verification.

## Linux

Automatic path detection:

```bash
curl -fsSL https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_linux.sh | bash
```

Explicit game path:

```bash
curl -fsSL https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_linux.sh | bash -s -- --game-path "/path/to/Battlezone 98 Redux"
```

Linux installs the MinGW cross-compiler only if it is missing, then builds `dsound.dll` locally.
After deployment, it also runs a Linux EXU compatibility repair pass (best effort) so Workshop EXU files are mirrored into paths the game loader checks.

After install, set Steam launch options once:

```text
WINEDLLOVERRIDES=dsound=n,b %command% -nointro
```

## Windows

Paste these into **PowerShell** (press Start, type `powershell`, Enter), not
Command Prompt.

Automatic path detection:

```powershell
irm https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_windows.ps1 | iex
```

Explicit game path — two lines, same window:

```powershell
$env:BZNET_GAME_PATH = 'D:\Steam\steamapps\common\Battlezone 98 Redux'
irm https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_windows.ps1 | iex
```

Do not wrap either of these in `powershell -NoProfile -ExecutionPolicy Bypass
-Command "..."`. That wrapper was retired: pasted into PowerShell rather than
Command Prompt, the outer shell expands every `$env:` inside the double quotes
before the child shell ever sees it, so the variables arrive empty and the
remaining `='...'` is a syntax error. The ref is baked into each branch's copy
of the script, so fetching it from a branch and running it plain installs that
branch with no `$env:` prefix needed.

Windows installs the known-good `winmm.dll` from this repo and verifies SHA256 before deploy. No Steam launch option changes are required.

The expected hash is read from `prebuilt/windows/winmm.dll.sha256` at run time rather than baked into the script, so refreshing the prebuilt cannot break people running a cached or saved copy of the installer. To pin a specific build instead, set `BZNET_WINMM_SHA256` and it wins over the sidecar:

```powershell
$env:BZNET_WINMM_SHA256='<64 hex>'
```
## Uninstalling

```bash
# Linux
./install/uninstall_linux.sh                 # add --purge-logs to delete captures too

# Windows
powershell -ExecutionPolicy Bypass -File install\uninstall_windows.ps1
```

Removes the proxy DLL and the `net.ini` tuning mod, and offers to remove
`/etc/sysctl.d/99-battlezone-netcode.conf` on Linux. Logs and captures are
**kept** unless you pass `--purge-logs` / `-PurgeLogs` — they are research data,
and an uninstaller should not quietly delete someone's session logs.

Neither script can clear your Steam launch options; do that by hand.
