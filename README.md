---

## Quick Start


### Windows 🪟

Step 1: paste this into PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://github.com/PiercingXX/battlezone-netcode-testing/raw/main/install/install_windows.ps1 | iex"
```

Windows does not need any Steam launch option changes, because for once Windows is the less annoying one here. 🙃



--- 


### Linux / Proton 🐧

Step 1: paste this into terminal:

```bash
curl -fsSL https://github.com/PiercingXX/battlezone-netcode-testing/raw/main/install/install_linux.sh | bash
```

Step 2: set this in Steam launch options:

```text
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```



---





### The Actual Problem
Battlezone's netcode is brutally unforgiving: it drops any UDP packet that doesn't arrive in *exact* sequential order, even by milliseconds. WiFi? Wireless? International? Anything with even mild jitter? You're not losing packets to the network - you're losing them to a rigid sequencing requirement that tolerates zero deviation.

It is now substantially less dumb.

### The "Solution": Out-of-Order Packet Reordering
I built a packet reordering engine that intercepts wayward packets mid-flight, buffers them for up to 45ms, then releases them once their predecessors arrive. Think traffic control, not packet panic.

**The result:** ~4-5 fewer drops per minute on typical connections. On WiFi or high-latency links: better hit registration, smoother movement, and fewer "how did that even happen" moments...yes there are still drops, I said 'fewer'.

The patch runs entirely in userspace via DLL proxy injection. The game never knows it's there.




















---


## What Was Actually Shipped (V1 -> V3)

### Version 1 (Patch 00)
- Forced bigger UDP socket buffers
- `SO_SNDBUF = 512 KB`
- `SO_RCVBUF = 2 MB`
- Result: better burst tolerance, fewer immediate choke events

### Version 2
- Forced even bigger UDP socket buffers
- `SO_SNDBUF = 512 KB`
- `SO_RCVBUF = 4 MB`
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











































### Want To Do It Manually Instead? 🔧

Manual build and deployment steps are still listed below if you want full control or just do not trust automation. Fair.

### Linux / Proton

Manual install path:

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
cd "$HOME/Downloads/battlezone-netcode-testing-main"
./Linux/deploy_linux.sh "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

Snap Steam path:
```bash
cd "$HOME/Downloads/battlezone-netcode-testing-main"
./Linux/deploy_linux.sh "/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

Flatpak Steam path:
```bash
cd "$HOME/Downloads/battlezone-netcode-testing-main"
./Linux/deploy_linux.sh "/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
```

3. Steam launch options:

```text
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

### Windows

Manual install path:

Windows Defender note (important):

- Some users report Defender quarantining winmm.dll from GitHub source ZIP downloads as Program:Win32/Contebrew.A!ml.
- This is a heuristic/PUA-style detection commonly triggered by unsigned DLL proxy/hook binaries.
- If Defender quarantines the file, do not disable antivirus globally. See remediation below.

1. Build-it-yourself option (recommended for source ZIP users):

```bash
cd "$HOME/Downloads/battlezone-netcode-testing-main"
cd Microslop/winmm_proxy && make
```

Then copy:

```text
Microslop/winmm_proxy/build/winmm.dll
```

to your game folder:

```text
C:\Program Files (x86)\Steam\steamapps\common\Battlezone 98 Redux\
```

2. Prebuilt option: use the prebuilt DLL in this repo if your AV policy allows it:

```text
prebuilt/windows/winmm.dll
```

Optional integrity check:

```bash
cd "$HOME/Downloads/battlezone-netcode-testing-main/prebuilt/windows"
sha256sum -c winmm.dll.sha256
```

3. Copy winmm.dll to your game folder:

```text
C:\Program Files (x86)\Steam\steamapps\common\Battlezone 98 Redux\
```

4. Launch normally.

If Defender quarantines the DLL:

- Open Windows Security -> Protection history.
- Confirm affected item path and detection name.
- If hash matches your expected build, restore the file and add a narrow exception for that one game-folder DLL only.
- Submit a false-positive report to Microsoft with the DLL and detection details.
- Prefer signed release artifacts when available.

Maintainer release guidance (recommended):

- Avoid shipping unsigned DLL binaries inside the source ZIP path when possible.
- Prefer GitHub Releases assets with SHA256 + Authenticode signature.

If you want to build from source:

```bash
cd "$HOME/Downloads/battlezone-netcode-testing-main"
cd Microslop/winmm_proxy && make
```

For full Windows-specific notes, see [Microslop/winmm_proxy/README.md](Microslop/winmm_proxy/README.md).

---

## Optional Logging

If you want hard data instead of vibes:

Windows:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
& "$HOME\Downloads\battlezone-netcode-testing-main\buffer-logging\buffer_logger_windows.ps1" -Action Start
# Play session
& "$HOME\Downloads\battlezone-netcode-testing-main\buffer-logging\buffer_logger_windows.ps1" -Action Stop
```

Linux:

```bash
./buffer-logging/buffer_logger_linux.sh start "/path/to/Battlezone 98 Redux" 32 65536
# Play session
./buffer-logging/buffer_logger_linux.sh stop
```

Details: [logging_readme.md](logging_readme.md)

---

## Known Limits

- Primary UDP path is hooked (matches BZ behavior)
- This fixes out-of-order handling, not every form of packet loss physics

---

## More Technical Docs

- [Linux/proton_dsound_proxy/README.md](Linux/proton_dsound_proxy/README.md)
- [Microslop/winmm_proxy/README.md](Microslop/winmm_proxy/README.md)
- [resources/INVESTIGATION_WRITEUP.md](resources/INVESTIGATION_WRITEUP.md)
