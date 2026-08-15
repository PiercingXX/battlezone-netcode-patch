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
-Command "..."`. Pasted into PowerShell rather than Command Prompt, the outer
shell expands every `$env:` inside the double quotes before the child shell
sees it, so the variables arrive empty — the explicit-path form then reduces
to a bare `='...'` and fails to parse. The ref is baked into each branch's
copy of the script, so running it plain installs the branch you fetched it
from.

Windows installs the known-good `winmm.dll` from this repo and verifies SHA256 before deploy. No Steam launch option changes are required.

The expected hash is read from `prebuilt/windows/winmm.dll.sha256` at run time rather than baked into the script, so refreshing the prebuilt cannot break people running a cached or saved copy of the installer. To pin a specific build instead, set `BZNET_WINMM_SHA256` and it wins over the sidecar:

```powershell
$env:BZNET_WINMM_SHA256='<64 hex>'
```