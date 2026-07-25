# Changelog

## V4.7 (current)

**Reorder-buffer correctness pass, the whole `[Net]` block written directly into
the game, outbound measurement, and Windows IOCP groundwork.**

### Reorder buffer: eight defects fixed

An audit found eight defects in the reorder path. All of them existed twice,
because the Linux and Windows proxies carried character-identical copies of the
logic. The state machine now lives once, in `shared/reorder_core.h`.

- **Burst overflow was destroyed.** The drain pulled up to 96 datagrams per call
  into 8-slot queues and discarded whatever did not fit — so under exactly the
  bursts the buffer exists to survive, the patch threw away packets an unpatched
  game would have received. The drain now stops when storage fills, leaving the
  rest in the 4 MB kernel buffer where they keep their order for free, and an
  evicted packet is handed to the game rather than dropped.
- **The sequence cursor could walk backwards.** Superseded packets were
  buffered, released out of order anyway, and then dragged `last_seq`
  *backwards*, corrupting tracking for everything after. They are now rejected
  at insert, the cursor only advances, and packets that go stale while queued
  are purged.
- **The hold window ratcheted and never came down.** Growth doubled (5 → 100 ms
  in four events); decay was 5 ms per 2 s, so recovery took ~38 seconds and one
  burst pinned a peer at the ceiling for the rest of a match. Growth is now
  sized by the lateness actually measured; decay is proportional.
- **`BZ_REORDER_MAX_HOLD_MS`** hard-caps added latency independently of the
  adaptive window.
- Oversized datagrams are counted rather than silently ending the drain.
- Idle peer entries are reclaimed instead of bypassing the buffer forever.
- The delivery scan is round-robin, so no peer is starved in a mesh.
- **`reorder_stats` counters**, including `hold_ms(avg/max)` — the latency the
  patch itself adds, which no previous test could measure.

Capacity retuned: 32 slots × 16 peers (was 8 × 32), 2048-byte packet buffer
(was 1500).

### The `[Net]` block, poked directly

The `.data` poke was generalised from the four auto-kick thresholds to all ten
reverse-engineered `[Net]` tunables (`shared/net_globals.h`). `net.ini` has
twice been observed *found but not applied*, so the patch stops relying on it.

Each address now carries a plausibility range and is sanity-gated on first
contact: an implausible value means the address is wrong for this build, and the
entry is logged and skipped rather than written. The previous code wrote four
fixed addresses blind.

Defaults mirror `net-ini/net.ini` and are on (`BZ_NET_TUNE=0` restores stock).
`BZ_GOV_START` now defaults to 16000 after sitting verified-but-disabled since
V4.5.

### Outbound measurement and pacing

Burst measurement on the send path, always on (`send_stats`), plus an optional
token-bucket pacer (`BZ_SEND_PACE`, off by default) that never drops a packet,
never reorders one, and never delays traffic too small to carry a sequence
number — that being the ping exchange the auto-kick measures.

### Windows IOCP

A read-only `BZ_IOCP_SCAN` diagnostic, and an opt-in `BZ_IOCP_REORDER` path that
defers completions rather than swapping buffers. Unvalidated — see Known Limits
in the README.

### Tooling and tests

`tools/analyze_drops.py` gained warp-magnitude bucketing, governor range,
auto-kick events, and a fix to its duplicate-detection heuristic, which used an
absolute echo count and so mislabelled healthy 8-minute sessions as duplicating
and halved their drop counts.

This repo's first automated tests: `make -C tests run` — 30 cases, 506
assertions, one per fixed defect, no game required.

## V4.6

Auto-kick relax on by default (`BZ_AUTOKICK_RELAX`), with `BZ_AUTOKICK_*`
host-side overrides.

## V4.5

**Send-governor cold-start fix (`BZ_GOV_START`).** The governor hardcodes a
4000 B/s start for every match — the root of the first-60-seconds drop clusters.
The DRM-decrypted code was dumped at runtime and the exact site located;
rewriting the constant in `.text` works but SteamStub's integrity check then
kills the game, so the fix is a data-only patch that lifts the live send-rate
value off 4000 without touching game code. Sender-side, so it also improves how
traffic reaches unpatched peers. (Opt-in at the time; on by default since V4.7.)

## V4.4

**`BZ_SEND_DUP` deprecated.** A ~10-game A/B series (1v1s and 2v2s, logs from
all peers) settled it: outbound duplication does not help this game and degrades
busy uplinks — one link went 3.6 → 47.9 drops/min with it on, because at
Battlezone's ~30 pkt/s the rate cap rarely engages so it still roughly doubles
packet load. Removed from all recommended launch options and installer prompts;
still present, opt-in, off. Verdict and method:
[`test-logs/2026-07-03_dup_test_summary.md`](test-logs/2026-07-03_dup_test_summary.md).

## V4.3

Reworked `BZ_SEND_DUP` (loopback-skip, time-shifted and rate-capped copies),
added `BZ_DSCP` priority marking, ported the `setsockopt` re-force hook to
Windows, added the opt-in `BZ_GOV_SCAN` diagnostic.

## V4.2

Reorder ceiling raised 45 → 100 ms after live A/B testing measured ~65% fewer
drops (121 → 40/43 on the same map and opponent). `net.ini` now installs as a
local packaged mod.

## V4.1

**Windows launch-freeze hotfix** — the hook was routing the game's overlapped
(IOCP) receives through the synchronous reorder path, hanging the game at the
loading screen. Proton was unaffected; the bug had existed since V3. Also
ordinal IAT patching, and `BZ_SEND_DUP` moved to the `WSASendTo` hook.

## V4

Adaptive reorder window (5 ms floor), wake thread for stranded packets, Linux
kernel-clamp fix in the installer, Windows/Linux tuning parity, `BZ_SEND_DUP`,
drop metrics in the verify script.

## V3

In-proxy out-of-order packet reordering (`WSARecvFrom` hook), per-peer buffering
with deterministic sequence release. Sequence field located at `payload[13..16]`
(u32le) via binary capture analysis.

## V1–V2

Forced bigger UDP socket buffers (final: 512 KB send / 4 MB receive).
