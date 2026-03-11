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

### Steps

1. Build and deploy the Linux patch:

```bash
./Linux/deploy_linux.sh "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

Copy-paste example (common Steam path):

```bash
./Linux/deploy_linux.sh "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

If your path is different, open Steam, right-click Battlezone 98 Redux, then:
`Manage` -> `Browse local files` and copy that folder path.

2. In Steam launch options for Battlezone, set:

```text
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

3. Launch the game.
4. Enter multiplayer once.
5. Exit the game.

6. Verify:

```bash
cd "/path/to/Battlezone 98 Redux"
VERIFY_PROXY_READBACK=1 "/path/to/Battlezone Netcode Patch/Linux/verify_net_patch.sh"
```

Copy-paste verify example (common Steam path):

```bash
cd "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
VERIFY_PROXY_READBACK=1 "/path/to/Battlezone Netcode Patch/Linux/verify_net_patch.sh"
```

Optional guided flow:

```bash
./Linux/run_test_linux.sh "/path/to/Battlezone 98 Redux"
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
