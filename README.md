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

## Windows Test

Use runtime patching on Windows too (launch first, patch process memory, then test).

Run from PowerShell in the patch folder:

```powershell
.\run_test_windows.ps1 -GameRoot "C:\Path\To\Battlezone 98 Redux"
```

Manual flow:

1. Launch game and wait at in-game main menu.
2. Apply runtime patch:

```powershell
.\runtime_patch_windows.ps1
```

3. Host/join MP once.
4. Verify in game folder:

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
- SHA mismatch: executable build differs from expected hash.
