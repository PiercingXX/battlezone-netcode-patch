# Proton DSOUND Proxy

This directory contains the working startup-time `dsound.dll` proxy used for Battlezone 98 Redux on Proton.

## Why DSOUND.dll

The game imports `DSOUND.dll` very early, and in this build it only imports ordinal `1` from that DLL.

That makes `DSOUND.dll` a lower-risk proxy target than `WS2_32.dll`, which would require a full network stack proxy.

The goal is to get code execution inside the game process before the game reaches the startup socket initialization path.

## What This Proxy Does

On process attach, the proxy:

1. Installs an early `GetProcAddress` hook in the main module.
2. Resolves and patches Winsock imports for:
	- `setsockopt`
	- `WSASetSocketOption`
	- `getsockopt`
	- `WSAGetSocketOption`
	- `socket`
	- `closesocket`
3. Forces these target values when Battlezone configures socket buffers:
	- `SO_SNDBUF = 524288`
	- `SO_RCVBUF = 2097152`
4. Immediately reads the effective values back from the same socket handle.
5. Logs socket IDs, handles, force calls, readbacks, and close events to `dsound_proxy.log`.
6. Forwards ordinal `1` to the real system `dsound.dll` on demand.

## Important Behavioral Finding

In the validated Proton path:

- immediate proxy readback shows the target values are applied successfully
- `BZLogger.txt` still prints the old default startup buffer line

For this reason, `dsound_proxy.log` is the source of truth for verification.

## Build Requirements

You need a 32-bit MinGW cross-compiler because the game executable is 32-bit.

Expected toolchain:

- `i686-w64-mingw32-g++`
- `i686-w64-mingw32-gcc`
- `i686-w64-mingw32-dlltool`

On many Linux distributions this comes from a package like:

```bash
sudo apt install mingw-w64
```

## Build

From this directory:

```bash
make
```

Expected output:

- `build/dsound.dll`

## Deploy

From the repository root, the preferred path is:

```bash
./Linux/deploy_linux.sh "/path/to/Battlezone 98 Redux"
```

Manual copy is still:

```bash
cp build/dsound.dll "/home/piercingxx/.local/share/Steam/steamapps/common/Battlezone 98 Redux/dsound.dll"
```

## Steam Launch Options

Set the game's Steam launch options to:

```text
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

Meaning:

- `n` = prefer native DLL first
- `b` = fall back to builtin Wine or Proton implementation if needed

This causes Proton to load the local `dsound.dll` from the game directory first.

## Verify

Launch the game from Steam, enter multiplayer once, then verify:

```bash
cd "/home/piercingxx/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
VERIFY_PROXY_READBACK=1 "/media/Working-Storage/GitHub/Battlezone Netcode Patch/Linux/verify_net_patch.sh"
```

Successful verification may pass via proxy readback even if the Battlezone startup socket text line remains unchanged.

## Current Limitations

- The implementation here is Proton-specific because it depends on Wine loading a local native `dsound.dll`.
- Windows uses a separate implementation in `Microslop/winmm.dll` and `Microslop/winmm_proxy/`.

## Current Result

This proxy successfully reaches the startup socket path and forces the target values on the intercepted socket.

Linux and Windows now ship as separate startup-interception paths with matching socket buffer targets.