# Battlezone 98 Redux Netcode Patch

## What's New in V4.5

> The reorder fix, bigger buffers, and **DSCP priority marking** are on by default — **install and play, nothing to configure.** DSCP tags your game packets so a WMM/SQM router serves them ahead of bulk downloads (real effect under Proton; harmless no-op on stock Windows).
>
> **New: send-governor cold-start fix (`BZ_GOV_START`, opt-in).** The game hardcodes a 4000 B/s send rate at the *start of every match* and ramps up slowly — which is exactly why packet drops cluster in the first ~60 seconds. We dumped the DRM-decrypted game code at runtime, found the constant, and confirmed that rewriting it in-place works but trips SteamStub's anti-tamper — so the fix is a **data-only** patch that lifts the live send-rate off 4000 without touching game code. Set `BZ_GOV_START=16000` to try it. It's sender-side, so it also improves how your traffic reaches *unpatched* peers. Off by default while it's validated in live matches — see the note below.

---

Battlezone's netcode drops any UDP packet that doesn't arrive in *exact* sequential order, even by milliseconds. WiFi? Wireless? International? Anything with even mild jitter? You're not losing packets to the network - you're losing them to a rigid sequencing requirement that tolerates zero deviation.

This patch intercepts wayward packets mid-flight, buffers them briefly, and releases them in order. The game never knows it's there.

**Measured result (live A/B, same map, same opponent): 121 drops → 40 drops per match. ~65% fewer out-of-order drops.**

---

## Quick Start

**Everyone in the lobby should install this** — the fix is receive-side, so each install only protects that player's own inbound packets.

### Windows 🪟

