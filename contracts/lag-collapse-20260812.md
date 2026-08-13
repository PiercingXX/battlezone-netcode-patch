# The 2026-08-12 lag collapse — four fixes

Claude-authored (2026-08-12) from the `new logs/8-12` bundles: PiercingXX
(client, V4.92 proxy) and the second player (host, an 08-02 build), the
`xxMonke1.bzn` match, two players, 17.7 minutes.

Reproduce the headline numbers with:

```bash
python3 tools/analyze_drops.py <client>/BZLogger.txt <client>/dsound_proxy.log
```

BRANCH LAW: all work lands on experimental/v4.9 — NEVER master.

## What happened

Between **20:34:09 and 20:36:23** the client put **30,691 retransmitted
datagrams / 2.64 MB** on the wire, peaking at 52.9 kB/s against a governor
budget that had collapsed to 4.5 kB/s. Decoding the payload bodies, **30,007 of
them (97.8%) are four objects**: `apamorep804/805/806/807_repairkit`. Four
armour-repair pickups, each emitting ~2,000 *distinct* position updates on the
reliable channel over two minutes — roughly 17 state changes per second each,
multiplied by BZ's 3.22 proactive copies per message. They stop in the same
datagram batch at 20:36:22.888, two seconds after the client's ship is
destroyed.

The flood held the governor in an **uninterrupted 108-second down-ramp**: 54
consecutive steps, 25,900 → 4,150 B/s, no up-step in between. At 4,150 it hit
the game's stock floor, landed on exactly 4,000, and the patch's own cold-start
watcher read that as a match start and slammed it to 40,000.

The root cause — four pickups in perpetual motion — is a game or map-mod
behaviour (workshop 3781900699). Nothing in this repo can stop the engine
queueing distinct reliable messages. Everything below is about not turning that
into a two-minute outage.

## Rules for this marathon

Every task ships a test that reproduces its defect (red before, green after).
C tests run under `make -C tests run` at BOTH 64-bit and 32-bit; Python tests
run by their named file. The final gate is `make -C tests run` green at both
word sizes AND both Python tests green AND CI green. Do not weaken any existing
test.

## Tasks

- [x] Turn the duplicate suppressor on by default in both proxies, using the
  reorder-style "env absent = default" idiom so `BZ_SEND_DAMPEN=0` still
  disables. It shipped off in V4.93 pending a live-match validation; 2026-08-12
  supplied one. Replaying the logged send stream through `dampen_admit()`
  suppresses 63.9% of the storm's datagrams at the 60 ms floor window and 69.0%
  at a realistic 1.2×RTT window — 3.22 copies per message down to one.
  - verify: `./tests/send_dampen_test` and the shipped default in both proxy logs
  - files: Linux/proton_dsound_proxy/src/dsound_proxy.cpp,
    Microslop/winmm_proxy/src/netcode_hooks.cpp

- [x] Gate the cold-start sentinel against mid-match floor hits.
  `gov_trace_step()` fired `kGovBumped` on any `live == 4000`, on the written
  assumption that the ramp "moves it off 4000 immediately and never returns".
  The host's proxy logged `poke held ... reads 38000` at 20:36:03 — a bump at
  20:35:53, thirteen minutes into the match. Classify the sentinel by arrival:
  a read reached from within `descent_band` bytes above it, off a value held
  less than `descent_ms`, is the new `kGovFloorRescue`, which still raises the
  rate but is counted apart from real match starts. This also fixes the
  telemetry: the analyzer reported 32 "matches verified held" for an evening
  with three matches in it.
  - verify: `./tests/gov_trace_test`
  - files: shared/gov_trace.h, tests/gov_trace_test.cpp, both proxies

- [x] Reconcile the governor ramp asymmetry. Measured off the collapse: down
  −203 B/s per second, up +40.5 B/s per second — 5:1, so a two-minute collapse
  needs nine minutes to undo. Stock is Up=10/Down=5, i.e. up twice as fast as
  down; the shipped preset was Up=50/Down=200, an 8× inversion of the stock
  bias. The host and client were also on different pairings (100/50 vs 50/200).
  Reconcile to Up=100/Down=50 across net_globals.h, net-ini/net.ini and both
  proxy READMEs, and pin the 2:1 recovery invariant in the test.
  - verify: `./tests/net_globals_test`
  - files: shared/net_globals.h, net-ini/net.ini, tests/net_globals_test.cpp,
    both proxy READMEs

- [x] Assert MinBandwidth as the collapse floor. `kNetTunePreset` left it at 0
  because the 2026-07-26 A/B disproved it as the *opening* rate — a different
  question from whether it floors a collapse, which no session in that set had
  produced. 2026-08-12 did, and bottomed out at the stock 4,000. Set the preset
  to 16000 (the value net-ini/net.ini has documented all along), narrow the
  sanity gate now the as-found values are known, and pin the invariant that the
  floor clears the cold-start sentinel and its descent band — which is what
  makes task 2 the backstop rather than the mechanism.
  - verify: `./tests/net_globals_test`
  - files: shared/net_globals.h, net-ini/net.ini, tests/net_globals_test.cpp

## What is NOT settled

MinBandwidth is a live experiment on an address that is identified but not
confirmed. `BZ_NET_MINBANDWIDTH=0` reverts it. **One collapse observed with this
on settles it**: if the governor bottoms out at 16,000 instead of 4,150, the
address is what we think it is and the floor works. The number to read is the
proxy's `governor_trace` window minimum, which is where the 4,150 was seen.

The per-step B/s figures behind task 3 are what the ramp did at 50/200. That
they scale linearly with the knobs is inferred from the step sizes in the log,
not separately measured.

`tools/analyze_drops.py` cross-contaminates its retransmit-share denominator
when two bundles are passed on one command line: both reports used the client's
proxy byte counter (pid 828 / 24.60 MB) instead of the host's (pid 628 /
31.35 MB). Not fixed here — it is a measurement bug, not a lag fix, and it will
distort the A/B that scores these four. Fix it before scoring anything.

## Final gate

`make -C tests run` green at 64-bit and 32-bit, both Python tests green, and CI
green on experimental/v4.9.
