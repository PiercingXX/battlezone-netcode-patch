# Battlezone 98 Redux – Windows Netcode Patch

## Overview

This patched `winmm.dll` proxy for 64-bit Windows improves network performance for **Battlezone 98 Redux** multiplayer by:
1. **Enlarging UDP socket buffers** to handle burst traffic: SO_SNDBUF=512KB, SO_RCVBUF=4MB
2. **Out-of-order packet reordering** (OOO) with per-peer buffering and time-based delivery to reduce lag and frame drops
3. **Optional binary packet logging** to `bz_buffer_log.bin` for low-overhead capture and offline analysis

The patch is deployed as a 32-bit DLL (for Windows 7/10/11 compatibility) that is injected into the game process at startup.

## Architecture

**winmm.dll Proxy Chain:**
- Game process → [hooked winmm.dll] (this file) → System32\\winmm.dll (real implementation)
- All audio exports are forwarded transparently

**Netcode Hooks (IAT patching):**
- WSASocketW → Hooked_WSASocketW (apply buffer tuning at socket creation)
- WSARecvFrom → Hooked_WSARecvFrom (implement OOO reorder with drain-and-deliver)
- closesocket → Hooked_closesocket (reset per-peer state on socket close)

## Build Instructions (Linux cross-compile)

### Prerequisites
```bash
sudo apt install gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686
```

### Build
```bash
cd Microslop/winmm_proxy
make          # produces build/winmm.dll
make clean    # remove build artifacts
```

## Deployment

### Manual Installation (Testing)
1. Build `winmm.dll` as above
2. Copy `build/winmm.dll` to your game directory (same folder as `battlezone98redux.exe`):
   ```
   C:\Program Files (x86)\Steam\steamapps\common\Battlezone 98 Redux\
   ```
3. Launch the game normally (Windows will prefer the local winmm.dll over System32)

### Steam Launch Options
No special launch options needed. Just start the game:
```
%command%
```

The netcode hook thread runs automatically inside DLL_PROCESS_ATTACH, resolves real API functions, patches the game's IAT, and initializes reorder state.

## Configuration

Socket buffer targets are baked into code defaults:

| Parameter | Default | Notes |
|-----------|---------|-------|
| SO_SNDBUF | 524,288 bytes (512 KB) | Set at socket creation via WSASocketW hook |
| SO_RCVBUF | 4,194,304 bytes (4 MB) | Set at socket creation via WSASocketW hook |

The reorder hold window is **adaptive per peer**: it starts at a small floor
(`BZ_REORDER_MIN_MS`, default 5 ms) so clean connections get near-zero added
latency, grows toward the ceiling (`BZ_REORDER_WINDOW_MS`, default 100 ms)
only when reordering is actually observed on that link, and decays back down
proportionally once the link goes quiet (a clean link returns to the floor in
seconds).  Growth is sized by the lateness actually measured, so pure packet
loss — which is not reorder evidence — no longer pins the window at the
ceiling.  `BZ_REORDER_MAX_HOLD_MS` caps the hold regardless.

A background **wake thread** prevents held packets from stranding when the
game sleeps in `select()` on the (already drained) socket: it nudges the
socket readable with a tiny internal datagram that the hook discards.

The state machine lives in [`shared/reorder_core.h`](../../shared/reorder_core.h),
shared verbatim with the other proxy and covered by `make -C tests run`.

Since V4.7 the buffer also reports what it is doing, every 10 seconds and once
at session end:

```
reorder_stats: delivered=9127 (in_order=9000 forced=120 evicted=3 first=4)
  dropped(stale=57 dup=12 reclaim=0) bypass(short=88 tblfull=0) emsgsize=0
  peers_reclaimed=1 drain_max=14 hold_ms(avg=4 max=97)
  hist[0,1-5,6-15,16-30,31-60,61+]=[8000,900,150,60,12,5] win_ms=[35,5]
```

