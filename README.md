## Quick Start

**Everyone in the lobby should install this** — the fix is receive-side, so each install only protects that player's own inbound packets. See [Who Should Install It?](#who-should-install-it) below.

---

### Windows 🪟

Step 1: paste this into PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_windows.ps1 | iex"
```

The installer auto-detects your Battlezone 98 Redux install (registry + Steam library folders), downloads the known-good prebuilt `winmm.dll`, and verifies its SHA256 hash before deployment.

Windows does not need any Steam launch option changes.

---

### Linux / Proton 🐧

Step 1: paste this into terminal:

```bash
curl -fsSL https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_linux.sh | bash
```

The installer will:

- auto-detect your Battlezone 98 Redux install (native, Snap, or Flatpak Steam)
- ask before installing build dependencies (MinGW cross-compiler, make) via apt, pacman, or dnf
- build `dsound.dll` locally from source and install it into the game folder
- offer to raise the kernel UDP buffer limits (`net.core.rmem_max` / `net.core.wmem_max`) — without this, the kernel silently clamps the enlarged socket buffers to ~208 KB and most of the buffer patch does nothing
- run the Linux EXU compatibility repair (best effort)

Running non-interactively? Set `BZNET_ASSUME_YES=1` to skip the confirmation prompts.

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

### Who Should Install It?

Everyone playing, ideally. Battlezone drops out-of-order packets **at the receiver**, and this patch sits in front of *your* copy of the game — it un-scrambles packets arriving *to you* and does nothing for packets you send to others. If only you install it, enemies look smooth on your screen while you still look laggy on theirs.

| Component | Who benefits from *your* install |
|---|---|
| Reorder buffer | Only you (your inbound from every peer) |
| Bigger socket buffers | Only you (your burst tolerance) |
| `BZ_SEND_DUP=1` (opt-in) | The **other** players — protects your outbound against loss, works even if they're unpatched |
| Tuned net.ini | Everyone in the game, but only if you're the host |

Playing with someone who can't or won't install? Enable `BZ_SEND_DUP=1` on your side — it's the only piece that helps an unpatched peer.

---

## What Was Actually Shipped (V1 -> V4)

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

### V3
- Added in-proxy out-of-order packet reordering (`WSARecvFrom` path)
- Per-peer buffering with deterministic sequence release
- Tuned profile:
- Reorder window `45 ms`
- Drain budget `96`
- Per-peer depth `8`
- Peer cap `32`
- Sequence location `payload[13..16]` (`u32le`)
- Linux and Windows now have matching behavior

### V4 (Current)
- **Adaptive reorder window:** the fixed 45 ms hold added latency to every
  gap, even on clean connections. The window is now per-peer: it starts at a
  5 ms floor, grows toward the 45 ms ceiling only when reordering is actually
  observed on that link, and decays back down when the link is clean.
  Clean connections now pay near-zero added latency.
- **Stranded-packet fix (wake thread):** the reorder hook drains the kernel
  socket, so a game sleeping in `select()` could leave held packets stuck far
  past the window until the next real packet arrived. A background thread now
  nudges the socket readable so held packets release on time.
- **Linux kernel clamp fix:** the installer raises
  `net.core.rmem_max`/`net.core.wmem_max` (with consent) — previously the
  kernel silently clamped the 4 MB receive buffer to ~208 KB on most distros.
- **Tuning parity:** Windows now honors the same `BZ_REORDER_*` env vars as
  Linux (`BZ_REORDER`, `BZ_REORDER_WINDOW_MS`, `BZ_REORDER_MIN_MS`,
  `BZ_REORDER_ADAPT`, `BZ_REORDER_WAKE`, `BZ_REORDER_DEPTH`,
  `BZ_REORDER_PEERS`, `BZ_REORDER_DRAIN`). See the proxy READMEs.
- **Opt-in loss redundancy (`BZ_SEND_DUP=1`):** sends every outbound P2P
  datagram twice. Reordering can't recover a packet the network actually
  dropped; a duplicate can. Receivers dedup it whether patched or vanilla.
  Costs 2x upstream bandwidth — for genuinely lossy links only.
- **Host-side net.ini tuning:** the game's send-rate governor
  (`MaxPing`/`MaxBandwidth`/`MinBandwidth`) can collapse throughput on
  jittery links — a problem no receive-side proxy can fix. A tuned
  [net.ini](net-ini/README.md) ships with the repo; it only takes effect on
  the hosting machine.
- **Drop metric in verify:** `Linux/verify_net_patch.sh` now prints the
  game-side packet drop counts (total and out-of-order) for the latest
  session, so patch changes can be measured instead of vibed.

---

## Verify It Worked ✅

Launch the game once (start or join a multiplayer session), quit, then:

Linux — run from the game folder:

```bash
cd "/path/to/Battlezone 98 Redux"
/path/to/battlezone-netcode-patch-master/Linux/verify_net_patch.sh
```

It checks `BZLogger.txt`, the `dsound_proxy.log` effective-buffer readback, and warns if the kernel limits are still below the patch targets. Expect `VERIFY RESULT: PASS`.

Windows:

```powershell
.\Microslop\verify_windows.ps1
```

Auto-detects the game path (or pass `-GamePath "D:\...\Battlezone 98 Redux"`) and confirms the `winmm.dll` proxy deployed and the socket buffer hook fired.

You can also just look at the proxy log in the game folder (`dsound_proxy.log` on Linux, `winmm_proxy.log` on Windows) for a `reorder: enabled` line and `effective readback SO_SNDBUF=524288 ... SO_RCVBUF=4194304`.

---

### Want To Do It Manually Instead? 🔧

Manual build and deployment steps are still listed below if you want full control or just do not trust automation. Fair.

These steps assume you downloaded and extracted the source archive to `~/Downloads/battlezone-netcode-patch-master`.

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

2. Raise the kernel UDP buffer limits (the deploy script warns about this but does not apply it):

```bash
sudo sysctl -w net.core.rmem_max=4194304 net.core.wmem_max=524288
printf 'net.core.rmem_max=4194304\nnet.core.wmem_max=524288\n' | sudo tee /etc/sysctl.d/99-battlezone-netcode.conf
```

3. Deploy proxy to your Battlezone install.

Native Steam path:
```bash
cd "$HOME/Downloads/battlezone-netcode-patch-master"
./Linux/deploy_linux.sh "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