Paste into PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_windows.ps1 | iex"
```

Auto-detects your install (registry + Steam library folders), downloads the prebuilt `winmm.dll` (SHA256-verified), and installs the tuned `net.ini` as a local mod. No launch option changes needed — reorder, bigger buffers, the setsockopt fix, and DSCP marking are active by default. To help test the opt-in governor cold-start fix, run `setx BZ_GOV_START 16000` and fully restart Steam; confirm `governor_patch: enabled` in `winmm_proxy.log`.

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

That's all you need — the reorder patch, bigger buffers, and DSCP marking are on by default. To help test the opt-in governor cold-start fix, add `BZ_GOV_START=16000` before `%command%` (see the note at the top); confirm `governor_patch: enabled` in `dsound_proxy.log`.

### Who Should Install It?

Everyone playing, ideally. Battlezone drops out-of-order packets **at the receiver**, and this patch sits in front of *your* copy of the game — it un-scrambles packets arriving *to you* and does nothing for packets you send to others. If only you install it, enemies look smooth on your screen while you still look laggy on theirs.

| Component | Who benefits from *your* install |
|---|---|
| Reorder buffer | Only you (your inbound from every peer) |
| Bigger socket buffers | Only you (your burst tolerance) |
| DSCP marking | Only you (your outbound gets router priority, if your router honours it) |
| Tuned net.ini | Your own send governor (it runs on every machine, not just the host's) |

Playing with someone who can't or won't install? There's no client-side setting that fixes *their* end — if their connection is the bottleneck, they need wired ethernet, router QoS, or to stop background uploads. (`BZ_SEND_DUP` was the old answer here; testing showed it doesn't help — see the note at the top.)

---

## How It Works

- **Adaptive per-peer reorder window:** starts at a 5 ms floor so clean connections pay near-zero added latency, grows toward a 100 ms ceiling only when reordering is actually observed on that link, and decays back down when the link is clean. The 100 ms ceiling is what produced the measured 65% drop reduction (the old 45 ms default left too many near-misses on the table).
- **Wake thread:** the reorder hook drains the kernel socket, so a game sleeping in `select()` could leave held packets stranded. A background thread nudges the socket readable so held packets release on time.
- **4 MB receive / 512 KB send socket buffers**, forced at socket creation for burst tolerance — and re-forced through a `setsockopt` hook on both platforms, so the game can't shrink the send buffer back to 32 KB (it tries to, on real Windows).
- **DSCP priority marking (`BZ_DSCP`, default 46 = EF):** tags the P2P socket so routers running WMM (WiFi voice queue) or SQM/fq_codel serve game packets ahead of bulk traffic — targeting queueing delay, which live testing showed is the real lag source under load. Effective under Proton; a no-op on stock Windows (which needs qWAVE or a router rule). `BZ_DSCP=0` disables.
- **Loss redundancy (`BZ_SEND_DUP=1`) — deprecated, off by default.** Re-sends outbound P2P datagrams via the `WSASendTo` hook so a genuinely *lost* packet could still arrive, with a time-shifted copy (`BZ_DUP_DELAY_MS`) and rate cap (`BZ_DUP_MAX_PPS`). Live A/B testing showed it doesn't help this game and hurts busy uplinks (a peer's link went 3.6 → 47.9 drops/min with it on), because at BZ's ~30 packets/sec the rate cap rarely engages and duplication still ~doubles the packet load. Kept for completeness; leave it off.
- **Governor cold-start fix (`BZ_GOV_START`, opt-in):** the game's send-rate governor hardcodes a 4000 B/s start for *every* match and ramps up slowly, starving the opening world-state burst — which is why the heaviest packet drops cluster in the first ~60 seconds. Setting `BZ_GOV_START=16000` raises that start. It's a **data-only** patch (the proxy watches the live send-rate value and lifts the 4000 cold-start to your target; it never modifies game code, so the DRM's integrity check is untouched). Sender-side, so it improves how your packets reach *every* peer, patched or not. Off by default; verified not to crash the DRM, still being validated in live matches.
- **Auto-kick threshold relax (`BZ_AUTOKICK_RELAX` / `BZ_AUTOKICK_*`, on by default, host-only):** the `System automatically kicked <player>` drop mid-match is a host-side timer — a player is ejected once their connection stays bad (ping over `AutoKickPing`, default 750 ms, *or* loss over `AutoKickLoss`, default 25) continuously for `AutoKickTime` (default 15 s). We recovered these four `[Net]` thresholds from the decrypted code; the proxy widens them automatically so a transient lag spike no longer kicks anyone (`BZ_AUTOKICK_RELAX=0` restores stock kicking). Same **data-only**, DRM-safe poke as `BZ_GOV_START`, re-asserted every 100 ms so it also overrides the fragile `net.ini` path. Enforced by the session host, so it only helps when *you* host.
- Everything runs in userspace via DLL proxy injection (`dsound.dll` on Proton, `winmm.dll` on Windows). Same code, same tuning env vars (`BZ_REORDER_*`, `BZ_SEND_DUP`, `BZ_DUP_*`, `BZ_DSCP`, `BZ_GOV_START`, `BZ_AUTOKICK_*`) on both platforms — see the proxy READMEs.

## What We Learned About the Game (the hard way)

Verified in live testing, because the community wisdom was mostly stale:

- **net.ini only loads through the mod system — and even then, found ≠ applied.** A copy next to `battlezone98redux.exe` is silently ignored. The installers deliver it as a local mod at `packaged_mods/9990001/net.ini`, which the game logs as `MOD FOUND` on both platforms — but a live match (2026-07-05) proved the values still aren't always applied: the host ran `AutoKickTime = 45000` in that file yet kicked at the stock 15 s, and its bandwidth sat below the file's `MinBandwidth`. Working theory: the game only parses net.ini out of the session's *active* mod (the map's mod), not from every discovered mod. The proxies' env-var pokes bypass this path entirely, which is why the auto-kick relax now defaults on.
- **Disabling a workshop mod in-game does NOT stop its net.ini loading.** Only unsubscribing does. If you're subscribed to the "Auto-Kick Reduction Patch" (workshop `1895622040`), it overrides the local mod — and it caps your send rate at 32 KB/s.
- **The send-rate governor runs on every machine**, not just the host's. And it always starts at a hardcoded 4,000 bytes/s, ramping up slowly — `MinBandwidth` does not set the starting rate, and in short matches the `MaxBandwidth` ceiling is never reached.
- The game exe imports classic winsock functions **by ordinal** and sends all P2P traffic via `WSASendTo` (`sendto` isn't in its import table at all). On real Windows its receives are overlapped/IOCP; under Proton they aren't.

## Version History

- **V1–V2:** forced bigger UDP socket buffers (final: 512 KB send / 4 MB receive). Better burst tolerance.
- **V3:** in-proxy out-of-order packet reordering (`WSARecvFrom` hook), per-peer buffering with deterministic sequence release. Sequence field located at `payload[13..16]` (u32le) via binary capture analysis.
- **V4:** adaptive reorder window (5 ms floor), wake thread for stranded packets, Linux kernel-clamp fix in the installer, Windows/Linux tuning parity, `BZ_SEND_DUP`, drop metrics in the verify script.
- **V4.1:** **Windows launch-freeze hotfix** — the hook was routing the game's overlapped (IOCP) receives through the synchronous reorder path, hanging the game at the loading screen (Proton was unaffected; the bug existed since V3). Also: ordinal IAT patching, and `BZ_SEND_DUP` moved to the `WSASendTo` hook where it actually works.
- **V4.2:** reorder ceiling default raised 45 → 100 ms after live A/B testing measured ~65% fewer drops (121 → 40/43 on the same map/opponent); harmless on clean links thanks to the adaptive floor. net.ini now installs as a local packaged mod.
- **V4.3:** reworked `BZ_SEND_DUP` (loopback-skip + time-shifted + rate-capped copies), added `BZ_DSCP` priority marking, ported the `setsockopt` re-force hook to Windows, and added the opt-in `BZ_GOV_SCAN` diagnostic that locates the hardcoded 4000 B/s governor start constant in the DRM-decrypted image at runtime.
- **V4.4:** **`BZ_SEND_DUP` deprecated.** A ~10-game A/B series (1v1s + 2v2s, logs from all peers) settled it: outbound duplication doesn't help this game and degrades busy uplinks — one link went 3.6 → 47.9 drops/min with it on, because at BZ's ~30 pkt/s the rate cap rarely engages so it still ~doubles packet load. It's removed from all recommended launch options and installer prompts (still present, opt-in, off). The validated core is the always-on reorder + bigger buffers + DSCP marking. The governor scanner is now on both proxies. Full verdict + method: [`test-logs/2026-07-03_dup_test_summary.md`](test-logs/2026-07-03_dup_test_summary.md); analysis tool at [`tools/analyze_drops.py`](tools/analyze_drops.py); options survey: [`resources/PATCH_OPTIONS_RESEARCH.md`](resources/PATCH_OPTIONS_RESEARCH.md).
- **V4.5 (current):** **send-governor cold-start fix (`BZ_GOV_START`, opt-in).** The governor hardcodes a 4000 B/s start for every match — the root of the first-60-seconds drop clusters, and the biggest remaining drop source. We dumped the DRM-decrypted code at runtime, located the exact site, and found that rewriting the constant in `.text` works but SteamStub's integrity check then kills the game — so the fix is a **data-only** patch: the proxy watches the live send-rate value and lifts the 4000 cold-start to `BZ_GOV_START` (e.g. 16000), never touching game code. Verified end-to-end that it applies and the game survives (no DRM trip). It's sender-side, so it also improves how your traffic reaches unpatched peers. Off by default pending live-match validation. See the proxy READMEs for the `BZ_GOV_START` knob.

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

- Fixes out-of-order handling, not raw packet loss or congestion. Reordering can't recover packets delayed *beyond* the 100 ms window — heavy congestion produces exactly this, and no receiver-side patch can fix a saturated uplink (that's a wired-ethernet / router-QoS problem on the sending peer's end). Outbound duplication was tested as a loss mitigation and deprecated (see the top note).
- The game's send governor hardcodes a 4 KB/s start at match start (net.ini can't change it), which drives the start-of-match drop bursts. This is now addressed by the opt-in `BZ_GOV_START` data patch (see How It Works) — off by default while it's validated in live matches. The exe is DRM-encrypted, so this is a runtime in-memory fix, not a static one.
- If you're subscribed to a workshop mod that ships net.ini, it overrides the patch's tuned copy — unsubscribe (disabling in-game is not enough).
- On real Windows the game receives via overlapped/IOCP, which the reorder hook deliberately bypasses — so Windows players currently get bigger buffers, DSCP, and dup, but **not** inbound reordering. Reorder is fully active under Proton. (An IOCP-aware reorder path is on the roadmap.)

---

## More Technical Docs

- [Linux/proton_dsound_proxy/README.md](Linux/proton_dsound_proxy/README.md)
- [Microslop/winmm_proxy/README.md](Microslop/winmm_proxy/README.md)
- [net-ini/README.md](net-ini/README.md)