`hold_ms` is the one to watch: it is the latency **this patch** added to the
game's streams, which the game's own drop counter cannot show. `evicted` and
`emsgsize` should both be zero; if they aren't, the buffer is under pressure
and needs a deeper `BZ_REORDER_DEPTH` or a larger packet buffer respectively.
Run `tools/analyze_drops.py <proxy log>` to have these read out for you.

The `session end:` lines are normally written from `closesocket`. A game that
exits without closing its P2P socket never reaches that path, and before V4.8
the whole session's counters went with it — the 2026-07-26 V4.8 match lost its
send measurement exactly this way on the Proton proxy. `ShutdownNetcodeHooks`
now re-emits them on the `DLL_PROCESS_DETACH` path as a backstop, preceded by:

```
process exit without closesocket: emitting session counters from DLL_PROCESS_DETACH
```

Treat that marker as a caveat on the numbers below it, not on their validity:
the counters are accurate as of process exit, but the pacer queue is not
flushed on this path, so a few in-flight packets may be uncounted. If a worker
thread died holding a lock the line reads `... lock held at exit, send_stats
lost` instead, and there is nothing to recover.

### `[Net]` tuning poke (`BZ_NET_*`, on by default)

The game reads its whole `[Net]` configuration into fixed globals at match
start.  `net.ini` is supposed to supply them, but the game has twice been caught
logging `MOD FOUND net.ini` while running stock values anyway, so the proxy
writes them directly instead — data-only, DRM-safe, re-asserted every 100 ms,
and version-gated on the same signature the governor patch uses.  Each address
is additionally sanity-checked: a value outside a plausible range means the
address is wrong for this build, and the entry is logged and skipped rather than
written.