Snap Steam path:
```bash
cd "$HOME/Downloads/battlezone-netcode-patch-master"
./Linux/deploy_linux.sh "/home/$USER/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
```

Flatpak Steam path:
```bash
cd "$HOME/Downloads/battlezone-netcode-patch-master"
./Linux/deploy_linux.sh "/home/$USER/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
```

`deploy_linux.sh` builds the proxy from source, installs it, and also runs Linux EXU compatibility repair automatically (best effort).

4. Steam launch options:

```text
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

### Windows

Manual install path:

Windows Defender note (important):

- Some users report Defender quarantining winmm.dll from GitHub source ZIP downloads as Program:Win32/Contebrew.A!ml.
- This is a heuristic/PUA-style detection commonly triggered by unsigned DLL proxy/hook binaries.
- If Defender quarantines the file, do not disable antivirus globally. See remediation below.

1. Recommended option: use the prebuilt DLL in this repo if your AV policy allows it:

```text
prebuilt/windows/winmm.dll
```

Optional integrity check:

```bash
cd "$HOME/Downloads/battlezone-netcode-patch-master/prebuilt/windows"
sha256sum -c winmm.dll.sha256
```

2. Build-it-yourself option (advanced/troubleshooting only; some local Windows builds have been reported to crash in-game):

```bash
cd "$HOME/Downloads/battlezone-netcode-patch-master"
cd Microslop/winmm_proxy && make
```

The built DLL lands in `Microslop/winmm_proxy/build/winmm.dll`.

3. Copy winmm.dll to your game folder:

```text
C:\Program Files (x86)\Steam\steamapps\common\Battlezone 98 Redux\
```

4. Launch normally. No Steam launch option changes are needed on Windows.

If Defender quarantines the DLL:

- Open Windows Security -> Protection history.
- Confirm affected item path and detection name.
- If hash matches your expected build, restore the file and add a narrow exception for that one game-folder DLL only.
- Submit a false-positive report to Microsoft with the DLL and detection details.
- Prefer signed release artifacts when available.

Maintainer release guidance (recommended):

- Avoid shipping unsigned DLL binaries inside the source ZIP path when possible.
- Prefer GitHub Releases assets with SHA256 + Authenticode signature.

For full Windows-specific notes, see [Microslop/winmm_proxy/README.md](Microslop/winmm_proxy/README.md).

---

## Optional Logging

If you want hard data instead of vibes:

Windows:

```powershell
Set-ExecutionPolicy -Scope Process Bypass -Force
& "$HOME\Downloads\battlezone-netcode-patch-master\buffer-logging\buffer_logger_windows.ps1" -Action Start
# Play session
& "$HOME\Downloads\battlezone-netcode-patch-master\buffer-logging\buffer_logger_windows.ps1" -Action Stop
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

## Hosting a Game?

The proxy fixes the receive side. The send side has its own failure mode:
the game's bandwidth governor cuts your send rate every time ping crosses
`MaxPing` and caps it at `MaxBandwidth` — with stock values one jitter spike
can collapse your outbound rate and the lag feeds itself. Copy
[net-ini/net.ini](net-ini/net.ini) into your game folder before hosting; see
[net-ini/README.md](net-ini/README.md) for the workshop-mod precedence
gotcha.

---

## More Technical Docs

- [Linux/proton_dsound_proxy/README.md](Linux/proton_dsound_proxy/README.md)
- [Microslop/winmm_proxy/README.md](Microslop/winmm_proxy/README.md)
- [net-ini/README.md](net-ini/README.md)
- [resources/INVESTIGATION_WRITEUP.md](resources/INVESTIGATION_WRITEUP.md)
