# Throttle Test — Validating the Auto-Kick Relax

The V4.6+ host-side auto-kick relax (`AutoKickPing=2000`, `AutoKickTime=60000`
and friends) has never been observed working: no kick event appears in any log
since it shipped. This test manufactures one, deliberately, by degrading one
client's connection in two calibrated phases. Phase 1 proves the relaxed host
*tolerates* what a stock host would kick; phase 2 proves the relaxed limits
are still *enforced* past the new threshold.

One volunteer client runs the throttle (Linux instructions below — `tc netem`
needs root). Everyone else just plays.

## Ground rules

- **The host runs the patched DLL** (that's what's being tested — auto-kick is
  host-enforced).
- Start throttling only **after the match is 60+ seconds old** — kicks are
  disabled before `AutoKickStart` expires.
- The throttle degrades the volunteer's **entire machine**, voice chat
  included. Coordinate phases over text.
- Announce "phase 1 now" / "phase 2 now" in the channel with rough
  timestamps, so the log timeline can be lined up afterwards.

## Setup (volunteer, before the match)

Find your network interface name:

```bash
ip route get 1.1.1.1 | grep -oP 'dev \K\S+'
```

Use that name in place of `IFACE` below.

## Phase 1 — bad but tolerable (expected: NOT kicked)

A couple of minutes into the match:

```bash
sudo tc qdisc add dev IFACE root netem delay 800ms 200ms loss 10%
```

Ping lands around 800–1000 ms with 10% outbound loss — past the stock kick
thresholds (a stock host ejects this in ~10–15 s) but under the relaxed
2000 ms ceiling. **Hold for a full 2 minutes.**

Expected on a patched host: heavy rubber-banding, possibly flagged "lagging",
but no kick. **A kick here means the relax is not holding — that is a
finding, not a failed test.**

## Phase 2 — beyond the relaxed limit (expected: kicked in 60–90 s)

```bash
sudo tc qdisc change dev IFACE root netem delay 2500ms 300ms loss 20%
```

This is past the relaxed 2000 ms threshold. The relaxed timer needs the link
bad for 60 continuous seconds, so expect the kick in the 60–90 s range.

**Getting kicked here is the success case** — it proves the relaxed
thresholds are enforced, not just written into memory. Still in after
3 minutes? Also a finding.

## Cleanup — immediately after, or if anything goes sideways

```bash
sudo tc qdisc del dev IFACE root
```

Connection is instantly normal. `RTNETLINK answers: No such file or
directory` just means there was nothing to remove. If the game disconnects
mid-test, run the cleanup **before** rejoining.

## Afterwards

Exit the game normally on both ends — the wrappers upload everything. The
evidence lands in the **host's** bundle: the first kick event ever captured
under the relaxed thresholds. The volunteer's phase announcements in the
channel are what line the log timeline up.

Prefer running the throttle from a native (non-sandboxed) install where the
choice exists — it is the environment with the most measurement history.
