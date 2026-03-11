# Battlezone Netcode Patch - Tester Guide

This repo helps test larger multiplayer socket buffers for Battlezone 98 Redux.

Target values:

- Send buffer: 524288
- Receive buffer: 2097152

## Linux (Proton)

### What You Need

- Linux with Steam and Proton working
- Battlezone 98 Redux installed
- `make`
- `i686-w64-mingw32-g++`

If tools are missing on Debian or Ubuntu:

```bash
sudo apt install mingw-w64 make
```

### Step 1: Find Your Game Path

**This is the most important step.** The path varies depending on your Steam installation type, and guessing will fail.

1. Open Steam
2. Right-click "Battlezone 98 Redux"
3. Select `Manage` → `Browse local files`
4. A file manager window will open showing your game folder
5. Copy the full folder path from the address bar or `pwd` in terminal

**Common paths by Steam type:**

- **Native Steam**: `/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux`
- **Snap Steam**: Varies; use "Browse local files" to find it
- **Flatpak Steam**: Varies; use "Browse local files" to find it

### Step 2: Verify the Path

Before deploying, verify your path is correct by checking for the game executable:

```bash
ls -la "/full/path/to/Battlezone 98 Redux/battlezone98redux.exe"
```

If this shows the file exists, your path is correct. If "No such file or directory", go back to Step 1 and get the correct path.

### Step 3: Deploy the Patch

```bash
./Linux/deploy_linux.sh "/full/path/to/Battlezone 98 Redux"
```

Replace `/full/path/to/Battlezone 98 Redux` with the path you found in Step 1.

### Step 4: Set Steam Launch Options

1. Open Steam
2. Right-click Battlezone 98 Redux
3. Select `Properties`
4. In "General" tab, find "Launch Options"
5. Paste this:

```text
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

### Step 5: Test

1. Launch the game
2. Enter multiplayer once
3. Exit the game

### Step 6: Verify Success

```bash
cd "/full/path/to/Battlezone 98 Redux"
VERIFY_PROXY_READBACK=1 "/path/to/Battlezone Netcode Patch/Linux/verify_net_patch.sh"
```

Or use the guided flow:

```bash
./Linux/run_test_linux.sh "/full/path/to/Battlezone 98 Redux"
```

### Linux Success / Failure

- Success: `VERIFY RESULT: PASS`
- Main log to trust: `dsound_proxy.log`
- If no `dsound_proxy.log`: launch options are wrong, or `dsound.dll` was not deployed.

## Windows

### What You Need

- Windows with Steam and Battlezone 98 Redux installed
- Included ready-to-copy DLL: `Microslop/winmm.dll`

### Steps

1. Copy `Microslop/winmm.dll` to:

```text
C:\Program Files (x86)\Steam\steamapps\common\Battlezone 98 Redux\winmm.dll
```

If your Steam library is on another drive, use Steam -> right-click Battlezone 98 Redux -> `Manage` -> `Browse local files`, then copy to that folder as `winmm.dll`.

2. Launch the game.
3. Enter multiplayer once.
4. Exit the game.

5. Verify:

```powershell
.\Microslop\verify_windows.ps1
```

Optional deploy script:

```powershell
.\Microslop\deploy_windows.ps1
```

### Windows Success / Failure

- Success: `RESULT: PASS`
- Main log to trust: `winmm_proxy.log`
- If no `winmm_proxy.log`: `winmm.dll` is not in the game folder.

## Important Note

The Battlezone startup text line can still show old values even when the patch is working.
Use proxy log readback (`dsound_proxy.log` or `winmm_proxy.log`) as source of truth.

## Technical Details

- Full investigation history: `INVESTIGATION_WRITEUP.md`
