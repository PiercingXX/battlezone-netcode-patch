# Battlezone 98 Redux Netcode Patch

## *** V4.3 Needs Testing!!! ***

> ### 🧪 Help us test the reworked redundancy + QoS
> The core reorder patch is stable and on by default — **you don't need to change anything to benefit.** What we're testing in V4.3:
>
> - **Smarter packet duplication (`BZ_SEND_DUP`, opt-in, off by default).** A seven-game series showed the old "send every packet twice" was a *net loss* on busy/WiFi links — it doubles packets-per-second right when the queue is filling. V4.3 makes it delayed + rate-capped + loopback-skipping instead. We need data on whether the new version actually helps.
> - **DSCP priority marking (`BZ_DSCP`, on by default at EF/46).** Marks game packets so a WMM/SQM router serves them ahead of bulk downloads. Real effect under Proton; harmless no-op on stock Windows.
>
> To try duplication: **Linux** add `BZ_SEND_DUP=1` to the launch options; **Windows** run `setx BZ_SEND_DUP 1` and fully restart Steam. Confirm `send_dup=enabled` in the proxy log.
>
> After games, send logs (`BZLogger.txt` + `dsound_proxy.log`/`winmm_proxy.log`) as **file attachments** — chat pastes truncate.

---

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

Auto-detects your install (registry + Steam library folders), downloads the prebuilt `winmm.dll` (SHA256-verified), and installs the tuned `net.ini` as a local mod. No launch option changes needed. To help test opt-in duplication, additionally run `setx BZ_SEND_DUP 1` and restart Steam (see the note at the top).

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
WINEDLLOVERRIDES=dsound=n,b %command% -nointro
```

That's all you need — the reorder patch, bigger buffers, and DSCP marking are on by default. To help test opt-in duplication, add `BZ_SEND_DUP=1` before `%command%` (see the note at the top).

### Who Should Install It?

Everyone playing, ideally. Battlezone drops out-of-order packets **at the receiver**, and this patch sits in front of *your* copy of the game — it un-scrambles packets arriving *to you* and does nothing for packets you send to others. If only you install it, enemies look smooth on your screen while you still look laggy on theirs.

| Component | Who benefits from *your* install |
|---|---|
| Reorder buffer | Only you (your inbound from every peer) |
| Bigger socket buffers | Only you (your burst tolerance) |
| DSCP marking | Only you (your outbound gets router priority, if your router honours it) |
| `BZ_SEND_DUP=1` (opt-in) | The **other** players — adds loss redundancy to your outbound, works even if they're unpatched |
| Tuned net.ini | Your own send governor (it runs on every machine, not just the host's) |

Playing with someone who can't or won't install? `BZ_SEND_DUP=1` on your side is the only piece that helps an unpatched peer — but it's opt-in and best on links with spare upload headroom (see the testing note above).

---

## How It Works

- **Adaptive per-peer reorder window:** starts at a 5 ms floor so clean connections pay near-zero added latency, grows toward a 100 ms ceiling only when reordering is actually observed on that link, and decays back down when the link is clean. The 100 ms ceiling is what produced the measured 65% drop reduction (the old 45 ms default left too many near-misses on the table).
- **Wake thread:** the reorder hook drains the kernel socket, so a game sleeping in `select()` could leave held packets stranded. A background thread nudges the socket readable so held packets release on time.
- **4 MB receive / 512 KB send socket buffers**, forced at socket creation for burst tolerance — and re-forced through a `setsockopt` hook on both platforms, so the game can't shrink the send buffer back to 32 KB (it tries to, on real Windows).
- **DSCP priority marking (`BZ_DSCP`, default 46 = EF):** tags the P2P socket so routers running WMM (WiFi voice queue) or SQM/fq_codel serve game packets ahead of bulk traffic — targeting queueing delay, which live testing showed is the real lag source under load. Effective under Proton; a no-op on stock Windows (which needs qWAVE or a router rule). `BZ_DSCP=0` disables.
- **Opt-in loss redundancy (`BZ_SEND_DUP=1`):** re-sends outbound P2P datagrams via the `WSASendTo` hook so a genuinely *lost* packet (not just reordered) can still arrive. V4.3 sends the copy **time-shifted** (`BZ_DUP_DELAY_MS`, default 25 ms) and **rate-capped** (`BZ_DUP_MAX_PPS`, default 40), and never duplicates the game's loopback self-connection — because naive back-to-back duplication was measured to *hurt* busy links by doubling packet rate mid-congestion. Receivers dedup it whether patched or vanilla. Best on links with spare upload headroom.
- Everything runs in userspace via DLL proxy injection (`dsound.dll` on Proton, `winmm.dll` on Windows). Same code, same tuning env vars (`BZ_REORDER_*`, `BZ_SEND_DUP`, `BZ_DUP_*`, `BZ_DSCP`) on both platforms — see the proxy READMEs.

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
- **V4.2:** reorder ceiling default raised 45 → 100 ms after live A/B testing measured ~65% fewer drops (121 → 40/43 on the same map/opponent); harmless on clean links thanks to the adaptive floor. net.ini now installs as a local packaged mod.
- **V4.3 (current):** overhauled `BZ_SEND_DUP` after a seven-game test series showed naive back-to-back duplication *degrades* constrained uplinks (it doubles packets-per-second exactly when queues are forming). Duplication now skips the game's loopback self-connection, time-shifts the copy (`BZ_DUP_DELAY_MS`, default 25 ms — one queue spike can't kill both copies), and rate-caps duplicates (`BZ_DUP_MAX_PPS`, default 40). New `BZ_DSCP` marks P2P packets EF/DSCP-46 so WMM/SQM routers prioritize them over bulk traffic (effective on Proton; no-op on stock Windows). The Windows proxy now hooks `setsockopt` too, so the game can no longer shrink our enlarged send buffer back to 32 KB. Opt-in `BZ_GOV_SCAN` diagnostic locates the hardcoded 4000 B/s send-governor start constant in the DRM-decrypted image at runtime. Full analysis: [`test-logs/2026-07-03_dup_test_summary.md`](test-logs/2026-07-03_dup_test_summary.md); options survey: [`resources/PATCH_OPTIONS_RESEARCH.md`](resources/PATCH_OPTIONS_RESEARCH.md).

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
WINEDLLOVERRIDES=dsound=n,b %command% -nointro
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

- Fixes out-of-order handling; true packet loss is only mitigated by the opt-in `BZ_SEND_DUP` redundancy, and reordering can't recover packets delayed *beyond* the 100 ms window (heavy congestion produces exactly this — see the test writeup).
- The game's send governor always ramps from a hardcoded 4 KB/s at match start — net.ini can't change the starting rate, and short matches never reach any bandwidth ceiling. Start-of-match traffic bursts remain the main unsolved drop source. The exe is DRM-encrypted so this can't be patched statically; the `BZ_GOV_SCAN` diagnostic locates the constant at runtime as the first step toward an in-memory fix.
- If you're subscribed to a workshop mod that ships net.ini, it overrides the patch's tuned copy — unsubscribe (disabling in-game is not enough).
- On real Windows the game receives via overlapped/IOCP, which the reorder hook deliberately bypasses — so Windows players currently get bigger buffers, DSCP, and dup, but **not** inbound reordering. Reorder is fully active under Proton. (An IOCP-aware reorder path is on the roadmap.)

---

## More Technical Docs

- [Linux/proton_dsound_proxy/README.md](Linux/proton_dsound_proxy/README.md)
- [Microslop/winmm_proxy/README.md](Microslop/winmm_proxy/README.md)
- [net-ini/README.md](net-ini/README.md)
- [resources/INVESTIGATION_WRITEUP.md](resources/INVESTIGATION_WRITEUP.md)
