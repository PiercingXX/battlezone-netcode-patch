# Battlezone Netcode Testing

## Netcode, But Less Embarrassing

Battlezone drops out-of-order UDP packets like they're invalid by moral principle.

That is fine on perfect links and garbage on real ones.

This repo ships the proxy patch that fixes that behavior.

No game binary edits. No ritual config voodoo. Just a DLL proxy that intercepts recv path traffic, reorders short-window out-of-order packets, and hands clean sequence flow back to the game.

---

## What We Shipped (V1 -> V3)

### V1 (Patch 00)
- Forced bigger UDP socket buffers
- `SO_SNDBUF = 512 KB`
- `SO_RCVBUF = 2 MB`
- Immediate gain: better burst tolerance

### V2
- Forced even bigger UDP socket buffers
- SO_SNDBUF = 512 KB
- SO_RCVBUF = 4 MB
- Hardened hooks and improved deployment consistency

### V3 (Current)
- Added in-proxy out-of-order packet reordering (`WSARecvFrom` path)
- Per-peer buffering with deterministic sequence release
- Final tuned profile:
- Reorder window `45 ms`
- Drain budget `96`
- Per-peer depth `8`
- Peer cap `32`
- Sequence location `payload[13..16]` (`u32le`)
- Linux and Windows now have matching behavior

---

## Quick Start

### Linux / Proton

1. Install build tools.

Debian/Ubuntu:
```bash
sudo apt install mingw-w64 make
```

Arch/Manjaro:
```bash
sudo pacman -S mingw-w64-gcc make
```

2. Deploy proxy to your Battlezone install.

Native Steam path:
```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
./Linux/deploy_linux.sh "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

Snap Steam path:
```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
./Linux/deploy_linux.sh "/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

Flatpak Steam path:
```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
./Linux/deploy_linux.sh "/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
```

3. Steam launch options:

```text
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

### Windows

1. No-build option (recommended): use the prebuilt DLL in this repo:

```text
prebuilt/windows/winmm.dll
```

Optional integrity check:

```bash
cd "Battlezone Netcode Testing/prebuilt/windows"
sha256sum -c winmm.dll.sha256
```

2. Copy `winmm.dll` to your game folder:

```text
C:\Program Files (x86)\Steam\steamapps\common\Battlezone 98 Redux\
```

3. Launch normally.

If you want to build it yourself instead:

```bash
cd "$HOME/Downloads/Battlezone Netcode Testing"
cd Microslop/winmm_proxy && make
```

For full Windows-specific notes, see [Microslop/winmm_proxy/README.md](Microslop/winmm_proxy/README.md).

---

## Optional Logging (Linux)

If you want hard data instead of vibes:

```bash
./buffer-logging/buffer_logger_linux.sh start "/path/to/Battlezone 98 Redux" 32 65536
# Play session
./buffer-logging/buffer_logger_linux.sh stop
```

Details: [logging_readme.md](logging_readme.md)

---

## Known Limits

- Windows packet logging is not wired yet
- Primary UDP path is hooked (matches BZ behavior)
- This fixes out-of-order handling, not every form of packet loss physics

---

## More Technical Docs

- [Linux/proton_dsound_proxy/README.md](Linux/proton_dsound_proxy/README.md)
- [Microslop/winmm_proxy/README.md](Microslop/winmm_proxy/README.md)
- [resources/INVESTIGATION_WRITEUP.md](resources/INVESTIGATION_WRITEUP.md)
