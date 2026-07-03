# Beyond dup: what else can we patch? (research, 2026-07-03)

Question: the reorder buffer and send-dup both operate at the winsock boundary.
Are there other valid angles — can we patch a different part of the game?

## The decisive constraint, and the door it leaves open

`battlezone98redux.exe` is wrapped in **SteamStub DRM** (the `.bind` section): the
`.text` section is encrypted on disk and only decrypted in memory at launch.
Verified empirically: the governor's log strings and all four net.ini keys
(`UpCount`, `DownCount`, `MinBandwidth`, `MaxBandwidth`) are plainly visible in
`.rdata` (e.g. `"Net: Bandwidth usage now set to %d, Interval %d ms"` at VA
0x883CE0), yet a scan of the entire on-disk `.text` finds **zero** code
references to those addresses — impossible for live code, expected for
encrypted code.

Consequences:

- **Static exe patching: not viable** (would break DRM checksum, and you can't
  even find the code to patch).
- **Runtime memory patching: fully viable.** Our proxy DLL (`dsound.dll` /
  `winmm.dll`) is loaded into the process and runs after the stub has decrypted
  `.text`. It can signature-scan the live image and patch bytes/data in memory,
  per-launch, no file modification, update-tolerant if the scan is
  signature-based rather than address-based.

Everything below is ranked by expected value against the measured problem
(queueing-delay bursts + a tiny, slow-ramping send budget), not by novelty.

## A. Send-governor tuning — config first, then memory patch

The governor starts hardcoded at **4000 B/s** and ramps slowly (+10..+160 per
~3 s adjustment; a 4-minute game only reached 9880). That is dial-up-era
pacing: the game is starved of state-update budget for the entire first half
of a typical match, which magnifies the cost of every lost/stale packet.

1. **Zero-code experiment first: `UpCount`/`DownCount` in net.ini.** These keys
   exist in the binary and net.ini is already loaded via the packaged-mod
   mechanism. If `UpCount` controls the ramp increment, a config change may buy
   most of the win. Untested to date — cheap A/B next session.
2. **Runtime patch of the initial 4000** (and, if needed, the ramp step) from
   the proxy: signature-scan the decrypted `.text`/`.data` at startup, log the
   candidate site, patch after one verified run. The proxy already has the
   logging/env-var infrastructure to gate this (`BZ_GOV_START=16000` style).

Risk: low-moderate. The governor paces *sending* only; peers already run with
mismatched governor settings today (a workshop net.ini ships MaxBandwidth=32000),
so there is no sync-model reason a faster ramp should desync anything. Verify
with the standard A/B protocol.

## B. Make the reorder buffer work on real Windows (IOCP path)

On real Windows the game uses overlapped/IOCP receives, which our hook
deliberately bypasses (the V4.1 freeze taught us why). Result: **most players
(Windows) have no reorder protection at all** — KFK's and Monkey's receive
columns in the 2v2s are vanilla behavior. Options, in increasing invasiveness:

- Hook `WSARecvFrom`'s overlapped path by substituting our own buffer and
  completing the game's overlapped request from the reorder queue.
- Hook `GetQueuedCompletionStatus` / `WSAGetOverlappedResult` and reorder at
  completion-delivery time.

High effort, delicate (loading-screen freeze regression risk), but it's the
single biggest untapped *receive-side* improvement for the community. Note the
ceiling measured on 2026-07-03: under congestion bursts, reordering barely
helps (63 vs 72/min on the same stream) — this pays off in mild-reorder
conditions like the original 65% Grizzly Gulch win, not under bufferbloat.

## C. QoS/DSCP marking (cheap, targets the actual mechanism)

Mark the P2P socket EF/DSCP-46 so home-router WMM (WiFi voice queue) and
SQM/fq_codel deprioritize bulk traffic behind game packets:

- Proton/Linux: `setsockopt(IP_TOS)` from the dsound proxy — trivial.
- Windows: `IP_TOS` is ignored by policy; needs the qWAVE API
  (`QOSCreateHandle`/`QOSSetFlow`) — moderate but documented.

Complements: a docs section telling players to enable SQM (fq_codel/CAKE) on
their routers — the bufferbloat literature says this is the real fix for the
delay-spike mechanism we measured.

## D. Pace *all* outbound sends (not just duplicates)

The dup pacer infrastructure (v4.3) can be generalized: token-bucket smoothing
of the game's own send bursts so multi-packet volleys don't slam a bloated
queue all at once. A few ms of added send latency, bounded. Do only after A/B
shows burstiness matters with the governor fixed (A may change send patterns).

## E. Proxy-to-proxy FEC (phase 2, highest ceiling)

UDPspeeder-style parity (XOR or Reed-Solomon over N-packet groups, interleaved)
between patched peers; receiver-side reconstruction injects recovered packets
through the existing reorder buffer. ~10–30% overhead instead of dup's 100%,
and it recovers loss without the PPS doubling that sank always-on dup.
Prerequisites: a wire format that unpatched receivers provably drop harmlessly
(craft a packet type BZRNet discards without side effects — needs a controlled
test), plus capability negotiation between proxies.

## F. Patch the receiver's stale-drop check — rejected for now

Delivering late packets the game would drop risks feeding stale state into
whatever consistency model BZRNet runs ("Sync: On" lobbies suggest lockstep
elements). Without real reverse-engineering of packet semantics, do not touch.

## G. Out-of-game: tunnels for chronic cases

For a persistently bad link (KFK), a WireGuard or UDPspeeder tunnel between the
two players fixes loss with FEC today, no game patching at all. Setup burden
per pair; document as a power-user option, not a product direction.

## Small parity item

The winmm proxy still lacks the `setsockopt` re-force hook the dsound proxy
has, so on real Windows the game caps its own send buffer back to 32 KB.
Harmless at current rates but should be ported for parity — one function.

## Recommended order

1. **A1** — `UpCount` net.ini A/B (config-only, next play session).
2. **v4.3 dup re-test** (already shipped) in the same session.
3. **C** (Linux `IP_TOS` + docs) and the **winmm setsockopt parity** — small.
4. **A2** — runtime governor patch (scanner in proxy, log-only first run).
5. **B** — IOCP reorder for Windows (big lift, big audience).
6. **E** — FEC (phase 2).
