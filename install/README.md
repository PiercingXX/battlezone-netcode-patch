# One-Line Install

Linux builds from source on your machine.
Windows installs the known-good prebuilt DLL with hash verification.

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

Windows installs the known-good `winmm.dll` from this repo and verifies SHA256 before deploy. No Steam launch option changes are required.