# Changelog

## V5.0 — first shipped release

**Nine instrumented matches on 2026-08-15 found the lag, fixed it, and
verified the fix live. This release promotes the result to `master` and
retires the experimental branch.**

### The fix that mattered

The engine's retransmit loop is frame-locked: it may resend every unacked
reliable message once per frame, so a 180 fps machine emits copies every
4 ms — up to ~27 copies of one message inside a single round trip (measured:
28.94). One traffic spike snowballed into sustained storms of 14,000–58,000
retransmitted datagrams per match, congestion collapse, and the lag players
felt. Two defects let it through the duplicate suppressor that V4.94 shipped
to stop exactly this:

- The suppressor's RTT feed was never connected, so its window sat at the
  60 ms floor — 3x the copies the 1.2xRTT design window allows at the
  measured 149 ms. The proxy now measures per-peer RTT from the protocol's
  own ack field (Karn-filtered, reliable stream only) and feeds it in live.
- Its 64-slot sequence ring held ~6 s of storm churn against retries
  spanning 9.5 s; retries of evicted sequences looked like peer restarts and
  silently wiped the suppression state mid-storm. The ring is now 512 slots
  and the wipes are counted (`epoch_resets`).

Verified across both host/client roles: 24–43x fewer retransmissions, and a
1-second frame stall that would previously have ignited a storm was absorbed
and recovered inside 60 seconds.

### Also in V5.0

- **RTT sampling** (`rtt_trace:` every 15 s + session summary), on by
  default, observation-only. It measured the testers' real link at 25–48 ms
  and proved the storms were self-inflicted, not the network.
- **Auto-kick dialled back to ~2x stock** (20000/1000/50/20000). The V4.9
  relax (60000/2000/200/60000) abolished the engine's amputation reflex: a
  2026-08-15 collapse left a visibly dead match running for five minutes
  because nothing would kick. Spikes are still forgiven; corpses are not.
- **MaxBandwidth 320000 → 64000.** Nine matches never measured a send rate
  above 24,872 B/s; the "uncapped" headroom was untested surface. 64000 is
  4x stock and ~2.5x the highest rate ever observed.
- **send_dup retired.** Live A/B showed outbound duplication doesn't help
  this game and degrades busy uplinks. The knob is no longer honoured.
- **Installers refresh the log uploader on every run** (webhook or not), and
  print the old → new wrapper version, ending silent version drift.
- Repo slimmed for release; dev-only captures and scratch files removed.

### Known engine issue (not fixed by this patch)

An armory launch's "Scrap Impact Zone" marker can fail to replicate (its
object class ships without a model file — `impactzn.sdf` is missing) and the
event ignites a message-generation storm the suppressor can only blunt, not
cure. One occurrence collapsed a match beyond recovery. Until it's
understood, avoid armory launches / Day Wreckers in multiplayer.

## V4.94 (experimental branch)

**The first build whose defaults change on the strength of a measured field
event. The 2026-08-12 `xxMonke1.bzn` match produced a two-minute bandwidth
collapse that the logs explain end to end: four runaway mod objects, a governor that
cut five times faster than it recovered, a floor that was never being written,
and — reading the sentinel that the governor collapsed onto — a bug in this
patch that mistook the collapse for a match start. All four are addressed;
the damper ships on.**

Full derivation, with every number reproducible from the bundled logs:
`contracts/lag-collapse-20260812.md`.

### What the session showed

Between 20:34:09 and 20:36:23, the client put **30,691 retransmitted datagrams
/ 2.64 MB** on the wire. Decoding the payload bodies, **30,007 of them (97.8%)
are four objects** — `apamorep804/805/806/807_repairkit` — each emitting ~2,000
*distinct* position updates on the reliable channel over two minutes, at BZ's
3.22 proactive copies per message. Peak 52.9 kB/s against a governor budget
that had by then collapsed to 4.5 kB/s: **11× over**, because retransmits are
not subject to the budget. They stop in one datagram batch at 20:36:22.888, two
seconds after the client's ship is destroyed. `apamorep798`, spawned 21 seconds
earlier, appears exactly once — that is what a normal pickup costs.

The root cause is a game or map-mod behaviour (workshop 3781900699). No proxy
can stop the engine queueing *distinct* reliable messages; it can only suppress
duplicate copies of them, which is what changes below.

### The send damper is on by default

`BZ_SEND_DAMPEN` shipped off in V4.93 pending a live-match validation. The
2026-08-12 session supplied one from the wrong side: the damper was not running
while 2.64 MB of redundant traffic went out. Replaying that logged send stream
through `dampen_admit()` unchanged suppresses **63.9% of the datagrams at the
60 ms floor window and 69.0% at a realistic 1.2×RTT window** — the plateau is
the copies-per-message ratio itself, 3.22 collapsing to 1.0. Peak offered load
52.9 → ~16 kB/s.

Both proxies now use the `BZ_REORDER`-style "env absent = default" idiom, so
`BZ_SEND_DAMPEN=0` restores the old behaviour exactly.

### The cold-start sentinel is classified by arrival (`kGovFloorRescue`)

`gov_trace_step()` fired `kGovBumped` on any read of exactly 4000, on the
assumption — written into both proxy banners — that the ramp "moves it off 4000
immediately and never returns to exactly 4000". The host's proxy logged
`poke held … reads 38000` at 20:36:03, which means a bump at 20:35:53:
**thirteen minutes into a match that started at 20:22:41**. The governor had
walked *down* onto the sentinel, and the patch read it as a match start and
raised the rate 10× with nobody asking.