| Variable | Default | net.ini key | Stock | Notes |
|---|---|---|---|---|
| `BZ_NET_TUNE` | `1` | — | — | `0` restores the game's stock governor behaviour |
| `BZ_NET_MINBANDWIDTH` | `16000` | MinBandwidth | 4000 | the collapse floor. On since V4.94: a 2026-08-12 collapse bottomed out at the stock 4,150 B/s and the match spent two minutes there. `0` reverts to leaving the game's value alone |
| `BZ_NET_MAXBANDWIDTH` | `320000` | MaxBandwidth | 16000 | the governor is closed-loop, so a high ceiling is not itself a risk |
| `BZ_NET_UPCOUNT` | `100` | UpCount | 10 | recovery step, twice DownCount as stock intends. Reconciled 2026-08-12: the old 50/200 pairing cut 5x faster than it recovered (measured -203 vs +40.5 B/s per second), so a two-minute collapse needed nine minutes to undo |
| `BZ_NET_DOWNCOUNT` | `50` | DownCount | 5 | bytes removed from the send budget per adjustment while over MaxPing (the governor's back-off step, not a receive budget) |
| `BZ_NET_MAXPING` | `450` | MaxPing | 300 | stock turns a jitter spike into a rate cut into more warping into more spike |
| `BZ_NET_MAXPINGSLOST` | leave | MaxPingsLost | 20 | no evidence a change helps |
| `BZ_AUTOKICK_RELAX` | `1` | — | — | `0` restores stock kicking |
| `BZ_AUTOKICK_START` | `60000` | AutoKickStart | 10000 | host-enforced |
| `BZ_AUTOKICK_PING` | `2000` | AutoKickPing | 750 | host-enforced |
| `BZ_AUTOKICK_LOSS` | `200` | AutoKickLoss | 25 | host-enforced |
| `BZ_AUTOKICK_TIME` | `60000` | AutoKickTime | 15000 | host-enforced |

Any variable set to `0` leaves the game's own value alone.

### Outbound pacing (`BZ_SEND_PACE`, off by default)

Burst measurement is unconditional and appears as `send_stats` alongside the
reorder counters: peak packets/sec, peak bytes/sec, and how many seconds ran
above a burst threshold.  This exists because the failure that has actually
ended matches is a peer's reliable-send queue burst-retransmitting — 1,179
packets in 13.4 s in the measured case — and nothing here had ever looked at
what the local machine puts on the wire.

| Variable | Default | Notes |
|---|---|---|
| `BZ_SEND_PACE` | `0` | bytes/sec token bucket; `0` measures without ever delaying |
| `BZ_SEND_PACE_MAX_MS` | `20` | hard cap on added send latency |

The pacer never drops a packet, never reorders one, and never delays traffic too
small to carry a sequence number (the ping exchange the auto-kick measures).  It
can only absorb `BZ_SEND_PACE_MAX_MS × rate` bytes, so at Battlezone's rates the
default shapes well under one packet and traffic passes straight through — read
`send_stats` before raising either knob.

### Duplicate suppressor (`BZ_SEND_DAMPEN`, on by default since V4.94)

BZRNet's reliable retry timer is fixed at ~10 ms with no backoff, against an
RTT the game itself reports as 56–91 ms, so every reliable message goes out
6–9 times before an acknowledgement can physically return (see
`resources/CAMERAPOD_STORM.md`).  The damper drops the redundant in-window
copies on the send path: only a 2nd-or-later copy of a `(peer, sequence)`
already sent inside its window is ever suppressed — a first transmission, a
distinct sequence, and anything too small to carry a sequence number always
go.  A suppressed send looks to the game like a successful one (a UDP send
promises handoff, not delivery), is still counted by the burst measurement,
and is never duplicated by `BZ_SEND_DUP`.

| Variable | Default | Notes |
|---|---|---|
| `BZ_SEND_DAMPEN` | `1` | suppresses redundant in-window reliable retransmits; `0` disables. On by default since V4.94 — replaying the 2026-08-12 storm's logged send stream through it suppresses 63.9% of the datagrams at the 60 ms floor window and 69.0% at a realistic 1.2xRTT window |

The suppression window starts at a 60 ms floor and doubles on each genuine
loss-recovery retransmit, capped at 400 ms.  A peer restart is detected
in-band (a sequence below the ring's oldest retained entry), and the
socket-close path purges every peer explicitly, so a reconnecting peer is
never suppressed.  Counters appear at teardown as a `session end: dampen:`
line.

### Windows overlapped/IOCP receives (`BZ_IOCP_*`, off by default)

On real Windows the game receives through overlapped/IOCP calls, which the
synchronous reorder path deliberately bypasses — so inbound reordering does
nothing for most installs.

| Variable | Default | Notes |
|---|---|---|
| `BZ_IOCP_SCAN` | `0` | read-only: logs which completion API the game uses, and how many overlapped receives it posts |
| `BZ_IOCP_REORDER` | `0` | **unvalidated**: defers completions to restore order |

**Run the scan first.**  Which completion API this game actually calls has never
been observed, and the reorder path cannot apply if it is not
`GetQueuedCompletionStatus`.

Reordering here fundamentally requires holding a completion back.  Swapping
buffers cannot work: whatever we hand the game, its own sequencing advances
immediately and cannot be un-advanced, so by the time a late packet arrives the
cursor has already passed it.  Holding completions is also exactly what froze the
game at the loading screen in V4.1, so this implementation holds at most four,
never past the reorder window, never past the caller's own timeout, and latches
itself off permanently if a hold overruns.  That is careful, not proven — it has
never run on real Windows.  Do not enable it on a machine you are not willing to
have fail to launch.

Runtime tuning (same env vars as the Linux dsound proxy):

| Variable | Default | Notes |
|-----------|---------|-------|
| BZ_REORDER | **0** | Off since V4.8, and V4.9 found the structural reason: the protocol's sequence number counts *messages*, not datagrams, so there is no per-datagram key to reorder by. See `resources/BZ_P2P_HEADER.md`. `1` still enables it; the rows below only matter if you do |
| BZ_REORDER_WINDOW_MS | 100 | Max (ceiling) hold time before forced delivery (clamp 5–200) |
| BZ_REORDER_MIN_MS | 5 | Adaptive window floor; `0` = deliver immediately unless reordering seen |
| BZ_REORDER_ADAPT | 1 | Set to `0` for a fixed window equal to BZ_REORDER_WINDOW_MS |
| BZ_REORDER_MAX_HOLD_MS | = window | Hard ceiling on how long any packet may be held, independent of the adaptive window. Bounds the latency the patch can add to your ping (clamp 0–500) |
| BZ_REORDER_WAKE | 1 | Set to `0` to disable the wake thread |
| BZ_REORDER_STATS | 1 | Set to `0` to silence the 10-second `reorder_stats:` counter lines |
| BZ_REORDER_DEPTH | 32 | Max buffered packets per peer (max 32) |
| BZ_REORDER_PEERS | 16 | Max distinct IPv4 sources (max 16) |
| BZ_REORDER_DRAIN | 96 | Real WSARecvFrom calls per hook invocation (max 128). The drain also stops early whenever a peer queue is full, so this is an upper bound, not a target |
| BZ_SEND_DUP | 0 | **Deprecated** (off by default). Re-sends outbound P2P datagrams. Live A/B testing showed it doesn't help this game and degrades busy uplinks by ~doubling packet rate. Kept for completeness; leave off |
| BZ_GOV_START | **40000** | **On by default since V4.8.** Raise the send governor's hardcoded 4000 B/s match-start rate to this many bytes/sec (e.g. `16000`). Data-only patch of the live send-rate global (never touches `.text`, so SteamStub's integrity check is untouched). `0` = disabled. Targets the first-60-seconds drop clusters; sender-side |
| BZ_GOV_VERIFY_MS | 10000 | V4.9 read-back: how long after the cold-start poke to re-read the global and declare it held. A `POKE DID NOT HOLD` line means something rewrote the value and the session is not a valid BZ_GOV_START sample — this happened in one of the two V4.8 matches and took hand-correlating the game's own log to notice |
| BZ_GOV_TRACE_MS | 15000 | V4.9 read-back: interval of the periodic `governor_trace:` line reporting the live send rate with its min/max since the last one, which is what makes the ramp rate visible. `0` silences it |
| BZ_GOV_SCAN | 0 | Diagnostic: 15 s after launch, scan the DRM-decrypted `.text` for the 4000 B/s governor start constant and log candidate addresses. Read-only; never patches |
| BZ_DUP_DELAY_MS | 25 | Delay before the duplicate is transmitted (max 500). Time-shifting the copy means one queue spike can't kill both. `0` = legacy back-to-back duplicate |
| BZ_DUP_MAX_PPS | 40 | Cap on duplicates per second (max 2000). Low-rate control traffic keeps redundancy; bulk bursts shed theirs. `0` = unlimited |
| BZ_DSCP | 46 | DSCP class marked on the P2P socket (max 63). 46 = EF. Ignored by stock Windows policy (no-op); use qWAVE or a router rule for real effect there. `0` disables |
| BZ_AUTOKICK_RELAX | 1 | **On by default, host-only.** One-switch preset relaxing all four auto-kick thresholds below (start=60000, ping=2000, loss=200, time=60000) so a transient lag spike no longer ejects a player. Individual `BZ_AUTOKICK_*` vars override it; `0` restores stock kicking. Only affects kicks when **this machine hosts** the session |
| BZ_AUTOKICK_TIME | 0 | Override `AutoKickTime` — ms a player's connection must stay continuously bad before the host kicks them (game default `15000`). `0` = leave the game's value |
| BZ_AUTOKICK_PING | 0 | Override `AutoKickPing` — ping ceiling in ms; a tick above it is "bad" (game default `750`). `0` = leave the game's value |
| BZ_AUTOKICK_LOSS | 0 | Override `AutoKickLoss` — per-tick loss-count ceiling (game default `25`). `0` = leave the game's value |
| BZ_AUTOKICK_START | 0 | Override `AutoKickStart` — ms of grace after a join before monitoring begins (game default `10000`). `0` = leave the game's value |

Auto-kick is enforced by the session **host** (data-only poke of the four
`[Net]` threshold globals, re-asserted every 100 ms, version-gated on the
governor signature; never touches `.text`), so these only change whether *this*
machine kicks a lagging peer. The relax preset defaults on because the
`net.ini` route proved unreliable in live play: a host whose
`packaged_mods/9990001/net.ini` set `AutoKickTime = 45000` still fired a stock
15 s kick (2026-07-05) — the game logs `MOD FOUND net.ini` but does not
reliably apply its values unless the file ships inside the session's *active*
mod. Confirm with `autokick_patch: enabled` in `winmm_proxy.log`.

Optional logging controls:

| Variable | Default | Notes |
|-----------|---------|-------|
| BZ_BUFFER_LOG | off | Set to `1` to enable binary packet capture |
| BZ_BUFFER_LOG_BYTES | 32 | Payload prefix bytes stored per record |
| BZ_BUFFER_LOG_RING | 65536 | Number of ring-buffer records held in memory |

## Verification

### Check That Patching Succeeded

Look for log output in `winmm_proxy.log` (same directory as the game exe):

```
=== winmm_proxy.dll loaded ===
  Game dir : C:\...\common\Battlezone 98 Redux\
  Log file : C:\...\common\Battlezone 98 Redux\winmm_proxy.log
...
InstallNetcodeHooks: starting
InstallNetcodeHooks: WSASocketW IAT patched OK  SO_SNDBUF target=524288  SO_RCVBUF target=4194304
InstallNetcodeHooks: WSARecvFrom IAT patched OK  OOO reorder enabled max_window_ms=100 min_window_ms=5 adapt=1 wake=1 depth=8 peers=32 drain=96
InstallNetcodeHooks: closesocket IAT patched OK
```

### Check Socket Buffer Readback

Subsequent log lines will show actual effective SO_SNDBUF / SO_RCVBUF values:

```
WSASocketW hook: sock=0x... af=2 type=2 proto=17  SO_SNDBUF set_rc=0 effective readback SO_SNDBUF=524288  SO_RCVBUF set_rc=0 effective readback SO_RCVBUF=4194304
```

If effective values are **less than target**, Windows may have clamped them due to registry limits. Consult Windows network tuning docs.

### Check Binary Packet Logging

If you launched with `BZ_BUFFER_LOG=1`, the proxy will flush these files into the game directory on exit:

```
bz_buffer_log.bin
bz_buffer_log.meta.txt
```

`winmm_proxy.log` will also include startup/shutdown lines such as:

```
buffer_log: enabled payload=32 ring=65536 stride=84
buffer_log: flushed records=... total_events=...
```

## OOO Reorder Engine (Technical)

### Packet Flow

1. **Socket Hookup:** Game calls WSASocketW → our hook applies SO_SNDBUF/SO_RCVBUF tuning → returns socket handle
2. **Drain Loop:** Game calls WSARecvFrom → our hook:
   - Pulls up to 96 packets from the real socket (non-blocking)
   - Extracts sequence number (BZRNet frame counter at byte offset 13)
   - Buffers each packet per source IP:port into per-peer PeerBuf
3. **Delivery Selection:** Scan per-peer buffers for ready packets:
   - Prefer exact in-order successor of last_seq
   - Fallback to lowest-seq packet once it's aged past the peer's adaptive window
   - On first packet per peer, deliver oldest immediately
4. **Return to Game:** Copy selected packet to game's WSA buffers, update per-peer sequence state, return

### Per-Peer State Structure (PeerBuf)

```c
struct PeerBuf {
    uint64_t key;       // (ipv4 << 16) | port (0 = entry unused)
    uint32_t seq_init;  // 1 once last_seq is valid
    uint32_t last_seq;  // last delivered sequence number
    uint32_t filled;    // count of occupied slots
    ReorderSlot slots[8];  // 8 packet buffers per peer
};
```

Each ReorderSlot holds: arrival timestamp, sequence number, packet length, source address, full packet data (up to 1500 bytes).

### Sequence Number Extraction

- **Location:** Byte offset 13 in UDP payload (confirmed via binary capture analysis)
- **Format:** u32le (little-endian)
- **Packets too short** (<17 bytes) or non-IPv4 sources bypass reorder and deliver directly

### Drain Limit

The drain loop will pull **up to 96 packets per WSARecvFrom hook call**. If the real socket has ≥96 packets buffered, we pull 96 and pause delivery to prevent the game from hanging. Next WSARecvFrom call will drain more. This balances responsiveness with throughput.

## Files

| File | Purpose |
|------|---------|
| src/dllmain.cpp | Entry point, winmm.dll forwarding, logging setup, hook thread spawn |
| src/netcode_hooks.h | Reorder structures (PeerBuf, ReorderSlot), hook function imports |
| src/netcode_hooks.cpp | WSASocketW, WSARecvFrom, closesocket hooks; reorder helpers; IAT patcher |
| src/winmm_proxy.cpp | winmm.dll stub exports (forwarding) |
| src/winmm.def | DLL export table (.def file for linker) |
| Makefile | i686-w64-mingw32-g++ cross-compile build rules |
| README.md | This file |

## Comparison with Linux Version

Both Linux (`dsound.dll`) and Windows (`winmm.dll`) proxies implement the same reorder engine:

| Aspect | Linux | Windows |
|--------|-------|---------|
| **Real DLL** | dsound.dll (audio output) | winmm.dll (multimedia) |
| **Hook Target** | WSARecvFrom in Proton's WS2_32.dll | WSARecvFrom in native WS2_32.dll |
| **Tuning** | SO_SNDBUF=524KB, SO_RCVBUF=4MB | SO_SNDBUF=524KB, SO_RCVBUF=4MB |
| **Reorder Profile** | adaptive 5–45ms, drain=96, depth=8, peers=32 | adaptive 5–45ms, drain=96, depth=8, peers=32 |
| **Logging** | Optional binary packet capture (BZ_BUFFER_LOG) | Optional binary packet capture (BZ_BUFFER_LOG) |

## Known Limitations

- **32-bit only:** 64-bit wine/proton not supported; Windows native only (though 32-bit DLL runs on 64-bit Windows via SysWoW64)
- **Assumes single UDP socket:** Resets entire peer buffer table on closesocket (correct for BZ which uses one socket per session)

## Testing Notes

- **Optimal lobby quality:** ~4.38 packet drops per minute (45ms window, 96 drain on clean peers)
- **Longer RTT (100ms+):** Drop rate may increase due to higher variance; tune `BZ_REORDER_WINDOW_MS` (ceiling) and `BZ_REORDER_MIN_MS` (floor) via env vars
- **Patching failures:** Check `winmm_proxy.log` for IAT patching errors; common causes are different DLL names in import table (code tries both "WS2_32.dll" and "ws2_32.dll")

## Troubleshooting

### Windows Defender quarantines winmm.dll (Program:Win32/Contebrew.A!ml)

- This project uses a DLL proxy/hook pattern, which can trigger heuristic or PUA detections on unsigned binaries.
- If Defender quarantines winmm.dll, confirm the detection details in Protection history and verify file integrity against your expected hash/build.
- Do not disable Defender globally. If you trust the exact file hash, restore only that item and apply a path-specific exception for the game-folder winmm.dll.
- Submit the quarantined sample to Microsoft as a false positive and include the detection name and file path.
- For maintainers/distribution: signed release artifacts and published SHA256 values reduce repeated false positives.

### "WSASocketW not found in game IAT"
- Game exe may have been linked differently; reorder will not be enabled
- Buffer tuning (SO_SNDBUF/SO_RCVBUF) may still help

### "WSARecvFrom not found in game IAT"
- OOO reorder will NOT be active; only buffer tuning will apply
- Check game version; may be newer build with different API usage

### Socket buffers not increasing
- Windows may have registry limits on SO_SNDBUF/SO_RCVBUF
- Check effective readback values in log; if clamped, tune registry (not covered here)

### High packet drop rate despite patch
- Check lobby quality (ping, packet loss) in game stats
- Verify log shows expected reorder config values
- If patching failed, fallback to buffer tuning alone may help but won't fix OOO issues

## License & Attribution

Based on live test data from Battlezone 98 Redux P2P network captures (March 2026). In-game sequence number location and reorder profile verified via binary packet analysis.
