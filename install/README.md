# One-Line Install

These commands download the source from this repository, install missing build dependencies if needed, compile the patch locally on the machine that runs the command, and then install the resulting DLL into the Battlezone 98 Redux folder.

If dependencies are missing, the installer explains what it wants to install and why before asking for confirmation.

## Linux

Automatic path detection:

```bash
curl -fsSL https://github.com/PiercingXX/battlezone-netcode-testing/raw/main/install/install_linux.sh | bash
```

Explicit game path:

```bash
curl -fsSL https://github.com/PiercingXX/battlezone-netcode-testing/raw/main/install/install_linux.sh | bash -s -- --game-path "/path/to/Battlezone 98 Redux"
```

Linux installs the MinGW cross-compiler only if it is missing, then builds `dsound.dll` locally.

After install, set Steam launch options once:

```text
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

## Windows

Automatic path detection:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/PiercingXX/battlezone-netcode-testing/raw/main/install/install_windows.ps1 | iex"
```

Explicit game path:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "$env:BZNET_GAME_PATH='D:\Steam\steamapps\common\Battlezone 98 Redux'; irm https://github.com/PiercingXX/battlezone-netcode-testing/raw/main/install/install_windows.ps1 | iex"
```

Windows installs MSYS2 plus the MinGW 32-bit compiler only if they are missing, then builds `winmm.dll` locally. No Steam launch option changes are required.