The sentinel is now told apart by how it was arrived at. A match start writes
4000 over a value that has been sitting still for the length of a lobby; a
collapse arrives from within `descent_band` (2000 B/s) above it, off a value
that lasted one governor step. The latter is `kGovFloorRescue`: it still writes
the target — with the game's floor at 4000 the alternative is a match that
spends the rest of its life at 4 kB/s — but it is logged as `FLOOR RESCUE`,
rate-limited to one report per `descent_ms` (30 s), and **counted apart from
real match starts**.

That last part is not cosmetic. Every mid-match floor hit was being counted as a
match: `analyze_drops.py` reported **32 "matches verified held" for an evening
with three matches in it**, and 126 on the host. A sample set counted that way
cannot score an A/B, and several were.

The known limit is stated rather than hidden: a match ending near the floor
followed by a lobby shorter than 30 s would miss one poke, opening at the stock
4000 and saying so in the log. That is the benign direction to fail in.

### The governor recovers faster than it cuts again

Measured off the collapse: down **−203 B/s per second** (25,900 → 4,150 in
107 s, 54 consecutive steps, no up-step in between), up **+40.5 B/s per second**
(23,900 → 29,450 over 137 s). Five to one — a two-minute collapse needs nine
minutes to undo, so in a sustained fight the governor never recovers before the
next spike and the rate just ratchets down.

Stock is `UpCount=10 / DownCount=5`: up twice as fast as down. The shipped
preset was `50 / 200`, an **8× inversion of the stock bias** in exactly the
direction that turns a traffic spike into a collapse. Reconciled to
`100 / 50` — stock's 2:1 recovery bias, keeping the 10× scale-up that is the
point of tuning these at all — across `shared/net_globals.h`,
`net-ini/net.ini` and both proxy READMEs, with the 2:1 invariant pinned in
`net_globals_test`.

The host and client were also on *different* pairings (100/50 vs 50/200)
throughout the evening, which is its own reason to pin one value.

### MinBandwidth is written again, as the collapse floor

`kNetTunePreset` left MinBandwidth at 0 because a 2026-07-26 A/B disproved it as
the *opening* send rate. That result stands, and it answers a different
question: no session in that set had ever collapsed to the floor, so nothing in
it could observe whether the value acts as one. 2026-08-12 collapsed, and
bottomed out at **4,150 → 4,000 B/s — the stock floor**, not the 16000 that
`net-ini/net.ini` has documented all along.

The preset is now 16000. A floor there also puts the governor out of the 4000
sentinel's reach, which makes the fix above the backstop rather than the
mechanism — an invariant now pinned in `net_globals_test` against
`kGovColdStartSentinel + kGovDescentBandDef`, so the two headers cannot drift
into overlapping. The sanity gate narrows from `[500, 2000000]` to
`[500, 100000]` now the as-found values are known and consistent across a
Windows host and a Linux client (1000 at the menu, 4000 at cold start).

**This one is an experiment on an address that is identified but not
confirmed**, and it is written to say so. `BZ_NET_MINBANDWIDTH=0` reverts.
One collapse observed with this on settles it: if the governor bottoms out at
16,000 instead of 4,150, the address is what we think it is. The number to read
is the proxy's `governor_trace` window minimum, which is where the 4,150 was
seen.

### Known, not fixed

`tools/analyze_drops.py` cross-contaminates its retransmit-share denominator
when two bundles are passed on one command line: both reports used the client's
proxy byte counter (pid 828 / 24.60 MB) instead of the host's own (pid 628 /
31.35 MB). Small here (0.5% → 0.4%) but it will distort the A/B that scores
this release. Run one bundle per invocation until it is fixed.

The per-step B/s figures behind the ramp reconciliation are what the governor
did at 50/200. That they scale linearly with the knobs is inferred from the step
sizes in the log, not separately measured.

## V4.93 (experimental branch)

**The retransmit storm has a named cause, a fix handed upstream, and — new in
this build — a mitigation this project controls: a per-`(peer, sequence)`
duplicate suppressor wired into every send path of both proxies. Alongside it,
the six defects an external review found in the estate build are fixed with
reproducing tests, the damper's restart detection is re-derived in-band, and
the repo has CI for the first time.**

### The send damper (`BZ_SEND_DAMPEN`, off by default)

BZRNet's reliable retry timer is fixed at ~10 ms with no backoff, against an
RTT the game itself reports as 56–91 ms, so every reliable message goes out
6–9 times before an acknowledgement can physically return
(`resources/CAMERAPOD_STORM.md`). `shared/send_dampen.h` suppresses only the
redundant in-window copies: a first transmission, a distinct sequence, and
anything too small to carry a sequence number always go, and a genuine
loss-recovery retransmit past its window goes too, with the window doubling
(60 ms floor, 400 ms cap). Suppression looks to the game like a successful
send — the handoff contract a UDP send already means — and the burst
measurement still counts every datagram, suppressed or not.

The wiring gap this entry closes: the damper first landed in one proxy's
`WSASendTo` hook only, and a "suppressed" copy still fell through to the real
send, so nothing was actually dropped. All four hook sites (both send paths ×
both proxies) now suppress before the pacer, skip `BZ_SEND_DUP` duplication of
a suppressed copy, and report a `session end: dampen:` counter line at
teardown on both the closesocket and process-exit paths. Off by default
pending a live-match validation; `BZ_SEND_DAMPEN=1` enables.

