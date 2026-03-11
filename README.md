# Battlezone 98 Redux Netcode Runtime Patch

Runtime-only multiplayer net buffer patch for Battlezone 98 Redux.

No on-disk EXE editing in the normal workflow.

## What this does 🛠️

- Patches running process memory (not the game file on disk)
- Uses build-tolerant runtime detection on Linux + Windows
- Safely aborts if address detection is ambiguous

Target profile:

- Send buffer: 524288 (512 KB)
- Receive buffer: 2097152 (2 MB)

## Files 📦

- `runtime_patch_linux.sh`
- `runtime_patch_linux.py`
- `runtime_patch_windows.ps1`
- `run_test_linux.sh`
- `run_test_windows.ps1`
- `verify_net_patch.sh`
- `verify_net_patch.ps1`
- `launch_and_patch_linux.sh` (optional one-shot launcher + patcher)


---


## Linux Quick Start 🚀

First-time user flow:

1. Launch game from Steam and wait at in-game main menu.
2. Apply runtime patch:

```bash
bash ./runtime_patch_linux.sh "/path/to/Battlezone 98 Redux"
```

3. Host or join one multiplayer session.
4. Verify latest startup session:

```bash
cd "/path/to/Battlezone 98 Redux"
VERIFY_RUNTIME_ONLY=1 /path/to/Battlezone\ Netcode\ Patch/verify_net_patch.sh
```

Guided Linux flow (interactive prompts):

```bash
bash ./run_test_linux.sh "/path/to/Battlezone 98 Redux"
```


---


## Windows Quick Start 🪟

Required order:

1. Start Steam as Administrator.
2. Launch game and wait at in-game main menu.
3. Open PowerShell as Administrator.
4. Run:

```powershell
.\runtime_patch_windows.ps1
```

Guided flow:

```powershell
.\run_test_windows.ps1 -GameRoot "C:\Path\To\Battlezone 98 Redux"
```


---



## Verify ✅

After entering multiplayer once, verify latest log session:

Linux:

```bash
cd "/path/to/Battlezone 98 Redux"
VERIFY_RUNTIME_ONLY=1 /path/to/Battlezone\ Netcode\ Patch/verify_net_patch.sh
```

Windows:

```powershell
cd "C:\Path\To\Battlezone 98 Redux"
$env:VERIFY_RUNTIME_ONLY="1"
\path\to\Battlezone Netcode Patch\verify_net_patch.ps1
```

Expected line:

`BZRNet P2P Socket Opened With 2097152 received buffer, 524288 send buffer`


---


## Troubleshooting 🧯

Linux ptrace blocked:

```bash
sudo sysctl -w kernel.yama.ptrace_scope=0
pkill -9 r2 || true
```

Windows access denied:

- Run both Steam/game and PowerShell as Administrator
- Keep elevation level consistent (both elevated)

Windows optional PID targeting:

```powershell
.\runtime_patch_windows.ps1 -TargetPid 12345
```

---


## Notes 📝

- Runtime patching must be applied each game launch (memory resets on exit)
- This repo is now runtime-first and build-tolerant by design


---


## Optional: Linux One-Command Launcher

Use this if you want one command for a terminal keybind:

```bash
bash ./launch_and_patch_linux.sh "/path/to/Battlezone 98 Redux"
```

What it does:

- Launches via Steam (`steam -applaunch 301650` by default)
- Waits for game process
- Applies runtime patch automatically

Optional env vars:

- `BATTLEZONE_APP_ID` (default `301650`)
- `RUNTIME_PATCH_TIMEOUT_SECS` (default `180`)
