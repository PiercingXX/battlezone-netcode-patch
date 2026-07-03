# Battlezone 98 Redux Netcode Patch

Battlezone's netcode drops any UDP packet that doesn't arrive in *exact* sequential order, even by milliseconds. WiFi? Wireless? International? Anything with even mild jitter? You're not losing packets to the network - you're losing them to a rigid sequencing requirement that tolerates zero deviation.

This patch intercepts wayward packets mid-flight, buffers them briefly, and releases them in order. The game never knows it's there.

**Measured result (live A/B, same map, same opponent): 121 drops → 40 drops per match. ~65% fewer out-of-order drops.**

---

## Quick Start

**Everyone in the lobby should install this** — the fix is receive-side, so each install only protects that player's own inbound packets. See [Who Should Install It?](#who-should-install-it) below.

### Windows 🪟

Paste into PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_windows.ps1 | iex"
```

Auto-detects your install (registry + Steam library folders), downloads the prebuilt `winmm.dll` (SHA256-verified), and installs the tuned `net.ini` as a local mod. No launch option changes needed.

### Linux / Proton 🐧

Step 1 — paste into terminal:

```bash
curl -fsSL https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_linux.sh | bash
```

The installer will:

- auto-detect your install (native, Snap, or Flatpak Steam)
- ask before installing build dependencies (MinGW cross-compiler, make) via apt, pacman, or dnf
- build `dsound.dll` locally from source and install it
- offer to raise the kernel UDP buffer limits — without this, the kernel silently clamps the enlarged socket buffers to ~208 KB
- install the tuned `net.ini` as a local mod
- run the Linux EXU compatibility repair (best effort)

Non-interactive? Set `BZNET_ASSUME_YES=1`.

Step 2 — set Steam launch options:

```text
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

### Who Should Install It?

Everyone playing, ideally. Battlezone drops out-of-order packets **at the receiver**, and this patch sits in front of *your* copy of the game — it un-scrambles packets arriving *to you* and does nothing for packets you send to others. If only you install it, enemies look smooth on your screen while you still look laggy on theirs.

