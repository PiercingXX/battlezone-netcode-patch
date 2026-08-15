# Proton DSOUND Proxy (Linux) — Buffer Sizing + OOO Resequencing

This build forces socket buffer sizes and adds in-proxy packet resequencing in
`WSARecvFrom`.

## Why DSOUND.dll

The game imports `DSOUND.dll` very early and only uses ordinal `1` from it, making it a low-risk proxy target compared to a full `WS2_32.dll` proxy.

## What This Proxy Does

On process attach, the proxy:

1. Installs an early `GetProcAddress` hook in the main module.
2. Resolves and patches Winsock imports: `setsockopt`, `WSASetSocketOption`, `getsockopt`, `WSAGetSocketOption`, `socket`, `WSASocketW`, `closesocket`, `recvfrom`, `WSARecvFrom`, `ioctlsocket`, `WSAIoctl`.
3. Forces these target values when Battlezone configures socket buffers:
   - `SO_SNDBUF = 524288`
   - `SO_RCVBUF = 4194304`
4. Immediately reads the effective values back from the same socket handle.
5. If reorder is enabled, holds slightly out-of-order packets and delivers to the game in sequence.
6. When `BZ_BUFFER_LOG=1` is set, writes a binary packet trace to `bz_buffer_log.bin`.
7. Logs socket IDs, handles, force calls, readbacks, reorder state, and close events to `dsound_proxy.log`.
8. Forwards ordinal `1` to the real system `dsound.dll` on demand.

## Important Behavioral Finding

`BZLogger.txt` still prints the old default startup buffer line even when the patch is working.
`dsound_proxy.log` is the source of truth for verification.

## Build Requirements

```bash
sudo apt install mingw-w64   # Debian/Ubuntu
sudo pacman -S mingw-w64-gcc  # Arch
```

## Build

```bash
make
```

Output: `build/dsound.dll`

## Deploy

From the repository root:

```bash
./Linux/deploy_linux.sh "/path/to/Battlezone 98 Redux"
```

## Reorder Configuration

The hold window is **adaptive per peer**: it starts at a small floor
(`BZ_REORDER_MIN_MS`, default `5` ms) so clean connections get near-zero
added latency, grows toward the ceiling (`BZ_REORDER_WINDOW_MS`, default
`100` ms) only when reordering is actually observed on that link, and decays
back down proportionally once the link goes quiet (a clean link returns to
the floor in seconds).  Growth is sized by the lateness actually measured, so
pure packet loss — which is not reorder evidence — no longer pins the window at
the ceiling.  `BZ_REORDER_MAX_HOLD_MS` caps the hold regardless.

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
send measurement exactly this way. `DLL_PROCESS_DETACH` now re-emits them as a
backstop, preceded by:

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

### Round-trip sampling (`BZ_RTT`, on by default since V4.94)

Observation only: it reads two header fields and never alters, delays or drops
a datagram.

It exists because of the 2026-08-15 lag report. That match could be narrowed
to "the link was at 141/174 ms, against 73 ms two matches earlier the same
evening" and no further, because BZLogger prints its `Delay:` block only at
match start. Whether the link spiked during the warp storm or stayed flat
while something else broke was unanswerable from anything the patch collects.

**Why the ack field and not the send clock.** The header carries the sender's
wall clock at offset 2, but the two machines' clocks are not synchronised, so
subtracting it on receive yields (delay + clock offset) with no way to separate
the terms. The ack at offset 14 closes a loop inside one clock: record when our
sequence S went out, and when a peer acknowledges S the elapsed local time is a
true round trip.

**Read it as an upper bound.** The ack is piggybacked on the peer's normal
traffic rather than sent immediately, so a sample includes however long the
peer sat on it. The padding is bounded by the peer's send interval and does not
grow with distance or congestion, which is what makes it usable for "is the
link degrading".

