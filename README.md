# Battlezone 98 Redux Netcode Patch

Battlezone drops any UDP packet that doesn't arrive in *exact* sequential order,
even by a millisecond. WiFi, wireless, international, anything with mild jitter —
you're not losing those packets to the network, you're losing them to a
sequencing rule that tolerates zero deviation.

This patch sits in front of the game as a DLL proxy. It writes the game's own
network tuning directly into memory, forces bigger socket buffers, and marks
your traffic for router priority. The game never knows it's there.

The single biggest win is lifting the send governor: stock `MaxBandwidth` is
**16,000 B/s**, and measured sessions sat pinned at exactly that. Raising it took
the same matches to 82,000-113,000 B/s and cut visible position corrections by
55-70% on identical maps.

**Everyone in the lobby should install it.** The tuning is per-machine, so your
install fixes your outbound rate and your inbound buffers, nobody else's.

> **On the reorder buffer.** Earlier versions led with an inbound packet
> reordering buffer. As of 2026-07-26 it ships **off by default**: measurement
> showed the game receives via overlapped I/O that never reaches it, and that
> out-of-order arrivals on real links run at 0.0-0.2% — the traffic problem is
> duplication and loss, which reordering does not address. See
> [Known Limits](#known-limits).

Current version: **V4.8** — see [CHANGELOG.md](CHANGELOG.md).

---

## Install

### Windows

Paste into PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -Command "irm https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_windows.ps1 | iex"
```

Finds your install, downloads the SHA256-verified `winmm.dll`, and installs the
tuned `net.ini`. No launch options to set. Launch the game normally.

> **Defender note:** some users see `winmm.dll` quarantined as
> `Program:Win32/Contebrew.A!ml` — a heuristic detection common for unsigned DLL
> proxies. Restore it from Protection History and add an exception for that one
> file in the game folder. Don't disable AV globally.

### Linux / Proton

**Step 1** — paste into a terminal:

```bash
curl -fsSL https://raw.githubusercontent.com/PiercingXX/battlezone-netcode-patch/master/install/install_linux.sh | bash
```

It detects your install (native, Snap or Flatpak Steam), builds `dsound.dll`
from source, offers to raise the kernel UDP buffer limits, and installs the
tuned `net.ini`. Set `BZNET_ASSUME_YES=1` to skip the prompts.

**Step 2** — set Steam launch options for Battlezone 98 Redux:

```text
WINEDLLOVERRIDES=dsound=n,b %command% -nointro
```

Without step 2 the DLL is never loaded. That's the whole install.

### Manual install

Prefer to do it by hand, or need a second install patched? See
[docs/MANUAL_INSTALL.md](docs/MANUAL_INSTALL.md).

---

## Check it's working

Play one multiplayer match, quit, then run the verify script.

**Linux:**

```bash
cd "/path/to/Battlezone 98 Redux"
/path/to/battlezone-netcode-patch/Linux/verify_net_patch.sh
```

**Windows:**

```powershell
.\Microslop\verify_windows.ps1
```

Expect `VERIFY RESULT: PASS`. If you'd rather eyeball it, open the proxy log
(`dsound_proxy.log` or `winmm_proxy.log`, next to the game exe) and look for:

| Line | Means |
|---|---|
| `effective readback SO_SNDBUF=524288 … SO_RCVBUF=4194304` | socket buffers applied |
| `net_patch: version confirmed` | the game build was recognised |
| `net_patch: MaxBandwidth 4000 -> 320000` | tuning is being written into the game |
| `governor_patch: cold-start caught, send-rate 4000 -> 40000` | the match opened at the raised rate |
| `send_stats: …` (every 10s) | outbound traffic is being measured |

`reorder: DISABLED` is expected — the buffer is off by default. `reorder_stats:`
lines only appear if you set `BZ_REORDER=1`, and even then only if the receive
path reaches the buffer, which on current builds it does not.

**Not working?** `net_patch: … VETOED` or `0 version signature matches` means
the game updated and the memory addresses moved — the patch falls back to stock
behaviour rather than risking anything. Everything else still works. Open an
issue with the log.

---

## Testing: how to collect a useful session

The numbers that matter now are **hold_ms**, **evicted** and **visible warps**,
not just the drop count. Collecting them takes two files per player per game.

### Before you play

1. Everyone installs the patch and confirms `VERIFY RESULT: PASS`.
2. Agree who is hosting. Auto-kick thresholds are enforced by the **host only**,
   so the host's install is what decides whether anyone gets kicked.
3. If anyone is subscribed to the Steam Workshop mod **"Auto-Kick Reduction
   Patch" (`1895622040`)**, unsubscribe. It ships its own `net.ini` that
   overrides the patch's and caps send rate at 32 KB/s. *Disabling it in-game is
   not enough — you must unsubscribe.*

### After each match

> **Critical: `BZLogger.txt` is overwritten every time the game launches.**
> Copy it aside *before* anyone relaunches, or the game is lost.

From the game folder (`…/steamapps/common/Battlezone 98 Redux/`), copy both:

- `BZLogger.txt` — the game's own log (drops, warps, kicks, governor)
- `dsound_proxy.log` (Linux) or `winmm_proxy.log` (Windows) — the patch's counters

Rename them per player and per game, e.g. `game3_piercingxx_BZLogger.txt`.

### Sending logs

**Zip and attach the files.** Pasted logs always truncate — these run to tens of
megabytes.

### Reading the results

```bash
python3 tools/analyze_drops.py game3_piercingxx_dsound_proxy.log game3_piercingxx_BZLogger.txt
```

| What it reports | Healthy | What a bad value means |
|---|---|---|
| real drops/min, per peer | low | high on one link only = that peer's uplink |
| `hold_ms max` | well under 100 | **latency this patch is adding to you** — high here is warping and kick risk, not a win |
| `evicted` | 0 | a queue filled and packets went out out-of-order; raise `BZ_REORDER_DEPTH` |
| `emsgsize` | 0 | datagrams were too big for the drain buffer and the stack destroyed them — report this |
| visible warps/min (≥50 m) | low | the actual symptom, as opposed to the raw warp count, which is ~90% sub-metre noise |
| `governor` range | starts ≥16000 | still opening at 4000 means the cold-start fix didn't apply |
| `peak_pps` / `burst_seconds` | low | *your* machine is producing retransmit floods |

A drop-count improvement bought with a large `hold_ms max` is a regression, not
a win. That's the trap every earlier round of testing walked into.

### Testing without the game

```bash
make -C tests run    # 30 cases, 506 assertions, no game required
```

### Optional: raw packet capture

Only needed for deep analysis — see [logging_readme.md](logging_readme.md).

---

## What it does

**Receive side (helps you):**

- **4 MB receive / 512 KB send socket buffers**, re-forced through a
  `setsockopt` hook so the game can't shrink them back.
- **Adaptive per-peer reorder buffer — off by default.** Kept, tested and
  correct, but measurement retired it as a default; see [Known limits](#known-limits).
  `BZ_REORDER=1` turns it on.

**Send side (helps everyone receiving from you):**

- **The game's `[Net]` tuning, written into memory** every 100 ms:
  `MaxBandwidth` 320000, `UpCount` 100, `DownCount` 50, `MaxPing` 450. Stock
  `MaxBandwidth = 16000` is the one that matters most — measured sessions sat
  pinned at exactly that ceiling for entire matches. Stock `MaxPing = 300`
  matters next: a jitter spike makes the governor *cut* your send rate, so fewer
  updates arrive, so you warp more, and the cut sustains the spike. `net.ini`
  was supposed to deliver these but has twice been caught found-but-not-applied.
  `MinBandwidth` is deliberately **not** written — a live A/B showed writing it
  changed nothing, and the address is unconfirmed.
- **Governor cold-start fix.** The game opens every match at a hardcoded
  4000 B/s trickle; this lifts it to 40000. This, not `MinBandwidth`, is what
  sets the opening rate. It matters more than it looks: a measured match sat at
  the opening rate for 72 seconds *after* the simulation started, then took
  4.7 minutes to reach 80 kB/s.
- **DSCP priority marking** (EF/46) so a WMM or SQM router serves your game
  traffic ahead of bulk downloads. Real effect under Proton; a no-op on stock
  Windows.
- **Outbound burst measurement** (`send_stats`), always on.

**Host only:**

- **Auto-kick relax.** A player is ejected once their connection stays bad for
  15 s (stock). Widened to 60 s with a 2 s ping ceiling, so a transient spike no
  longer kicks anyone.

All memory writes are data-only and DRM-safe — no game code is modified — and
every address is sanity-checked before it's written.

---

## Tuning

Everything above has an environment variable, and the defaults are meant to be
what you want. Full tables:

- [Linux / Proton proxy](Linux/proton_dsound_proxy/README.md)
- [Windows proxy](Microslop/winmm_proxy/README.md)
- [net.ini (superseded, kept as fallback)](net-ini/README.md)

The ones worth knowing:

| Variable | Default | Effect |
|---|---|---|
| `BZ_NET_TUNE` | `1` | `0` restores the game's stock governor behaviour |
| `BZ_AUTOKICK_RELAX` | `1` | `0` restores stock auto-kicking |
| `BZ_GOV_START` | `40000` | opening send rate; `0` restores the stock 4000 B/s cold start |
| `BZ_REORDER` | `0` | `1` enables inbound reordering (off by default since 2026-07-26) |
| `BZ_NET_MINBANDWIDTH` | unset | not written by default; set a value to re-test it |
| `BZ_REORDER_STATS` | `1` | `0` silences the 10-second counter lines |
| `BZ_SEND_PACE` | `0` | bytes/sec outbound smoothing; off, measure first |
| `BZ_IOCP_SCAN` | `0` | Windows: log which completion API the game uses |

---

## Known limits

- **It fixes ordering, not loss or congestion.** No receiver-side patch can fix
  a saturated uplink on the *sending* peer's end. That's a wired-ethernet or
  router-QoS problem for them.

- **Nobody gets inbound reordering — including under Proton.** This was
  believed to be a Windows-only gap until 2026-07-26, when a wire capture
  settled it. The game issues **overlapped** `WSARecvFrom` on every receive
  (28% return `WSA_IO_PENDING`), and the reorder hook deliberately bypasses
  overlapped calls — routing them through it froze the game at the loading
  screen back in V4.1. Across a 2.5-hour Proton session the buffer handled
  **zero packets** and emitted no `reorder_stats` at all. Every measured
  improvement to date belongs to the `[Net]` tuning, the socket buffers and
  DSCP marking. `BZ_IOCP_REORDER=1` exists but has never run on real Windows.

- **…and on these links there was nothing to reorder.** The same capture put
  out-of-order arrivals at **0.0-0.2%** across all three packet classes,
  independently corroborated by the game's own log: 6,998 of 7,012 discards were
  packets *already consumed*, and only 14 arrived early. What the links actually
  show is duplication (56-83% of inbound) and outright loss (10-27% of sequence
  numbers never arriving) — neither of which a reorder buffer can fix. That is
  why it now ships off, and why building an IOCP receive path to reach it is not
  currently worth the risk.

- **Reordering is not free, and the drop counter hides the cost.** Holding a
  packet adds latency, and the game's drop count stops counting a packet the
  moment it's held — whether or not you're better off. The old "65% fewer drops"
  headline was never the whole story. Score an A/B on visible warps per minute
  and retransmits per MB, not on the raw discard count — and note that warp
  rates are strongly map-dependent, so only ever compare the same map.

- **`MinBandwidth` and the sequence offset were both wrong, and both were
  believed for weeks.** The `MinBandwidth` address does not set the opening send
  rate as its comment claimed, and the reorder sequence field was read as u32
  little-endian at offset 13 when it is u16 big-endian at offset 16. Both are
  corrected in the source, but treat the remaining reverse-engineered addresses
  in `shared/net_globals.h` as provisional: the sanity gate only rejects wildly
  implausible values, not a plausibly-wrong address.

- **Ping packets aren't yet exempt from receive buffering.** If ping replies
  ride the ordered queue, the buffer inflates the round-trip time a host
  measures against the kick threshold. `BZ_REORDER_MAX_HOLD_MS` caps the damage;
  a proper fast lane needs one capture session to identify the packet type.

- **The memory addresses are pinned to one game build.** If Rebellion patches
  the game, expect `net_patch: … VETOED` and stock behaviour — not a crash, but
  not the fix either.

- **A workshop mod shipping `net.ini` overrides the patch's copy.** Unsubscribe;
  disabling in-game isn't enough.

---

## Things we learned the hard way

- **`net.ini` only loads through the mod system — and even then, found ≠
  applied.** A copy next to the exe is silently ignored. Delivered as a local
  mod the game logs `MOD FOUND`, yet a 2026-07-05 match proved the values still
  weren't used: the host ran `AutoKickTime = 45000` and kicked at the stock 15 s.
  Working theory is that only the session's *active* mod (the map's) is parsed.
  This is why V4.7 writes the values into memory instead.
- **The send governor runs on every machine**, not just the host's — contrary to
  long-standing community wisdom. Only auto-kick is host-enforced.
- **The raw warp count means nothing.** ~90% of `Possible Large Warp` lines are
  sub-metre corrections. Filter by distance or you're measuring noise.
- **The game imports winsock functions by ordinal** and sends all P2P traffic via
  `WSASendTo`; `sendto` isn't in its import table at all.

Full research notes: [resources/](resources/) ·
[test-logs/](test-logs/)
