# Battlezone 98 Redux Net Buffer Patch Test Guide

Portable cross-platform test kit for Linux and Windows.

Target profile:

- Send buffer: 524288 (512 KB)
- Receive buffer: 2097152 (2 MB)

## Files

- `apply_binary_patch.py`
- `net_fix_manifest.send512k.recv2m.json`
- `runtime_patch_linux.sh`
- `runtime_patch_linux.py`
- `auto_patch_linux_watch.sh`
- `runtime_patch_windows.ps1`
- `verify_net_patch.sh`
- `verify_net_patch.ps1`
- `run_test_linux.sh`
- `run_test_windows.ps1`

## Setup

1. Put this patch folder anywhere you want.
2. Use your game install path as `GAME_ROOT` (folder containing `battlezone98redux.exe`).
3. Keep `battlezone98redux.exe.bak` available for rollback.

## Linux Test (Steam/Proton)

Do not patch EXE on disk on Linux. Use runtime patching.

```bash
./run_test_linux.sh "/path/to/Battlezone 98 Redux"
```

Or manual flow:

1. Launch game from Steam and wait at in-game main menu.
2. Run:

```bash
./runtime_patch_linux.sh "/path/to/Battlezone 98 Redux"
```

3. Host or join MP once.
4. Verify:

```bash
cd "/path/to/Battlezone 98 Redux"
VERIFY_RUNTIME_ONLY=1 /path/to/Battlezone\ Netcode\ Patch/verify_net_patch.sh
```

## Linux Auto Patch (Per Launch)

If you do not want to manually run the patch every launch, run the watcher once and keep it open.
It will detect each new `Battlezone98Redux.exe` process and patch it automatically.

```bash
bash ./auto_patch_linux_watch.sh "/path/to/Battlezone 98 Redux"
```

Notes:

- Keep this watcher terminal running while you play.
- Stop with `Ctrl+C`.
- If ptrace is blocked, apply the troubleshooting `ptrace_scope` command below first.
- Address detection is runtime/build-tolerant: it resolves patch locations from process memory, not fixed EXE offsets.
- If detection is ambiguous, patching aborts safely instead of guessing.

## Windows Test

Use runtime patching on Windows too.

Important order on Windows:

1. Launch game first.
2. Wait at in-game main menu.
3. Run runtime patch script (patches running process memory).
4. Enter multiplayer once.
5. Verify logs.

Permissions requirement on Windows:

- Run Steam as Administrator before launching the game.
- Run PowerShell as Administrator before running the runtime patch script.
- Keep elevation level consistent (both elevated) to avoid access denied when opening process memory.

Do not patch the EXE on disk first for the normal workflow. On-disk patching is fallback-only.

Run from PowerShell in the patch folder:

```powershell
.\run_test_windows.ps1 -GameRoot "C:\Path\To\Battlezone 98 Redux"
```

Manual flow:

0. Start Steam as Administrator.
1. Launch game and wait at in-game main menu.
2. Open PowerShell as Administrator.
3. Apply runtime patch to the running game process:

```powershell
.\runtime_patch_windows.ps1
```

4. Host/join MP once.
5. Verify in game folder:

```powershell
cd "C:\Path\To\Battlezone 98 Redux"
$env:VERIFY_RUNTIME_ONLY="1"
\path\to\Battlezone Netcode Patch\verify_net_patch.ps1
```

## Pass Criteria

Log contains:

`BZRNet P2P Socket Opened With 2097152 received buffer, 524288 send buffer`

## Rollback

Linux:

```bash
cp battlezone98redux.exe.bak battlezone98redux.exe
```

Windows:

```powershell
Copy-Item .\battlezone98redux.exe.bak .\battlezone98redux.exe -Force
```

## Troubleshooting

- Linux ptrace blocked:

```bash
sudo sysctl -w kernel.yama.ptrace_scope=0
pkill -9 r2 || true
pkill -f runtime_patch_linux.sh || true
```

- Windows Python launcher not found: use `python` instead of `py`.
- Windows access denied: run both Steam/game and PowerShell as Administrator.
- SHA mismatch: executable build differs from expected hash.