| Component | Who benefits from *your* install |
|---|---|
| Reorder buffer | Only you (your inbound from every peer) |
| Bigger socket buffers | Only you (your burst tolerance) |
| `BZ_SEND_DUP=1` (opt-in) | The **other** players — protects your outbound against loss, works even if they're unpatched |
| Tuned net.ini | Your own send governor (it runs on every machine, not just the host's) |

Playing with someone who can't or won't install? Enable `BZ_SEND_DUP=1` on your side — it's the only piece that helps an unpatched peer.

---

## How It Works

- **Adaptive per-peer reorder window:** starts at a 5 ms floor so clean connections pay near-zero added latency, grows toward a 100 ms ceiling only when reordering is actually observed on that link, and decays back down when the link is clean. The 100 ms ceiling is what produced the measured 65% drop reduction (the old 45 ms default left too many near-misses on the table).
- **Wake thread:** the reorder hook drains the kernel socket, so a game sleeping in `select()` could leave held packets stranded. A background thread nudges the socket readable so held packets release on time.
- **4 MB receive / 512 KB send socket buffers**, forced at socket creation for burst tolerance.
- **Opt-in loss redundancy (`BZ_SEND_DUP=1`):** sends every outbound P2P datagram twice via the `WSASendTo` hook. Reordering can't recover a packet the network actually dropped; a duplicate can. Receivers dedup it whether patched or vanilla. Costs 2x upstream — for genuinely lossy links only.
- Everything runs in userspace via DLL proxy injection (`dsound.dll` on Proton, `winmm.dll` on Windows). Same code, same tuning env vars (`BZ_REORDER_*`, `BZ_SEND_DUP`) on both platforms — see the proxy READMEs.

## What We Learned About the Game (the hard way)

Verified in live testing, because the community wisdom was mostly stale:

- **net.ini only loads through the mod system.** A copy next to `battlezone98redux.exe` is silently ignored. The installers deliver it as a local mod at `packaged_mods/9990001/net.ini`, which the game provably loads on both platforms.
- **Disabling a workshop mod in-game does NOT stop its net.ini loading.** Only unsubscribing does. If you're subscribed to the "Auto-Kick Reduction Patch" (workshop `1895622040`), it overrides the local mod — and it caps your send rate at 32 KB/s.
- **The send-rate governor runs on every machine**, not just the host's. And it always starts at a hardcoded 4,000 bytes/s, ramping up slowly — `MinBandwidth` does not set the starting rate, and in short matches the `MaxBandwidth` ceiling is never reached.
- The game exe imports classic winsock functions **by ordinal** and sends all P2P traffic via `WSASendTo` (`sendto` isn't in its import table at all). On real Windows its receives are overlapped/IOCP; under Proton they aren't.

## Version History

- **V1–V2:** forced bigger UDP socket buffers (final: 512 KB send / 4 MB receive). Better burst tolerance.
- **V3:** in-proxy out-of-order packet reordering (`WSARecvFrom` hook), per-peer buffering with deterministic sequence release. Sequence field located at `payload[13..16]` (u32le) via binary capture analysis.
- **V4:** adaptive reorder window (5 ms floor), wake thread for stranded packets, Linux kernel-clamp fix in the installer, Windows/Linux tuning parity, `BZ_SEND_DUP`, drop metrics in the verify script.
- **V4.1:** **Windows launch-freeze hotfix** — the hook was routing the game's overlapped (IOCP) receives through the synchronous reorder path, hanging the game at the loading screen (Proton was unaffected; the bug existed since V3). Also: ordinal IAT patching, and `BZ_SEND_DUP` moved to the `WSASendTo` hook where it actually works.
- **V4.2 (current):** reorder ceiling default raised 45 → 100 ms after live A/B testing measured ~65% fewer drops (121 → 40/43 on the same map/opponent); harmless on clean links thanks to the adaptive floor. net.ini now installs as a local packaged mod, since the game-root location turned out to be dead.

---

## Verify It Worked ✅

Launch the game once (start or join a multiplayer session), quit, then:

**Linux** — run from the game folder:

```bash
cd "/path/to/Battlezone 98 Redux"
/path/to/battlezone-netcode-patch-master/Linux/verify_net_patch.sh
```

Expect `VERIFY RESULT: PASS` plus a per-session drop count (`Game-side packet drops this session: ...`) you can compare across matches.

**Windows:**

```powershell
.\Microslop\verify_windows.ps1
```

Quick manual checks in any log:

- Proxy log (`dsound_proxy.log` / `winmm_proxy.log`): `reorder: enabled max_window_ms=100 ...` and `effective readback SO_SNDBUF=524288 ... SO_RCVBUF=4194304`
- `BZLogger.txt`: `MOD FOUND net.ini at ...packaged_mods\9990001` confirms the net.ini mod loaded

---

## Want To Do It Manually Instead? 🔧

These steps assume the source archive is extracted to `~/Downloads/battlezone-netcode-patch-master`.

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

2. Raise the kernel UDP buffer limits (the deploy script warns but does not apply this):

```bash
sudo sysctl -w net.core.rmem_max=4194304 net.core.wmem_max=524288
printf 'net.core.rmem_max=4194304\nnet.core.wmem_max=524288\n' | sudo tee /etc/sysctl.d/99-battlezone-netcode.conf
```

3. Deploy to your Battlezone install (builds from source, installs the DLL and net.ini mod, runs EXU repair):

```bash
cd "$HOME/Downloads/battlezone-netcode-patch-master"
./Linux/deploy_linux.sh "/path/to/steamapps/common/Battlezone 98 Redux"
```

Common game paths: native `~/.local/share/Steam/steamapps/common/...`, Snap `~/snap/steam/common/.local/share/Steam/steamapps/common/...`, Flatpak `~/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/...`

4. Steam launch options:

```text
WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
```

### Windows

**Defender note:** some users report Defender quarantining `winmm.dll` from source ZIP downloads as `Program:Win32/Contebrew.A!ml` — a heuristic detection common for unsigned DLL proxies. If it triggers: restore from Protection history, add a narrow exception for that one game-folder DLL only, and report the false positive. Don't disable AV globally.

1. Recommended: use the prebuilt DLL (`prebuilt/windows/winmm.dll`; verify with `sha256sum -c winmm.dll.sha256`).
2. Or build it yourself (advanced): `cd Microslop/winmm_proxy && make` → `build/winmm.dll`.
3. Copy `winmm.dll` to the game folder (`...\steamapps\common\Battlezone 98 Redux\`).
4. Optionally copy `net-ini/net.ini` to `...\Battlezone 98 Redux\packaged_mods\9990001\net.ini`.
5. Launch normally — no launch option changes on Windows.

Full Windows notes: [Microslop/winmm_proxy/README.md](Microslop/winmm_proxy/README.md)

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

- Fixes out-of-order handling; true packet loss is only mitigated by the opt-in `BZ_SEND_DUP` redundancy.
- The game's send governor always ramps from a hardcoded 4 KB/s at match start — net.ini can't change the starting rate, and short matches never reach any bandwidth ceiling. Start-of-match traffic bursts remain the main unsolved drop source.
- If you're subscribed to a workshop mod that ships net.ini, it overrides the patch's tuned copy — unsubscribe (disabling in-game is not enough).

---

## More Technical Docs

- [Linux/proton_dsound_proxy/README.md](Linux/proton_dsound_proxy/README.md)
- [Microslop/winmm_proxy/README.md](Microslop/winmm_proxy/README.md)
- [net-ini/README.md](net-ini/README.md)
- [resources/INVESTIGATION_WRITEUP.md](resources/INVESTIGATION_WRITEUP.md)