Retransmitted sequences are never sampled (Karn's algorithm) — an ack for a
sequence sent twice cannot be attributed to either copy. The session line
reports `unmatched`, `ambiguous` and `discarded` alongside the sample count, so
a filtered population is visible instead of hidden behind a clean mean.

| variable | default | effect |
|---|---|---|
| `BZ_RTT` | `1` | per-peer round-trip sampling from the protocol's ack field; `0` disables |
| `BZ_RTT_TRACE_MS` | `15000` | interval for the periodic `rtt_trace:` line; `0` silences it and leaves only the session-end summary |

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
and is never duplicated.

| Variable | Default | Notes |
|---|---|---|
| `BZ_SEND_DAMPEN` | `1` | suppresses redundant in-window reliable retransmits; `0` disables. On by default since V4.94 — replaying the 2026-08-12 storm's logged send stream through it suppresses 63.9% of the datagrams at the 60 ms floor window and 69.0% at a realistic 1.2xRTT window |

The suppression window starts at a 60 ms floor and doubles on each genuine
loss-recovery retransmit, capped at 400 ms.  Since 2026-08-15 the window is
sized off the RTT sampler's live estimate (1.2 x srtt, same clamps): the
plumbing for this existed from day one but was never connected, so every
earlier session ran at the floor - at the measured 149 ms RTT that allowed
three times the copies the design intended.  The same evening's third game
also showed retries outliving the 64-slot sequence ring (659 new sequences a
minute against retries spanning 9.5 s), and a retry of an evicted sequence is
indistinguishable from a peer restart, so it wiped the ring mid-storm.  The
ring is now 512 slots and the wipes are counted (`epoch_resets` in the stats
line - during a match anything beyond one per reconnect means the ring is
undersized again).  A peer restart is detected in-band (a sequence below the
ring's oldest retained entry), and the socket-close path purges every peer
explicitly, so a reconnecting peer is never suppressed.  Counters appear at
teardown as a `session end: dampen:` line.

| Variable | Default | Description |
|---|---|---|
| `BZ_REORDER` | `1` | Set to `0` to disable reordering entirely |
| `BZ_REORDER_WINDOW_MS` | `100` | Max (ceiling) hold time before releasing oldest queued packet |
| `BZ_REORDER_MIN_MS` | `5` | Adaptive window floor; `0` = deliver immediately unless reordering seen |
| `BZ_REORDER_ADAPT` | `1` | Set to `0` for a fixed window equal to `BZ_REORDER_WINDOW_MS` |
| `BZ_REORDER_MAX_HOLD_MS` | = window | Hard ceiling on how long any packet may be held, independent of the adaptive window. Bounds the latency the patch can add to your ping |
| `BZ_REORDER_WAKE` | `1` | Set to `0` to disable the wake thread |
| `BZ_REORDER_STATS` | `1` | Set to `0` to silence the 10-second `reorder_stats:` counter lines |
| `BZ_REORDER_DEPTH` | `32` | Active per-peer reorder queue depth (max `32`) |
| `BZ_REORDER_PEERS` | `16` | Active peer table size (max `16`) |
| `BZ_REORDER_DRAIN` | `96` | Max socket drain iterations per hook call (max `128`). The drain also stops early whenever a peer queue is full, so this is an upper bound, not a target |
| `BZ_SEND_DUP` | — | Retired in V5: the knob is no longer honoured. Live A/B showed outbound duplication does not help this game |
| `BZ_DSCP` | `46` | DSCP class marked on the P2P socket via IP_TOS (max `63`). 46 = EF; WMM/SQM routers prioritize it over bulk traffic. Effective under Proton. `0` disables |
| `BZ_GOV_START` | `0` | **Opt-in.** Raise the send governor's hardcoded 4000 B/s match-start rate to this many bytes/sec (e.g. `16000`). Data-only patch of the live send-rate global (never touches `.text`, so SteamStub's integrity check is untouched); watches for the 4000 cold-start and bumps it. `0` = disabled. Targets the first-60-seconds drop clusters; sender-side, so it helps how your packets reach every peer |
| `BZ_GOV_SCAN` | `0` | Diagnostic: 15 s after launch, scan the DRM-decrypted `.text` for the 4000 B/s governor start constant and log candidate addresses. Read-only; never patches. (The signature is already captured; this is for re-locating it if the game updates) |
| `BZ_AUTOKICK_RELAX` | `1` | **On by default, host-only.** One-switch preset that relaxes all four auto-kick thresholds below (start=60000, ping=2000, loss=200, time=60000) so a transient lag spike no longer ejects a player. Individual `BZ_AUTOKICK_*` vars override the preset; `0` restores stock kicking. Only affects kicks when **this machine hosts** the session |
| `BZ_AUTOKICK_TIME` | `0` | Override `AutoKickTime` — ms a player's connection must stay continuously bad before the host kicks them (game default `15000`). Raise it (e.g. `60000`) to survive long lag spikes. `0` = leave the game's value |
| `BZ_AUTOKICK_PING` | `0` | Override `AutoKickPing` — ping ceiling in ms; a tick above it counts as "bad" (game default `750`). `0` = leave the game's value |
| `BZ_AUTOKICK_LOSS` | `0` | Override `AutoKickLoss` — per-tick loss-count ceiling; above it counts as "bad" (game default `25`). `0` = leave the game's value |
| `BZ_AUTOKICK_START` | `0` | Override `AutoKickStart` — ms of grace after a player joins before auto-kick monitoring begins (game default `10000`). `0` = leave the game's value |
| `BZ_BUFFER_LOG` | *(off)* | Set to `1` to capture binary packet trace |

## Auto-Kick Threshold Override (host-only, on by default)

Battlezone ejects a player whose connection stays bad for too long — the
`System automatically kicked <player>` / `Auto kicking player <player> due to
ping failure` you see right before a mid-match drop. Reverse-engineering the
decrypted game code showed this is governed by four `[Net]` values the host
reads at match start:

| Rule | Game default | Meaning |
|---|---|---|
| `AutoKickStart` | `10000` ms | grace period after a player joins before monitoring starts |
| `AutoKickPing` | `750` ms | ping ceiling; a tick above it is "bad" |
| `AutoKickLoss` | `25` | per-tick loss-count ceiling; above it is "bad" |
| `AutoKickTime` | `15000` ms | how long the connection must stay continuously bad before the kick |

A player is kicked once their connection has been bad (ping over `AutoKickPing`
**or** loss over `AutoKickLoss`) continuously for `AutoKickTime`, measured only
after the `AutoKickStart` grace. **Auto-kick is enforced by the session host**,
so only the *host's* values matter — a client setting these has no effect on
whether the host kicks it.

The relax preset is **on by default** (set `BZ_AUTOKICK_RELAX=0` to restore
stock kicking, or the individual `BZ_AUTOKICK_*` vars for custom values),
raising these ceilings so a transient lag spike no longer ends someone's game.
It defaults on because the `net.ini` route proved unreliable in live play: a
host running the `packaged_mods/9990001/net.ini` with `AutoKickTime = 45000`
still fired a stock 15 s kick (2026-07-05) — the game logs `MOD FOUND net.ini`
but does not reliably apply its values unless the file ships inside the
session's *active* mod.
Like `BZ_GOV_START` it's a **data-only** patch: the proxy waits for the game to
decrypt, confirms the build via the governor signature, then writes the live
threshold globals and re-asserts them every 100 ms — it never touches `.text`,
so SteamStub's integrity check is untouched. Because it re-asserts continuously
it also overrides any value the fragile `net.ini` mod path would have set.
Confirm with `autokick_patch: enabled` and `autokick_patch: version confirmed`
in `dsound_proxy.log`.

## Kernel Socket Buffer Limits (Required for Full Effect)

The Linux kernel silently clamps `setsockopt(SO_RCVBUF/SO_SNDBUF)` to
`net.core.rmem_max` / `net.core.wmem_max` (~208 KB on most distros).
Without raising them, the 4 MB receive buffer is mostly fictional — and the
Wine `getsockopt` readback can still report the requested value, so the
proxy log alone does not prove the kernel honored it.

```bash
sudo sysctl -w net.core.rmem_max=4194304 net.core.wmem_max=524288
printf 'net.core.rmem_max=4194304\nnet.core.wmem_max=524288\n' | sudo tee /etc/sysctl.d/99-battlezone-netcode.conf
```

The installer (`install/install_linux.sh`) offers to do this automatically.
Inspect the live socket while the game runs with `ss -uampn`.

## Steam Launch Options

```
WINEDLLOVERRIDES=dsound=n,b %command% -nointro
```

## Current Limitations

- Proton-specific: depends on Wine loading a local native `dsound.dll`.
- Windows uses a separate implementation in `Microslop/winmm_proxy/`.

Linux and Windows now ship as separate startup-interception paths with matching socket buffer targets and matching reorder/tuning env vars.