### Restart vs retransmit, decided in-band

The damper's first epoch heuristic gated its reset on liveness, so an ordinary
loss-recovery retransmit could wipe the whole suppression ring mid-storm. The
criterion is now pure in-band: a peer is reset only when a sequence jumps
backward below the ring's oldest retained entry — a retransmit of anything
still in the ring can never be below the oldest retained sequence, while a
restart near zero is. The explicit purge on socket close remains the primary
reset signal, and a multi-sequence storm test now drives concurrently-retried
sequences past each other, the exact interaction the single-sequence tests
missed.

That explicit purge is itself scoped: it fires only when the closing socket is
the tracked P2P socket (`dampen_close_ends_session`, a pure host-tested
predicate mirroring the reorder path's `was_reorder_sock` guard). The first
wiring purged on *every* `closesocket`, so any unrelated close — a
masterserver re-resolve, a stats upload — would have wiped suppression for all
peers mid-match. Caught by an adversarial audit; `contracts/epoch-purge-gating.md`
has the details, and `tests/dampen_gate_test.cpp` drives every branch of the
predicate. Both proxies carry the gate.

### Six external-review defects, each fixed with a reproducing test

The unsigned RTT-EWMA delta (a downward sample pinned the window at the
ceiling), the epoch reset that re-fired on every packet, the `gov_trace`
verdict that promoted a reverted read into the A/B sample set, the analyzer's
first-session proxy pairing (the ordinary one-process-one-match capture
reported "no matching proxy session"), the `DownCount` value and description
reconciled across all three sources, and the declared Python verifies actually
wired into CI. Details in `contracts/netcode-fixes.md`.

### CI, for the first time

Every push now cross-builds both proxies for 32-bit Windows (the only ABI
anyone installs, previously never compiled automatically), runs the host test
suite at both 64-bit and 32-bit, checks the prebuilt pins against the
committed binaries, runs every Python tool against committed fixtures, and
parses every shell and PowerShell script.

## V4.92 (experimental branch)

**V4.91's first night produced the first complete Windows bundle, recovered
the lost host-side evening, and answered questions we'd been circling for
weeks. V4.92 turns those answers into tooling: A/B arms switchable per
session, identities resolved in the output, crash-capture state visible, and
the stock-value record corrected.**

### Per-session `BZ_*` overrides — the governor A/B is now one file edit

Both wrappers read an optional `bznet.env` from the config directory
(`%APPDATA%\bz-netcode\` / `~/.config/bz-netcode/`), one `BZ_KEY=value` per
line, `BZ_` keys only, exported before the game launches. Every override is
logged and recorded in `meta.txt` (`overrides=`), so a bundle states which
arm it was — the host switching `BZ_GOV_START` between 16000/40000/80000 is
the use this exists for, and it no longer involves touching the Steam launch
line. Wrapper generation: `V4.92-arms-20260803`.

### The retransmit table names its peers

The analyzer's retransmit table keyed by raw IP with no name attached —
"who is that address?" took a manual grep even though every log carries the
answer in its `WAN Connect For Client` lines. Those now join IP to Steam ID
to player name: `Name(ip)=2831pkt`. On the recovered host log
this instantly re-attributed the 2026-08-02 retransmit concentration: 68% of
the host's resends were aimed at the Snap tester — the same link his own
receive side flagged. Both ends of one link, one story.

### A crashed process is not an old proxy

A hard crash never runs the teardown path, so its proxy log has no
session-end stats — and the analyzer blamed a "V4.7 proxy" for what was
actually a V4.91 log missing its stats because the process died (the
2026-08-03 crash bundle hit exactly this). A log with a build stamp but no
`session end:` lines now reports the crash explanation instead.

### The bundle states whether a crash could have left a dump

The 2026-08-03 map-load crash produced no dump, and nobody knew none *could*
exist — crash capture lives in `tester_diag`, which casual evenings don't
run. Both wrappers now write `crash_capture=` into `meta.txt` (WER
LocalDumps / procdump-available / systemd-coredump / NONE, report-only), so
the next crash bundle says up front whether a dump exists to go looking for.

### Stale captures stay out of bundles

`bz_buffer_log.*` persists in the game dir after the capture that made it,
so a session with logging off shipped the previous day's ring looking
current. Capture files older than session start are now skipped, with a log
line saying so.

### The stock-value record is corrected

The first values-as-found reads (identical on Windows and Linux, menu phase)
disagree with the reverse-engineered stock for six of ten [Net] keys.
`shared/net_globals.h` now carries the full comparison table and the
phase caveat (session init provably rewrites at least `MaxBandwidth`:
menu 4000, mid-match 16000 read live on 2026-07-26); `net-ini/README.md`
summarizes it. `MaxPingsLost` graduates from "unmapped" to
confirmed-live-address. An unpatched in-match read is the remaining
arbiter.

## V4.91 (experimental branch)

**Everything in this release comes from one evening of real bundles — the
first ever collected by V4.9's own pipeline (2026-08-02: seven players, nine
matches, three platforms). The instrumentation worked; V4.91 fixes what the
instrumentation caught, including two bugs in the instrumentation itself.**

### The Windows wrapper now harvests from the game root

The first real Windows bundle of the new generation arrived holding only
`meta.txt`. Steam's `%command%` names `launcher\BZLauncher.exe`, the wrapper
took that exe's parent as the game directory, and every log was harvested
from the launcher folder — where the game writes nothing. The launch path
already knew how to walk up to the folder holding `battlezone98redux.exe`;
`Get-GameDirFromCommand` now applies the same correction (`Resolve-GameRoot`),
so the bundle and the game agree on where the session lives. The wrapper also
bundles the tail of its own `bz_wrap.log` and `capture_verify.txt` when
present, so the next odd bundle explains itself. Wrapper generation:
`V4.91-harvest-20260803`.

### The governor read-back no longer convicts the governor

Eight of nine matches reported `POKE DID NOT HOLD` — and every failing read
was 39650..39900 against a 40000 target: the game's own governor taking
DownCount=50 steps off the poked baseline, which is the poke *working*. The
old rule ("any read below target") assumed the governor only ramps up;
DownCount exists precisely to walk it down. A clamp now requires the read to
be at/below the stock floor (16000) and more than `down_slack` (500 = 10
steps) under the target — a revert to stock or a 4000 re-init still convicts,
the governor's own adjustments never do. The held line now says which kind of
held it saw. Replayed in `tests/gov_trace_test.cpp` from the field log.

### `analyze_drops.py` scores whole evenings, not last lobbies

- **Per-match table.** A BZLogger holding nine matches got exactly one
  scored (the last). The analyzer now prints one row per match — lobby, map,
  duration, players, discards, retransmits — before the detailed report.
  On the 2026-08-02 log this immediately surfaced a 6-player match with
  18,220 discards in 11.4 minutes, an order of magnitude above every other
  match of the evening, previously invisible.
- **Pre-V4.91 false alarms are rescored, not discarded.** The clamp line's
  read-back value is parsed; a read above the stock floor is reported as
  "the poke worked" instead of invalidating the session — which rescues the
  existing dataset for the `BZ_GOV_START` A/B.
- **"pre-V4.9 proxy" is no longer the default explanation.** A log with the
  read-back banner but no verdict now reports "read-back armed but no
  cold-start poke was ever observed" — the actual state of the 2026-08-02
  Snap client's log, which was mislabeled as an old build.

### The proxies state what they found, not just what they changed

A [Net] global already at its target produced no log line, so a fresh proxy
whose values were pre-set (by the net.ini mod) was indistinguishable from a
proxy that never looked — that ambiguity cost a diagnosis round on the Snap
client. Both proxies now log one unconditional `values as found:` line before
their first write, and the governor logs the send-rate as found at attach.

### One `session end` line per session

Teardown closes several sockets back to back; the reorder stats were guarded
against that, the pace stats were not, so both Linux logs carried the same
`send_stats` line twice, 100 ms apart — doubling counters for anything that
greps naively. A byte-identical repeat is now suppressed.

### The analyzer records the host, match by match

The host role is a controlled variable in every A/B, and until now it lived
in a log-naming convention. The analyzer reads it from the roster (`Adding
Player … (ID 1, …)` — ID 1 is the host), prints it per match and in the
report header, and warns outright when the host changed mid-log, because
those matches are not comparable as one arm. On the 2026-08-02 evening this
confirms one host across all nine matches — the whole evening is a valid
single-arm sample. Map names also lost their trailing comma.

### Installers end with the truth

The 2026-08-02 Bitdefender case proved the failure mode: the uploader's red
failure block scrolled off-screen and the green "Install complete" (which is
about the DLL) read as all-clear. Both installers now end with an explicit
`Patch DLL: OK    Log uploader: <OK / DID NOT INSTALL / not requested>` line
— the outcome of both halves, stated where the eye actually lands.

### The buffer capture states when it wrapped

`bz_buffer_log.meta.txt` now carries `wrapped=1` when the ring discarded the
oldest events (the 2026-08-02 capture kept 65,536 of 126,715 and lost every
match start). One field to check instead of two counters to compare.

### Hardening

`bz_wrap.ps1` uses `-LiteralPath` in game-dir detection: a Steam library path
containing `[` or `]` would silently fail the wildcard-interpreting
`Test-Path` and lose that tester's bundle.

## V4.9 (experimental branch)

**The packet header is now known exactly rather than guessed, and that
withdrew two published conclusions, disproved the reorder buffer's founding
premise, and exposed a bug that would have discarded 98% of inbound traffic.**

### The sequence field is settled — by ground truth, not by scoring

Every previous answer came from scoring payload offsets for monotonicity, and
both were wrong, because that method cannot tell a sequence number from an
acknowledgement number.

BZLogger prints its retransmits as `TRY Sent Packet (i,n) to IP:PORT: <hex>`,
so both header ordinals are known for 65,806 packets in one capture alone.
`tools/seq_crossmatch.py` brute-forces (offset, width, endian) against those
known values and gets an exact match on **100% of 65,860 samples across five
packet classes**, in every log:

| offset | field |
|---|---|
| 0 | flags (`\|0x80` = retransmit) |
| 1 | class |
| 5 | u40 BE millisecond clock — agrees with wall time to 0.3 ms median |
| **10** | **u32 BE SEQUENCE** |
| **14** | **u32 BE ACK** |

V4.8 read offset 16 — the low half of the ACK field. An ack repeats by design,
which is the entire explanation for the "88.5% duplicate rate". Full derivation
in `resources/BZ_P2P_HEADER.md`.

**Withdrawn:** the "0.0–0.2% out-of-order, 56–83% duplication, 10–27% loss"
figures published in V4.8. They were never measurements of the link.

### The reorder buffer stays off, now for a structural reason

The sequence counts **messages, not datagrams** — one message spans several
datagrams carrying an identical header (19,572 distinct headers across 46,935
captured datagrams). There is no per-datagram ordering key, so the premise this
buffer was built on is false. On the corrected field the capture shows 28
first-arrival inversions in 1,021 sequences, needing an **883 ms** hold window
against the 100 ms ceiling it shipped with.

**Defect E, a live bug:** `reorder_insert` rejected already-delivered sequences
as stale and both proxies *dropped* the datagram. With `BZ_REORDER=1` that
discarded ~98% of inbound game traffic. Those datagrams now pass through.
`BZ_REORDER` has defaulted to 0 since V4.8, so no shipped default was affected.

### The retransmit storm is proactive redundancy

The most extreme event in the dataset — 65,806 retransmitted datagrams, 31% of
every byte the host sent — had never been examined. Each message is emitted
**3.57 times inside a median 46 ms window** and then never again. No ack can
arrive inside 50 ms of the first send, and the peer was acking normally
throughout. This is not loss recovery, and no bandwidth knob affects it.

The `syncJoin` A/B proposed for it is **cancelled**: all five sessions log
`Sync: On`. Four better candidates are ranked in `resources/RETRANSMIT_STORM.md`,
player count first.

### Measurement that was silently broken

- **`analyze_drops.py` could not see host logs at all.** Its bandwidth regex
  required a trailing ` ms`, which only client logs have, so every host log
  reported "no governor data".
- **`Actual Used` is the real-bytes denominator** the V4.8 CHANGELOG said did
  not exist. It is in every host log and always was. Reported as utilisation
  against the governor's own budget, which is precisely the question a
  `BZ_GOV_START` A/B asks.
- **Retransmits were counted in the wrong unit.** 11,986 bursts counted where
  there were 65,806 datagrams, with 5.07 MB discarded. All three are reported
  now, headlined by share of outbound bytes with the denominator named.
- New: an ending classifier (clean / abrupt / copied mid-lobby), the
  `observer.mesh` flood metric, torn-line detection, copies-per-message, and a
  cross-check of the proxy's `BZ_GOV_START` against what the game actually did.

### The governor tells you whether its own poke landed

`BZ_GOV_START` did not stick in one of the two V4.8 matches, and finding that
out took hand-correlating two logs from machines whose clocks are an hour
apart. The governor thread now re-reads the global and emits one verdict per
match — `poke held`, or `POKE DID NOT HOLD — wrote 40000, reads 16000 3800 ms
later` — plus a periodic trace carrying the live rate. `tests/gov_trace_test.cpp`
replays both real matches on the host.

### Shipped bugs fixed

- **Torn log lines.** `log_line` had no lock and issued two `WriteFile` calls
  per line while five threads logged concurrently. Two logs in the working set
  are corrupted by this. One formatted line, one write, under a lock.
- **Teardown use-after-free.** Worker threads are signalled and not joined
  (joining under the loader lock deadlocks), yet detach deleted every critical
  section, freed the buffer ring, closed the wake socket and `fclose`d the log.
  None of that happens now; the process is exiting and the OS reclaims it.
- **Silent datagram truncation.** `deliver_to_caller` copied
  `min(len, caller buffer)` and reported success. Now `WSAEMSGSIZE` +
  `MSG_PARTIAL`, as stock winsock does.
- **The IOCP path** still compared `seq == last_seq + 1` in 32-bit space, and
  mutated the reorder peer table holding only `g_iocp_cs` — a live data race
  against the wake thread. Fixed, with a documented lock order and scoped
  acquisition, and its two decisions moved into `reorder_core.h` so host tests
  reach them.
- **`BZ_BUFFER_LOG_PEER` did nothing.** Both logger scripts wrote it into every
  tester's launch options and no proxy read it. Implemented.
- **The ring size was never honoured.** The capture asked for 500,000 records
  and ran with 65,536, losing 48% of its events including the match start —
  because the game was launched before the launch options were pasted. Both
  proxies now record what they were *asked* for, and both logger scripts refuse
  to be quiet about a mismatch.

### Windows catches up

The Windows log was truncated on every launch (so a crash plus the relaunch a
tester always does destroyed the evidence) and had no timestamps. It emitted a
config line `analyze_drops.py` could not parse. It patched the IAT exactly once.
It had no `recvfrom`, `ioctlsocket` or `WSAIoctl` hook, so a Windows capture
could only ever answer "WSARecvFrom". It hooked whatever process loaded
`winmm.dll`. Its buffer-log `sid` meant something different from Linux's. All
six closed.

Both prebuilts are refreshed and both binaries now log
`proxy build: <version> <commit> <date>` at attach.

### Infrastructure

- **CI**, which did not exist: cross-builds both proxies, runs the host suite at
  64-bit *and* 32-bit (the shipped ABI was never exercised), runs the pin check
  that nothing had ever run, and checks shell and PowerShell syntax.
- **`tools/check_record_layout.py`** pins the binary record layout across the
  two C++ sources and the Python decoder — nothing did before.
- **Uninstallers** for both platforms, which did not exist. They keep your logs
  unless you ask otherwise.
- **`install_linux.sh`** verifies the source tarball it builds from, and
  validates `--ref`/`BZNET_REF` instead of interpolating them into a URL.
- **`upload/bz_wrap.sh`** — a launch-option wrapper that bundles and uploads a
  session on every exit, including a crash. Snapshots BZLogger before launch,
  which is the whole reason one 2026-07-26 log was lost.

### Closed by investigation, no code needed

`Source IP doesn't match any known player` appears in every log and had never
been examined. Every occurrence lands in the same millisecond as a connect
handshake, which probes every candidate address for a peer at once — including
loopback and a Docker bridge. Replies from addresses not yet in the player table
are dropped. Benign by construction.

### Snap Steam could not launch with the uploader line

The "one standardized line" was never true for Snap. Snap Steam remaps `HOME`
into `~/snap/steam/common/`, and snapd's home interface excludes the host's
dot-dirs entirely, so `${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode/bz_wrap.sh`
resolved to a path that does not exist inside the sandbox — Steam's exec of
the launch options failed and **the game never started**. (The plain
non-uploader line has no file dependency, which is why only testers hit it.)

Three changes close it:

- **`bz_wrap.sh` is self-locating**: a sibling `upload.conf` pulls the conf,
  outbox and work dirs next to the script, so a copy inside a sandbox is
  self-contained — and running that copy's `--retry` from a host shell drains
  the same outbox with the host's curl.
- **`install_linux.sh` mirrors** the wrapper and conf into the Snap and
  Flatpak sandbox dirs whenever those Steams exist (the Flatpak copy was a
  manual step before), and prints a Snap-specific launch line built on
  `$SNAP_USER_COMMON`, which snapd guarantees inside the sandbox no matter
  how `HOME` and the XDG vars are remapped.
- **`uninstall_linux.sh` removes** all the uploader copies it can find,
  keeping any outbox that still holds unsent bundles.

The first Snap field test then found the second half of the problem: the
game launched, but **the bundle never sent**. The upload path assumed `curl`
*and* `python3` (for JSON escaping) — the Steam snap's core22 runtime ships
neither, and stock Ubuntu desktop doesn't ship curl either, so even the
manual `--retry` escape hatch was broken on the machine most likely to need
it. Three more changes:

- **JSON escaping is pure bash** now; the curl path no longer needs python3.
- **A python3 stdlib uploader** (urllib, multipart built by hand) is the
  fallback when curl is missing — which is exactly a stock Ubuntu host.
- **The installer enables a host-side systemd user timer** on Snap machines
  (`bz-netcode-retry.timer`, every 10 minutes) that drains the outbox with
  host tools, since nothing inside the snap sandbox can ever send. The
  uninstaller disables and removes it.

### Bundles are attributed to the in-game name, not the OS account

The first Snap bundles arrived labeled with the tester's computer username,
which is not who anyone is in the game. Two attempts taught what the right
source is:

- The Steam **login** (`AccountName`) and the Steam **persona**
  (`PersonaName`) both came from `config/loginusers.vdf` — and the persona
  proved to be a login-time snapshot that matched nobody's actual in-game
  name when it hit the field.
- The name every peer's BZLogger agrees on is the one the game authenticates
  with, and the session's own log states it outright:
  `Authenticated to BZRNet As S<steamid>:<name>`. Unlike the `Adding Player`
  lines, that line only ever names the local player.

Both wrappers now read that line (the last one in the session's
`BZLogger.txt`) at upload time, so the bundle filename, `meta.txt` and the
Discord message all carry the same name the logs themselves use — which is
what cross-log analysis matches on. Fallbacks, in order: explicit
`BZ_PLAYER`/`BZNET_PLAYER`, the Steam persona (for a session that died
before authenticating), the OS username last. The installers stopped baking
the OS username into `upload.conf`.

### Windows: Defender blocks are detected and explained, not silently eaten

A tester hit "potentially malicious application blocked": AV heuristics flag
an unsigned MinGW proxy DLL that hooks networking, either by killing the
download mid-install or by quarantining `winmm.dll` moments *after* a
successful copy — leaving "install complete" a lie and the game silently
unpatched. The installer now catches the blocked-download error, re-checks
existence and hash a few seconds after the copy, and on either failure
prints the exact Protection-history / `Add-MpPreference` steps (allow the
one file; never disable AV globally). The README carries the same
walkthrough, including why allowing it is reasonable: the SHA256 sidecar
proves the allowed file is bit-for-bit the build whose source is public.

### Bundle names carry the player and the time — nothing else

Machine hostnames are personal info and were sitting in every bundle
filename and Discord headline (`bz_<player>_<hostname>_<stamp>`). Both are
now `bz_<player>_<stamp>` / `**player** — map …`; the hostname survives only
inside `meta.txt`, where debugging needs it. `wrapper_version` in `meta.txt`
is now a distinct stamp per wrapper generation (`V4.9-ingame-20260729`) —
a mislabeled bundle turned out to be undiagnosable when every wrapper
version called itself plain "V4.9".

### The install command survives being pasted into the wrong shell

A tester pasted the pinned Windows command — built as
`powershell -Command "$env:BZNET_REF='…'; …"` for Command Prompt — into
PowerShell. The outer shell expanded the `$env:` variables to empty before
the inner shell ran, and the installer silently fell back to `master`,
installing the wrong branch with no uploader while still printing "Install
complete". Three changes:

- **The default ref is baked per branch**: this branch's installers install
  this branch when run with no environment at all. `BZNET_REF` still
  overrides.
- **The README commands are now shell-native one-liners** (plain
  `irm … | iex`, plain `curl … | bash`) with no `$env:` prefix to lose.
- **A missing `BZNET_WEBHOOK` is announced**, telling test-crew members to
  re-paste the pinned command into PowerShell instead of leaving them
  without an uploader unknowingly.

### The Windows wrapper could never launch the game — found by its first real user

"Crashing before the launcher opens" traced to a one-word bug:
`Resolve-SplitCommand` and `Get-GameDirFromCommand` declared their parameter
as `$Args`, which PowerShell's automatic `$args` variable silently clobbers
to empty. Every wrapped launch handed `Start-Process` a null path, so the
wrapper (added in `e0e027a`) had never once launched the game on a real
machine. Fixed by renaming the parameter — and the class is fenced off:

- The **pre-launch phase is best-effort** (try/catch around resolve,
  snapshot, outbox flush): the one job the wrapper must never fail at is
  starting the game. A launch failure now logs the exact command instead of
  a bare stack trace.
- **`bz_wrap.bat` launches the game plain** when `bz_wrap.ps1` is missing —
  antivirus quarantine being the usual reason — instead of leaving a Steam
  play button that does nothing.
- **The installer verifies the wrapper files still exist** after install,
  same async-quarantine check as the DLL.
- **`Send-Bundle` was rewritten on `HttpClient`**: the `.bat` runs
  `powershell` (= Windows PowerShell 5.1 on stock machines), and
  `Invoke-RestMethod -Form` does not exist there — the upload would have
  failed on every tester box even after the launch fix. The multipart
  encoding (quoted part names, no `filename*`) is live-verified against
  Discord.
- All of this is exercised for real now: every `.ps1` in the repo is
  parse-checked with an actual PowerShell, and the wrapper ran end-to-end
  (launch, BZLogger snapshot, in-game-name bundle, webhook upload) under
  pwsh before shipping.

### The V4.9 Windows DLL could not load on a real Windows machine

"Runs fine on V4.8, errors on V4.9": the V4.9 threading work made
`winmm.dll` import **`libwinpthread-1.dll`**, a MinGW runtime library that
exists on no normal Windows install (V4.8's import table was clean, which is
why it worked). The loader failed the whole DLL, taking the game down with
it. Reproduced exactly under Proton's own Wine — old prebuilt fails
`LoadLibrary` with error 126, module not found — and fixed by linking the
runtime statically (`-static`; the Linux proxy was never affected). The
prebuilt and its SHA256 sidecar are refreshed; export table verified
identical to the working V4.8 build, and the new DLL load-tested clean.

Note for the antivirus false-positive submission: the refreshed prebuilt is
a **new hash** — submit this one.

### Windows bundles arrived crash-shaped, mid-session, or not at all

Two field reports from the first wave of real Windows testers, two causes:

- **The wrapper waited on the wrong process.** Steam's `%command%` on
  Windows is `Launcher\BZLauncher.exe`, which spawns
  `battlezone98redux.exe` and exits within seconds — so `-Wait` returned
  while the session was only just starting. The wrapper bundled a BZLogger
  with no game in it, found no `Exiting Game With Return Code`, stamped it
  **CRASH**, uploaded that, and was long gone before the real session
  ended. It now polls for the game process itself after the launch command
  returns and bundles when it is gone. Linux never had this: Proton's
  `waitforexitandrun` holds until the whole prefix is empty.
- **Menu-only bundles now upload by default, on every platform.** The old
  skip was principled — a menu-only session teaches nothing about the
  netcode — but skipped-on-purpose and silently-broken look identical from
  the channel, and two testers reported the uploader as broken when it had
  correctly decided their session was uninteresting. A menu-only bundle is
  a few kB and proves the whole pipeline on that machine.
  `BZ_UPLOAD_MENU=0` in `upload.conf` restores the skip.

### The installer allowlists itself where it legitimately can

To cut the manual AV dance for testers, the Windows installer now, before it
downloads anything:

- **Pre-authorizes the game folder and `%LOCALAPPDATA%\bz-netcode` with
  Windows Defender** via `Add-MpPreference` — the supported first-party API.
  On a Defender machine (most testers) the install now goes through with no
  block at all. It only ever *adds* two exclusions; it never disables
  anything, and it silently no-ops when it is not admin or when Defender is
  not the active AV.
- **Detects a third-party antivirus** (via SecurityCenter2) and, if one owns
  real-time protection, prints up front that `Add-MpPreference` will not
  reach it, with the two exact folders to except and — for Bitdefender
  specifically — the precise menu path plus the Advanced Threat Defense
  exception its runtime monitor needs.

What it deliberately does **not** do is reach into a third-party AV to
disable protection or add exceptions without the owner acting — that is the
machine owner's call to make in their AV's own UI, and a patch installer
silently doing it would be indistinguishable from malware.

## V4.8

**Measurement corrected the record: the reorder buffer never ran, and on real
links there was nothing for it to reorder. It now ships off by default.**

### What a wire capture showed

`buffer-logging/decode_buffer_log.py` (specified in the buffer-logging readme
since the logger landed, never written until now) decoded a 65,536-datagram
capture from a live match:

- **The game receives via overlapped I/O.** 100% of receives go through
  `WSARecvFrom`, and 28% return `WSA_IO_PENDING`. The reorder path bypasses
  overlapped calls, so across a 2.5-hour session it buffered zero packets and
  emitted no `reorder_stats` line at all — under Proton, where reordering was
  documented as "fully active". Every improvement measured to date belongs to
  the `[Net]` poke, the socket buffers and DSCP marking.
- ~~**Out-of-order arrivals are 0.0-0.2%.** Measured per packet class, and
  corroborated by the game's own log: 6,998 of 7,012 discards were packets
  already consumed, only 14 arrived early. The real traffic problem is
  duplication (56-83%) and loss (10-27%).~~
  **WITHDRAWN in V4.9** — these numbers were read from the acknowledgement
  field, which repeats by design. See `resources/BZ_P2P_HEADER.md`.

`BZ_REORDER` therefore defaults to **0**. The code is kept, tested and correct
rather than deleted — the finding is about these links, not all links.

### Two long-standing errors corrected

- **Sequence field.** *(Corrected again in V4.9: it is u32 big-endian at offset
  **10**. Offset 16 is the low half of the acknowledgement field. See
  `resources/BZ_P2P_HEADER.md`.)*
  Read as u32 little-endian at payload offset 13; it is
  **u16 big-endian at offset 16**. The old field could not distinguish packets:
  two datagrams with different sequence numbers both read `0x380000c1`, because
  byte 16 is the counter's high byte landing as that u32's most significant
  byte. It changed once per 256 packets. Extraction now lives in one shared
  helper instead of three separate `memcpy`s, comparison wraps in 16-bit space
  (the counter wraps every ~6-11 minutes of play, which 32-bit comparison would
  have read as a 65,535-packet backward jump and stalled the peer on), and two
  regression tests cover the wrap. `resources/valid_capture_reorder_signal_*`
  are marked superseded.
- **`MinBandwidth` does not set the opening send rate.** A live A/B with
  `BZ_NET_MINBANDWIDTH=40000` opened at 16000 anyway, with the live rate still
  4000 at session setup. `BZ_GOV_START` is the real lever. `MinBandwidth` is no
  longer written by default and its address is flagged unconfirmed.

### Governor

`BZ_GOV_START` raised 16000 -> 40000. A measured match sat at the opening rate
for 72 seconds *after* the simulation started, took 4.7 minutes to reach
80 kB/s, and peaked at 64 kB/s actual send against a governor budget that ran to
112,700 — the opening was far below what the link carried. Not yet validated at
40000 across a full match.

### Tooling

`tools/analyze_drops.py` was scoring every A/B on a number that did not mean
what its docstring claimed. Discards are all *already-consumed* packets, not
stale/out-of-order arrivals; `expected #0` (23-46% of lines) was folded into the
headline; a heuristic silently halved the result; and session slicing ran each
match into every later one, reporting a 20.8-minute game as 72.5 minutes. All
fixed, plus retransmits-per-MB added as the log's actual loss signal.

The buffer logger now writes a paste-ready `launch_options.txt` (a header line
above it meant a select-all paste fed Steam the header) and documents the
`%command%` ordering rule. The Linux proxy wrote literal `\r\n` into
`bz_buffer_log.meta.txt` where the Windows one writes real CRLF.

### Session counters survive an exit that skips `closesocket`

The first full V4.8 match produced no `session end: send_stats` line at all.
Both proxies emitted that summary from exactly one place — `hooked_closesocket`
— and the game held its P2P socket open through the post-match lobby, then went
away without unwinding. `DLL_PROCESS_DETACH` signalled the worker threads and
deleted the critical sections without ever formatting the counters, so a
19.7-minute measurement was discarded at the last step.

Both proxies now re-emit from the detach path when `closesocket` did not get
there first, guarded by two flags so a clean shutdown still logs once. The
detach path differs in three ways, all forced by the loader lock: locks are
taken with `TryEnterCriticalSection`, because the worker threads are already
terminated and a section one of them owned would never be released; the pacer
queue is not flushed, since sending is unsafe once ws2_32 may have unwound; and
no state is reset, as nothing will read it. The `session end: ` prefix is
byte-identical to the closesocket path so `analyze_drops.py` keeps matching it,
with provenance on a preceding `process exit without closesocket:` line.

This is a measurement fix, not a netcode change — no packet path is touched.

### Windows installer no longer hardcodes the prebuilt's hash

Refreshing `prebuilt/windows/winmm.dll` broke every Windows install: the hash
was also written as a literal in `install/install_windows.ps1`, and the script
is cached both by raw.githubusercontent and by anyone who saved it, so copies
from before the refresh kept checking the new binary against the old hash.
Updating the literal did not help those copies — they carry their own.

The script now reads `prebuilt/windows/winmm.dll.sha256` at run time, so the
hash always travels with the binary it describes. This is a corruption check
rather than a defence against a compromised repo — the sidecar shares an origin
with the DLL, and `irm | iex` already grants that origin code execution — which
is roughly what the literal was worth too. `BZNET_WINMM_SHA256` still forces a
specific value for anyone who wants strict pinning.

The Windows prebuilt is reverted to the V4.8 build published at `2438ff1` so
already-cached installers keep working. It therefore does not yet carry the
`DLL_PROCESS_DETACH` counter backstop; the Linux prebuilt does. Refresh it in a
deliberate release once the sidecar-reading installer has propagated.

`tools/check_prebuilt_pins.sh` verifies each prebuilt against its sidecar and
fails if an installer reintroduces a hardcoded hash.

### Known limit in the loss metric

`retransmits per MB sent` divides by the **median governor budget**
(`analyze_drops.py:256`), not by bytes actually sent. The budget is known to
overstate real sending — V4.7 measured 64 kB/s actual against a budget running
to 112,700 — and the ratio is not fixed, so the figure is only comparable
between runs whose budgets are in the same range. The 2026-07-26 V4.8 match sat
at a median of 25,700 against 76,600 for the run it was being compared to,
which is too wide a gap to score. Now that `send_stats` survives process exit,
a future revision can use the proxy's own `bytes=` as the denominator.

## V4.7

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
