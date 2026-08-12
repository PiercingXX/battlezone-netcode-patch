# Future plans

The next release is **V4.94** — an increment on V4.9, not V5. The V4.8 review
list (2026-07-26) is built out on `experimental/v4.9`; see `CHANGELOG.md` for
what landed.

## How to use this file

Three layers, in the order you want them:

1. **[Task index](#task-index)** and the task specs under
   [§V4.94 work](#v494-work) — everything that needs doing, written to be picked
   up cold. Each task states its files, its steps, and how you know it is done.
2. **[Field sessions](#field-sessions)** — work that needs people in a lobby.
   Cannot be done at a keyboard alone.
3. **[Evidence archive](#evidence-archive)** — why the tasks say what they say.
   Findings from each session, kept in date order, including the ones that were
   later retracted. Do not delete retracted findings; strike them and say why.

### Conventions

- **Task IDs are stable.** `T1`, `F2` etc. Reference them in commit messages.
  If a task is dropped, mark it dropped rather than renumbering.
- **Every code task ships with a test.** The host test suite is
  `make -C tests run`; it must stay green at both 64-bit and 32-bit (CI runs
  both — the shipped DLLs are 32-bit).
- **Both proxies or neither.** `Linux/proton_dsound_proxy/src/dsound_proxy.cpp`
  and `Microslop/winmm_proxy/src/netcode_hooks.cpp` are separate codebases that
  must behave identically. Shared logic belongs in `shared/*.h`, header-only,
  platform-independent, caller owns the lock and supplies the clock.
- **Nothing writes `.text`.** All memory writes stay data-only and DRM-safe.
- **Defaults are a decision.** Anything switched on by default needs a
  measurement behind it, and an env var to switch it off.
- **Don't quote a number you haven't re-derived.** Four figures in the first
  draft of the V4.94 section were wrong (see [§A4](#a4-corrections-log)); every
  one of them had been copied forward rather than re-measured.

### Status legend

`TODO` not started · `WIP` in progress · `BLOCKED` waiting on something named ·
`DONE` shipped · `DROPPED` deliberately not doing

---

## Task index

| ID | Task | Pri | Status | Blocked by |
|---|---|---|---|---|
| **[T1](#t1-sharedsend_dampenh--the-duplicate-suppressor)** | `shared/send_dampen.h` — per-(peer,seq) duplicate suppressor + tests | **P0** | **DONE** | — |
| **[T2](#t2-wire-the-damper-into-both-proxies)** | Wire the damper into both proxies, 4 hook sites | **P0** | **DONE** — live validation deferred | — |
| **[T3](#t3-ship-the-ramp-knobs-as-defaults)** | `DownCount` 50→200, `UpCount` 100→50 as defaults | **P1** | TODO | — |
| **[T4](#t4-the-wrapper-must-detect-a-real-crash)** | Wrapper captures the *game's* exit code, classifies CTD | **P0** | TODO | — |
| **[T5](#t5-crash-capture-configured-by-the-installer)** | Installer configures WER LocalDumps / verifies coredump | **P1** | TODO | — |
| **[T6](#t6-build-skew-guard)** | Refuse to bundle silently on proxy/wrapper version skew | **P1** | TODO | — |
| **[T7](#t7-analyzer-per-object-emission-rate)** | Analyzer: per-object emission rate | P3 | **MOSTLY DONE** | — |
| **[T8](#t8-analyzer-fix-the-observermesh-attribution)** | Analyzer: `observer.mesh` counted in-window, reword claim | **P2** | TODO | — |
| **[T9](#t9-analyzer-flag-a-client-out-sending-the-host)** | Analyzer: flag a client out-sending the host | **P2** | TODO | — |
| **[T10](#t10-analyzer-close-the-poke-did-not-hold-false-positive)** | Analyzer: close the `POKE DID NOT HOLD` rejoin false positive | **P2** | TODO | — |
| **[T11](#t11-analyzer-stop-guessing-which-proxy-session)** | Analyzer: stop guessing which proxy session to score | **P2** | TODO | — |
| **[T12](#t12-file-the-mod-bug-upstream)** | File the Nav beacon bug with the mod author | P1 | **DONE** | — |
| **[T13](#t13-merge-the-two-storm-documents)** | Merge `RETRANSMIT_STORM.md` into `CAMERAPOD_STORM.md` | **P3** | TODO | — |
| **[T14](#t14-delete-or-regenerate-the-bogus-reorder-resources)** | Delete/regenerate the two wrong-offset reorder resources | **P3** | TODO | — |
| **[T15](#t15-map-the-remaining-log-switches)** | Map `bzrnetlog=` / `netlog=`, the two unexplored log switches | **P3** | TODO | — |
| **[F1](#f1-nav-beacon-storm-test)** | Nav beacon storm test evening | P0 | **DONE** — cause found and fixed | — |
| **[F6](#f6-re-measure-once-the-mod-fix-ships)** | Re-measure a crew evening once the fix ships | **P1** | **BLOCKED** | T12 upstream release |
| **[F2](#f2-ramp-knob-ab)** | `DownCount`/`UpCount` A/B | **P1** | TODO | T3, T6, F6 |
| **[F3](#f3-bz_gov_start-ab--16000--40000--80000)** | `BZ_GOV_START` 16000/40000/80000 A/B | **P2** | TODO | T6, F6 |
| **[F4](#f4-buffer-capture-re-run)** | Buffer capture re-run with the ring honoured | **P3** | TODO | — |
| **[F5](#f5-map-the-nppi-keys)** | Map the five unmapped NPPI keys | **P3** | TODO | — |

**If you only do one thing:** T1+T2 — **DONE as of V4.93** (off by default;
the in-game validation pass is the deferred step, see T2). **The storms are
still live.** The cause
is found and a fix is written, tested and handed upstream (§A5) — but the mod
author has not published it, so nothing has changed for anyone actually
playing. The damper is the only mitigation this project controls, and it works
without anyone else shipping anything.

Second priority is [T4](#t4-the-wrapper-must-detect-a-real-crash): unaddressed,
with a crash in the record, no dump, and a wrapper that cannot tell a crash
from a clean quit.

**Re-price T1 only when the fix is actually deployed**, and not before —
"deployed" meaning published upstream *and* the crew confirmed on it. Even
then it keeps value: the crew runs mixed versions for weeks at a time, and the
6–9× amplifier is an engine defect that no mod fix touches.

**Related work:** another project is building the same kind of proxy and has
independently confirmed our packet header —
[`resources/RELATED_PROJECTS.md`](resources/RELATED_PROJECTS.md). Worth reading
before starting T1, T2 or T15.

**Sequencing note:** T6 before any A/B — three consecutive evenings were
muddied by build skew, and an A/B across mixed builds measures nothing.

**And [F6](#f6-re-measure-once-the-mod-fix-ships) before any *governor* A/B.**
Every bandwidth figure in this file was measured on a mod that was flooding the
reliable channel. A fix exists but is **not deployed** (§A5,
[T12](#t12-file-the-mod-bug-upstream)), so today's baseline is still the
flooded one. Once it ships, F2 and F3 both need re-scoring — their controls
were measured under storm conditions.

---
## V4.94 work

Everything here can be done at a keyboard. Field work is in
[§Field sessions](#field-sessions).

Each task is written to be picked up cold. If you find a step is wrong, fix the
task text in the same commit as the code — a task spec that lies is worse than
no task spec.

---

### T1. `shared/send_dampen.h` — the duplicate suppressor

**Priority P0 · Status DONE 2026-08-11 — header + tests shipped; epoch
criterion re-derived in-band after the liveness-gated version proved wrong
(see `contracts/epoch-refix.md`)**

**Goal.** A header-only, platform-independent table that answers one question
per outbound datagram: *have I already sent this exact `(peer, sequence)`
recently enough that this copy is redundant?* If yes, the caller drops it.

**Why.** BZRNet's reliable retry timer is fixed at **~10 ms with no backoff**,
measured directly from the per-copy send clock at header offset 2, against an
RTT the game itself reports as **56–91 ms**. Every reliable message therefore
goes out **6–9 times** before an acknowledgement can physically return. That is
an engine defect nobody outside Rebellion can fix, and it applies to all
reliable traffic, always. Evidence:
[`resources/CAMERAPOD_STORM.md`](resources/CAMERAPOD_STORM.md), §A5 in the
[Evidence archive](#evidence-archive).

**Status of the root cause, and why this is still P0.** As of 2026-08-10 the
storms are **still live in the field**. The cause is found and a fix is
written, locally verified and handed to the mod author (§A5,
[`resources/navfix/`](resources/navfix/)) — but it has not been published, so
no player is running it. Until it ships *and* the crew is confirmed on it, this
damper is the only mitigation under this project's control.

It keeps a smaller but real value after that too:

- the **flat 6–9× multiplier on all reliable traffic** is an engine defect no
  mod fix touches;
- the crew runs **mixed versions for weeks** — three consecutive evenings in
  this dataset were muddied by build skew, so "the mod is fixed" will not mean
  "everyone is fixed" for a long while;
- **insurance against the next misbehaving object.** Two independent defects in
  one mod produced this; the amplifier is what turns a script bug into an
  unplayable match.

**Expected saving.** Bounded by copies-per-message, and the storm-era figures
are the ones that matter while the storms are live:

| match | copies/msg | ceiling on retransmit bytes saved |
|---|---:|---:|
| 08-03 control | 3.83 | ~74% |
| 08-08 host | 4.06 | ~75% |
| 08-09 vbgthykuj | 2.47 | ~59% |
| 08-09 Peppy | 1.87 | ~46% |

Quote the range, never the best case. Once the mod fix is deployed, re-measure
against [F6](#f6-re-measure-once-the-mod-fix-ships) — in a healthy match reliable
traffic is only ~0.7–1.3% of bytes, so the damper's share of total uplink drops
sharply even though the multiplier is unchanged.

**Files.**

- new: `shared/send_dampen.h`
- new: `tests/send_dampen_test.cpp`
- edit: `tests/Makefile` (add to `BINS` and add the build rule)

**Design constraints — these are not negotiable.**

- **Never drop a distinct `(peer, seq)`.** Only 2nd-and-later copies of a
  sequence already sent. If in doubt, send.
- **Never drop a datagram too short to carry a sequence.** `kReorderSeqMinPay`
  (18 bytes) is the floor; control and ping traffic sits below it and is
  exactly the traffic the host's auto-kick measures latency on. Same rule
  `send_pace.h` already applies.
- **Never drop a datagram without the retransmit bit set.** First transmissions
  always go.
- **Caller owns the lock and supplies the clock.** Mirror `send_pace.h`: no
  threads, no syscalls, no globals, no allocation inside the header.
- Fixed-size storage. No `new`, no `std::map`. The proxies run inside a game
  process; an allocation on the send path is not acceptable.

**Reuse, do not re-derive.** All verified against live field data:

- `bznet::reorder_is_reliable(p)` — bit 7 of byte 0. **Note the rename:** this
  was `reorder_is_retransmit()` and the old name was a misreading — `0x80`
  means *reliable*, not *resent*. Nothing in the header marks a retransmit; the
  only way to know is to have seen that `(peer, seq)` before, which is exactly
  what this task builds. The old name survives as a deprecated alias.
- `bznet::reorder_seq_from_payload(p)` — u32 big-endian at offset 10
- `bznet::reorder_send_time_ms(p)` — u64 big-endian epoch ms at offset 2,
  stamped fresh per copy. **New**, and it is what made the retry cadence
  measurable. Use it for the RTT estimator below.
- `bznet::kReorderSeqMinPay` — minimum payload length carrying a sequence
- `bznet::kBzHdrFlagReliable` / `kBzHdrFlagFinal` — the byte-0 flags

**Shape to follow** (`shared/send_pace.h` is the reference implementation of
this pattern):

```
constexpr uint32_t kDampenPeers      = 16;   // same order as reorder's peer table
constexpr uint32_t kDampenSlots      = 64;   // per-peer ring of recent sequences
constexpr uint32_t kDampenFloorMs    = 60;   // never suppress on a shorter window
constexpr uint32_t kDampenMaxMs      = 400;  // backoff ceiling
constexpr uint32_t kDampenRttShift   = 3;    // ewma: rtt += (sample - rtt) >> 3

enum DampenDecision { kDampenSend, kDampenSuppress };

struct DampenEntry { uint32_t seq; uint64_t last_sent_ms; uint32_t window_ms; };
struct DampenPeer  { uint32_t addr; uint32_t rtt_ewma_ms; DampenEntry slots[kDampenSlots]; ... };
struct DampenStats { uint64_t seen, suppressed, bytes_saved, peers_evicted,
                     bypass_short, bypass_notretx, tbl_full; };
struct DampenCtx   { bool enabled; DampenPeer peers[kDampenPeers]; DampenStats st; };

inline void            dampen_init(DampenCtx *c, bool enabled, uint64_t now);
inline DampenDecision  dampen_admit(DampenCtx *c, uint32_t peer_addr,
                                    const uint8_t *pay, uint32_t len, uint64_t now);
inline void            dampen_observe_ack(DampenCtx *c, uint32_t peer_addr,
                                          uint32_t acked_seq, uint64_t now);
inline void            dampen_purge_peer(DampenCtx *c, uint32_t peer_addr);
inline int             dampen_format_stats(const DampenCtx *c, char *buf, size_t n);
```

**Backoff rule.** First send of a sequence: record it, return `kDampenSend`,
set `window_ms` to the current per-peer estimate. A repeat inside `window_ms`:
return `kDampenSuppress`, leave `last_sent_ms` alone (so the window is measured
from the last *actual* send, not the last attempt). A repeat after `window_ms`
elapses: return `kDampenSend`, update `last_sent_ms`, and double `window_ms`
capped at `kDampenMaxMs`. A genuine loss-recovery retransmit still gets through.

**The window must come from measured RTT, not a constant.** This is the
correction that follows from knowing the engine retries at ~10 ms flat: the
right window is "long enough that an ack had a fair chance", which is a
property of the link, not a magic number. Per peer:

```
window0 = clamp(1.2 * rtt_ewma, kDampenFloorMs /*60*/, kDampenMaxMs /*400*/)
```

Two ways to estimate `rtt_ewma`, both available in the hook:

1. Time from the first send of sequence *N* to the first **inbound** datagram
   whose ack field ≥ *N*. This is a true RTT and needs only what the proxy
   already sees.
2. The game's own `Received Packet … sent at %llu, received at %llu, diff %lld`
   line, when packet logging is enabled — a cross-check, not a dependency.

Until an estimate exists for a peer (first few seconds), use `kDampenFloorMs`
and suppress nothing below it. **Never let the window go below the floor** — a
window shorter than RTT reproduces the bug we are correcting.

**Eviction.** Per-peer ring, oldest slot wins. On overflow, count `tbl_full`
and **send** — never suppress because the table is full. Peers keyed by IPv4
address in network order; unknown peer takes a free slot, and if there are
none, evict the least-recently-used and count `peers_evicted`.

**Sequence wrap.** The field is u32 and a session will not exhaust it, but a
peer reconnecting restarts near zero. Treat a sequence more than
`0x40000000` *behind* the peer's high-water mark as a new epoch and clear that
peer's ring, rather than suppressing a fresh low sequence against a stale
high one.

**Env.** `BZ_SEND_DAMPEN` — default **1** (on) once tests pass; `0` disables and
must produce byte-identical behaviour to today.

**Review the default after field testing.** A sibling project working the same
surface holds the rule *"no packet may be silently dropped, truncated,
duplicated, delayed, or reordered by a default configuration"*
([`resources/RELATED_PROJECTS.md`](resources/RELATED_PROJECTS.md)). This damper
only ever drops a redundant copy of a message already sent, never a distinct
one, so the case for default-on is good — but it is a case worth making
explicitly against measured data rather than assumed. Once
[F6](#f6-re-measure-once-the-mod-fix-ships) has run and the damper's real
saving in a healthy match is known, revisit whether on-by-default is still the
right call. Parse it with
`bznet::ng_default_on()` / `ng_parse_env_u32()` from `shared/net_globals.h` so
it logs the same way every other knob does.

**Tests — `tests/send_dampen_test.cpp` must cover:**

1. First copy of a sequence always sends.
2. Second copy inside the window suppresses; the byte counter rises by the
   payload length.
3. A copy just past the window sends, and the window doubles.
3b. RTT estimator: an ack observed 80 ms after the send yields a window of
    ~96 ms, and a 10 ms ack still floors at `kDampenFloorMs`, never below.
3c. Replaying the measured engine cadence (copies at +12, +11, +10, +10 ms)
    against a 60 ms window suppresses copies 2-5 and sends only the first.
4. Backoff saturates at `kDampenMaxMs`, does not overflow.
5. A payload shorter than `kReorderSeqMinPay` always sends and counts
   `bypass_short`.
6. A payload without the retransmit bit always sends and counts
   `bypass_notretx`.
7. Two peers with the same sequence number do not interfere.
8. Ring overflow sends and counts `tbl_full`; nothing is suppressed on a full
   table.
9. Peer-table overflow evicts LRU, counts `peers_evicted`, still sends.
10. Sequence-epoch reset: a peer restarting at seq 3 after reaching seq 90,000
    is not suppressed.
11. `dampen_purge_peer` clears only that peer.
12. `enabled = false` returns `kDampenSend` for everything and touches no
    counters except `seen`.
13. **Replay test.** Feed the 08-08 storm's `(peer, seq, len, timestamp)`
    sequence — extract it from the log with the decoder in
    `resources/CAMERAPOD_STORM.md` — and assert suppression lands between 60%
    and 80%. Commit the extracted trace as a small fixture under
    `tests/fixtures/`, not the whole 75 MB log.

**Acceptance.**

- `make -C tests run` green at 64-bit **and** 32-bit (`CXXFLAGS=-m32`).
- CI green.
- Header compiles clean under `-Wall -Wextra` with no new warnings.
- No dynamic allocation, no platform headers, no `static` mutable state.

**Verify.**

```bash
make -C tests run
make -C tests clean && make -C tests run CXXFLAGS="-std=c++14 -O1 -g -Wall -Wextra -m32"
```

**Gotchas.**

- `pace_admit()` in `send_pace.h` is a *rate* limiter with a queue. This is
  not that. Do not queue anything, do not delay anything — suppression is
  immediate and final for that copy.
- Do not suppress based on the payload body or object name. That is T-future
  work (see "targeted mode" below) and is off by default because it drops
  distinct messages.

**Explicitly out of scope for T1** — a second, *object-targeted* mode that
matches the payload tail `_camerapod\0` and caps distinct state messages per
`(peer, object)` to N Hz. That one does drop distinct messages, so it is a knob
for a controlled experiment and must never be a default. Do not build it until
[F1](#f1-nav-beacon-storm-test) has run.

---

### T2. Wire the damper into both proxies

**Priority P0 · Status DONE 2026-08-11 — all four hook sites wired, V4.93.
The first pass wired one proxy's `WSASendTo` only and let a "suppressed" copy
fall through to the real send; both send paths in both proxies now suppress
before the pacer, keep the burst measurement seeing every datagram, skip
`BZ_SEND_DUP` on a suppressed copy, and emit `session end: dampen:` counters.
Still open (deferred, operator): the live-session acceptance — config line at
startup and stats line at teardown observed on both platforms, and
`BZ_SEND_DAMPEN=0` byte-parity against a pre-T2 build.**

**Goal.** Call `dampen_admit()` on every outbound datagram in both proxies,
before the pacer, and report its counters at session end.

**Files.**

- `Microslop/winmm_proxy/src/netcode_hooks.cpp`
- `Linux/proton_dsound_proxy/src/dsound_proxy.cpp`

**The four hook sites.** Each proxy has two send paths and both must be
covered — the game uses `WSASendTo` in normal play, but `sendto` is reachable
and was the path the original measurements came through:

| file | function | current `pace_take` call |
|---|---|---|
| `netcode_hooks.cpp` | `Hooked_sendto` (~line 1725) | ~line 1738 |
| `netcode_hooks.cpp` | `Hooked_WSASendTo` (~line 1770) | ~line 1803 |
| `dsound_proxy.cpp` | `hooked_sendto` (~line 1639) | ~line 1650 |
| `dsound_proxy.cpp` | `hooked_WSASendTo` (~line 1679) | ~line 1716 |

(Line numbers drift — find them with
`grep -n 'pace_take(' <file>`.)

**Order of operations, at each site.** The damper runs **before** the pacer and
before the real send. Suppression must look to the game exactly like a
successful send, the same contract `pace_take()` already relies on: a UDP
`sendto` promises handoff, not delivery.

```
1. measure       (existing pace_observe path — must still see every datagram,
                  including suppressed ones, or the burst stats lie)
2. dampen_admit  (new)  -> kDampenSuppress: return len, WSASetLastError(0), done
3. pace_take     (existing)
4. real send     (existing)
```

**Do not** let a suppressed datagram fall through to the `send_dup` block below
each hook — duplicating a copy you just decided was redundant is absurd.

**Wiring details.**

- One `DampenCtx` per process, guarded by the existing `g_pace_cs` critical
  section (Windows) / equivalent mutex (Linux). Do **not** add a second lock:
  the ordering between the pacer and the damper matters and one lock keeps it
  obvious. If profiling later shows contention, split it then.
- Extract the peer IPv4 from the `sockaddr` the same way the `send_dup` block
  already does; skip non-IPv4 and loopback (`127.0.0.0/8`) — loopback is the
  game talking to itself and is already excluded from duplication.
- Call `dampen_purge_peer()` from the `closesocket` hook alongside
  `pace_purge_socket()`.
- Config line at startup, next to the existing `send_pace:` / `send_dup:` /
  `reorder:` lines, in the same style:
  `send_dampen: enabled first_ms=50 max_ms=400 peers=16 slots=64 (BZ_SEND_DAMPEN=0 to disable)`
- Counters in the teardown line:
  `session end: dampen_stats: seen=N suppressed=N (P%) bytes_saved=N bypass(short=N notretx=N tblfull=N) peers_evicted=N`

**Acceptance.**

- Both proxies cross-build 32-bit (`make -C Linux/proton_dsound_proxy`,
  `make -C Microslop/winmm_proxy`) and CI's build-proxies job is green.
- With `BZ_SEND_DAMPEN=0`, a session's `send_stats` packet and byte counts are
  unchanged from a pre-T2 build on the same input.
- A live session logs the config line at startup and the stats line at
  teardown, on both platforms.
- `tools/analyze_drops.py` does not crash on a log containing the new lines
  (it will ignore them until T7).

**Gotchas.**

- The `session end:` stats currently **double-fire** on both Linux boxes
  (V4.91 §8, still open). Do not copy that bug into the new line; if you fix
  the double-fire while you are in there, say so in the commit.
- The Windows proxy's log has torn lines from concurrent writers with no shared
  lock (the analyzer reports 24–43 per session). Emit the new lines through the
  same one-write-per-line-under-lock path the V4.9 work established, not with a
  fresh `fprintf`.

---

### T3. Ship the ramp knobs as defaults

**Priority P1 · Status TODO · Blocked by nothing**

**Goal.** Change two defaults so the game's own bandwidth governor backs off
faster than it climbs, instead of the reverse.

**Why.** As shipped, `UpCount` is patched 10→100 and `DownCount` 5→50: the
governor climbs twice as fast as it retreats, and 10× faster than stock in both
directions. That is inverted for a congestion controller. Measured down-walk on
08-09 vbgthykuj is −400 every 2 s (= −3,000 per 15 s).

**Read §A3.3 in the [Evidence archive](#evidence-archive)
before quoting a prediction.** vbgthykuj is *not* a recovery — the match was
aborted while the budget was still descending. Recovery-by-starvation is
supported only by Peppy and the 08-03 session.

**Files.** `shared/net_globals.h` — the `kNetTunePreset` table (~line 154) and
the per-key defaults in `net_globals_defaults()` (~line 88).

**Change.**

- `BZ_NET_DOWNCOUNT` 50 → **200**
- `BZ_NET_UPCOUNT` 100 → **50**

**Acceptance.**

- `make -C tests run` green (`net_globals_test` covers the preset table).
- A session's `net_patch:` lines report `DownCount 5 -> 200` and
  `UpCount 10 -> 50`.
- Both env vars still override the new defaults.
- `README.md` and both proxy READMEs updated where they quote the old values.

**Do not** also cap `BZ_NET_MAXBANDWIDTH` to ~85000. That was proposed in
V4.93 §4 and the 08-09 data kills it: KFK's host reached **145,900 B/s without
storming** in session A of the same evening, so an 85k cap would cost real
headroom to prevent something it does not prevent.

**Then run [F2](#f2-ramp-knob-ab).** Ship the default, but the arm that scores
it is a field session.

---

### T4. The wrapper must detect a real crash

**Priority P0 · Status TODO · Blocked by nothing**

**Goal.** `meta.txt` states truthfully whether the game crashed.

**Why.** On 2026-08-09 KingFurykiller's game died as host, mid-match, taking the
lobby with it — and the bundle says `game_exit_code=0`. That zero is
**BZLauncher.exe's** exit code, not the game's. `bz_wrap.log` recorded
`game exited with 0` for a process that never ran `DLL_PROCESS_DETACH`. As it
stands the wrapper cannot tell a crash from a clean quit on either platform,
which means nobody can triage a crash report without hand-reading the logs.
Evidence: §A3.5 in the [Evidence archive](#evidence-archive).

**Files.**

- `upload/bz_wrap.ps1` — the wait loop at ~line 592 and
  `New-BundleAndUpload` (~line 345)
- `upload/bz_wrap.sh` — `collect_and_upload()` (~line 449)

**Windows.** The wrapper already polls
`Get-Process -Name battlezone98redux` until it disappears (~line 592) but
throws the process object away. Capture it on the first poll, then
`$gameProc.WaitForExit()` and read `$gameProc.ExitCode`. Note
`Get-Process` returns an object whose `ExitCode` is readable after exit only if
you held the handle — so keep the reference, do not re-query by name.

**Linux.** `%command%` runs under Proton; the game's own exit code is not
directly available the same way. Fall back to the log-evidence classifier below
and record `game_exit_code=unknown` rather than a misleading `0`.

**Log-evidence classifier (both platforms).** The wrapper has both files in
hand at bundle time. `tools/analyze_drops.py:classify_ending()` (~line 416)
already implements exactly this logic — mirror it, do not invent a second one,
and if the two ever disagree that is a bug in one of them:

| BZLogger tail | proxy log | verdict |
|---|---|---|
| has `Exiting Game With Return Code` | has `session end:` for this pid | `clean` |
| no `Exiting Game …`, stops mid-match | no `session end:` for this pid | `crash` |
| no `Exiting Game …`, stops in lobby | either | `incomplete` |

**Write into `meta.txt`:**

```
game_exit_code=<the game's, or "unknown">
launcher_exit_code=<what is recorded today>
game_ending=clean|crash|incomplete
game_ending_evidence=<one line saying which rule fired>
```

Keep `game_exit_code` as a key so old tooling does not break, but it must stop
carrying the launcher's value.

**Also.** The Discord message built at `bz_wrap.ps1:426` already has a
`$crashFlag`; drive it from `game_ending` instead of from the exit code.

**Acceptance.**

- A deliberately killed game (`taskkill /F` / `kill -9`) produces
  `game_ending=crash` on both platforms.
- A normal quit produces `game_ending=clean`.
- Closing the wrapper console mid-session does not produce a false `crash`.
- `shellcheck upload/bz_wrap.sh` and PowerShell syntax check both clean (CI
  runs these).
- Bump `wrapper_version=` in both files; the current generation is
  `V4.92-arms-20260803`.

**Gotcha.** There is a comment at `bz_wrap.ps1:583` explaining why the wrapper
waits on `battlezone98redux` rather than `%command%` — testers previously got
crash-shaped bundles because `-Wait` returned when the *launcher* exited. Do
not undo that; build on it.

---

### T5. Crash capture configured by the installer

**Priority P1 · Status TODO · Blocked by nothing**

**Goal.** When a crash happens, a dump exists.

**Why.** The one crash in the dataset produced no dump and no way to know one
was impossible. KingFurykiller runs the V4.91 wrapper, which does not even
write the `crash_capture=` field. V4.92 added the field but it is *report-only*
— it tells you afterwards that you had no capture configured, which is the
wrong time to find out.

**Files.** `install/install_windows.ps1`, `install/install_linux.sh`, and the
`crash_capture_status()` / `Get-CrashCaptureStatus` helpers in the wrappers.

**Windows.** Configure WER LocalDumps for `battlezone98redux.exe`:
`HKCU\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\battlezone98redux.exe`
with `DumpFolder`, `DumpCount=5`, `DumpType=2` (full). HKCU needs no admin.
Say plainly in the install output that it was configured and where dumps land.

**Linux.** Verify `systemd-coredump` is active and that
`/proc/sys/kernel/core_pattern` routes to it; if not, print the one command to
fix it rather than silently recording `NONE`.

**Both.** If a dump exists and is under the size cap, include it in the bundle;
if it is over, name it and its path in `meta.txt` so it can be fetched
manually.

**Acceptance.** A killed game on a freshly installed machine leaves a dump, and
the bundle either carries it or says exactly where it is.

**Gotcha.** Do not enable anything that uploads a dump automatically. Dumps can
contain memory contents; the privacy rule in `README.md` applies.

---

### T6. Build-skew guard

**Priority P1 · Status TODO · Blocked by nothing**

**Goal.** Make it impossible to run an A/B across mixed builds without noticing.

**Why.** Three consecutive evenings have been muddied. On 2026-08-09 King and
Awildbison were on the V4.91 proxy and wrapper while Monkey and PiercingXX were
on V4.92. Separately, **`overrides=` is empty in every bundle ever collected** —
V4.92's `bznet.env` per-session A/B mechanism, the entire reason that release
exists, has never once been used in the field.

**Files.** `upload/bz_wrap.ps1`, `upload/bz_wrap.sh`, `tools/analyze_drops.py`.

**Wrapper side.** At bundle time, read the proxy build id out of the proxy log
(`proxy build: V4.9x-experimental <sha> <date>`) and compare it to the
wrapper's own generation constant. On mismatch, write
`build_skew=proxy=<x> wrapper=<y>` into `meta.txt` and put a visible warning in
the Discord message. Do not refuse to upload — a skewed bundle is still data.

**Analyzer side.** When handed several bundles, print a build table first and
refuse to print a combined A/B verdict if the builds differ, saying which
machine is the odd one out.

**Also.** Make the installer print the resulting build id at the end of a
successful install, so a tester can paste one line into the channel and settle
"are we all on the same thing?" in seconds.

**Acceptance.** Two bundles with different proxy build ids produce a visible
warning in both the bundle and the analyzer output.

---

### T7. Analyzer: per-object emission rate

**Priority P3 · Status MOSTLY DONE · Blocked by nothing**

**Done 2026-08-10.** `tools/analyze_netpktlog.py` exists and does all of this
against `-netpktlog` captures: per-object emission rate with a `RUNAWAY` flag,
channel split, send decisions, per-peer, RTT, and automatic per-segment scoring
from the tester's own chat callouts. It is what found and then verified the fix.
See [`docs/PACKET_LOG_TEST.md`](docs/PACKET_LOG_TEST.md).

**What is left**, and it is small: `analyze_drops.py` still cannot do this for
an *ordinary* bundle — one captured without `-netpktlog`, which is every bundle
the crew has ever uploaded. Porting the identity decode and the rate flag into
`analyze_drops.py` would make historical bundles scoreable without a re-run.
Worth doing only if someone wants to re-score the archive.

**Files.** `tools/analyze_drops.py` — new helper near `parse_session_events()`
(~line 344), output in `print_session_events()` (~line 439).

**Implement.**

1. For each `TRY Sent Packet` payload, unhexlify and extract the trailing
   printable-ASCII identity string (the decoder in
   `resources/CAMERAPOD_STORM.md` is the reference; keep them identical).
2. Group by identity. For each object: distinct `(peer, seq)` count,
   first/last timestamp, datagram count, copies per message.
3. Emission rate = distinct ÷ span-in-seconds.
4. Print the top few objects by datagram count, plus a headline:
   `object mix: apcamr346_camerapod 92,090 dg (33.2 Hz, 12.0 min) …`
5. **Warn when any object exceeds ~5 Hz** — that threshold is the whole bug.
6. Report the camerapod share of all retransmitted datagrams, and **state the
   denominator explicitly**: share of *all* retransmitted datagrams, noting that
   ~3–8% are short control messages carrying no identity field. The first draft
   of the finding quoted 93/99/99/91% by mixing denominators; the correct
   figures are 94.8 / 94.8 / 96.8 / 87.1.

**Acceptance.** Running the analyzer over the 08-08 bundle reports
`apcamr346` at 33.2 Hz and `apcamr349` at 1.0 Hz, and warns about the former.

**Gotchas.**

- BZLogger has torn/concatenated lines with no newline between records. **Count
  regex matches, not lines** — `grep -c` undercounts. The analyzer already
  knows this in places; make sure the new code does too.
- Timestamps are each machine's local time. KFK is UTC−6, Monkey UTC−7,
  PiercingXX and Awildbison UTC−4. Do not cross-correlate machines without
  converting.
- Do **not** score "the transform keeps changing" as a signal. It is true of
  quiet beacons too (all 739 of `apcamr349`'s messages are distinct) and
  discriminates nothing.

---

### T8. Analyzer: fix the `observer.mesh` attribution

**Priority P2 · Status TODO · Blocked by nothing**

**Goal.** Stop the analyzer making a false causal claim, and stop it dividing a
whole-log count by one match's duration.

**Why.** The current line reads
`! N 'could not load observer.mesh' errors (R/min) — this flood precedes both
committed hard stops`. Both halves are wrong now:

- **The count is whole-log, the rate divides by the selected match.** It
  reported 116,556/min for a 6-minute match whose true peak was 58,398/min.
- **The causal claim does not hold.** The flood is absent from vbgthykuj
  entirely, absent from the crash log (1 error total), and on 08-08 it runs at
  17:21 against a 16:43–16:54 storm. It *does* overlap the Peppy storm by
  ~3 minutes and then continues for 19 minutes after that storm dies.

**Files.** `tools/analyze_drops.py` ~line 623–626, and the module docstring
~line 69 which repeats the claim.

**Implement.** Count only inside the selected match window. Replace the
causal sentence with something descriptive:
`! N 'could not load observer.mesh' errors in this match (R/min) — log-volume
problem; not correlated with the retransmit storms (see resources/CAMERAPOD_STORM.md)`.

**Acceptance.** vbgthykuj reports 0 in-window mesh errors instead of 705,161.

---

### T9. Analyzer: flag a client out-sending the host

**Priority P2 · Status TODO · Blocked by nothing**

**Goal.** One line that would have surfaced the Peppy storm immediately.

**Why.** In `Peppy` the host sent 1,647 retransmits and one client sent 50,170.
Nothing in the output drew attention to that inversion; it was found by reading
a table by eye.

**Files.** `tools/analyze_drops.py`, in the multi-log path of `main()`
(~line 949).

**Implement.** When several logs from one match are supplied, compare
retransmit rates and warn when any client exceeds the host by more than ~5×:
`! Monkey (client) sent 30x the host's retransmits — the storm source is a
client, not the host`.

**Acceptance.** Feeding the four 08-09 session-B bundles produces that warning
naming KingFurykiller.

---

### T10. Analyzer: close the `POKE DID NOT HOLD` false positive

**Priority P2 · Status TODO · Blocked by nothing**

**Goal.** Stop the loudest false alarm in the output.

**Why.** 13 matches on PiercingXX's Linux log and 6 on Awildbison's reported
`POKE DID NOT HOLD` while the trace shows the governor healthy at the very next
sample. V4.91 §2 narrowed the rule to fail only on a revert toward stock; the
case still uncovered is a **client re-syncing to the host's value at join** —
the client briefly reads 4000 because the host has not pushed its value yet.

**Files.** `shared/gov_trace.h` (`gov_trace_step()`, ~line 122) and the
analyzer's verdict counting.

**Implement.** Suppress the verdict entirely for the first N seconds after a
match start on a machine that is not the host, or require two consecutive
low reads before declaring a revert. Prefer the second — it needs no host/client
knowledge, which the proxy does not reliably have.

**Acceptance.** Re-running the analyzer over the 08-09 PiercingXX bundle
reports 0 spurious `POKE DID NOT HOLD`, and `gov_trace_test` gains a case for
the rejoin pattern.

---

### T11. Analyzer: stop guessing which proxy session

**Priority P2 · Status TODO · Blocked by nothing**

**Goal.** Score the proxy session that actually corresponds to the BZLogger.

**Why.** Proxy logs are append-only across sessions, and the analyzer picks the
**largest** one as the byte-count denominator (~line 594). KingFurykiller's
`winmm_proxy.log` holds **ten** `session end:` blocks from earlier processes,
and the crashed pid has none at all — so a crash bundle is silently scored
against a different, older session. The output does say
`check that session is the one this BZLogger covers`, which is not a fix.

**Files.** `tools/analyze_drops.py` — `proxy_log_health()` (~line 659) and the
denominator selection (~line 594).

**Implement.** Match by pid and time window: the proxy log's per-pid first and
last timestamps against the BZLogger's session span. If no session overlaps,
say so and omit the percentage rather than printing one from the wrong session.

**Acceptance.** The 08-09 session-A KFK bundle (crashed pid, no session-end
stats) reports "no matching proxy session — crashed before teardown" instead of
a byte share computed from an unrelated session.

---

### T12. File the mod bug upstream

**Priority P1 · Status REPORTED 2026-08-10 — awaiting upstream release**

Reproduced, fixed, verified and handed to the mod author with the writeup and a
tested patch. Two defects, both measured before and after; details in §A5 and
[`resources/navfix/`](resources/navfix/).

**Not closed.** Nothing has changed for anyone playing until the author
publishes it and the crew updates. Until then:

- plan as though the storms are live, because they are;
- [T1](#t1-sharedsend_dampenh--the-duplicate-suppressor)/[T2](#t2-wire-the-damper-into-both-proxies)
  stay P0;
- [F6](#f6-re-measure-once-the-mod-fix-ships) cannot run.

**Watch for:** a workshop update to 3406347034. When it lands, diff
`SBPNavLogic.lua` against `resources/navfix/SBPNavLogic.patch` to see whether
both defects were taken or only one, then run F6.

<details><summary>Original task text</summary>

**Blocked by F1 (preferred, not required)**

**Goal.** Get the Nav beacon fixed at source.

**What to send.**
[`resources/CAMERAPOD_STORM.md`](resources/CAMERAPOD_STORM.md) is written as a
standalone bug report for the author of Steam Workshop **3406347034** (Strat
Balance Patch / BZP). Send the file link; it carries the measurements, the
mechanism, four defects with line numbers, and Lua patches syntax-checked under
5.4.

**Lead with the fix, not the ODF.** `omegaSpin = 1.0` looks like the culprit and
the dataset falsifies it twice over — `apcamr349` had the identical ODF, was
spinning, and sat at 1 Hz for twelve minutes. The fix is the velocity pin in
`Update()`.

**Run [F1](#f1-nav-beacon-storm-test) first if the crew can manage it**, so the
report carries a reproduction rather than a correlation. Do not block
indefinitely on it — the measurements stand on their own.

</details>

---

### T13. Merge the two storm documents

**Priority P3 · Status TODO**

`resources/RETRANSMIT_STORM.md` (2026-07-26) and
`resources/CAMERAPOD_STORM.md` (2026-08-09) describe the same phenomenon.
V4.93 claimed they were structurally different; that is retracted
(§A2.2 in the [Evidence archive](#evidence-archive)).

Keep `CAMERAPOD_STORM.md` as the mechanism and current bug report. Reduce
`RETRANSMIT_STORM.md` to a historical note pointing at it, preserving the
2026-07-26 measurements and the record of what was concluded wrongly and why.
Its "what to test next" list is superseded.

---

### T14. Delete or regenerate the bogus reorder resources

**Priority P3 · Status TODO**

`resources/valid_capture_reorder_signal_only.md` and
`resources/valid_capture_reorder_signal_clusters_250ms.md` are byte-identical
to each other and both derived from a wrong sequence offset. They carry a
banner saying so. Either regenerate them against the settled field
(`resources/BZ_P2P_HEADER.md`) or delete them — a file that exists only to
say it is wrong is worse than no file.

---

### T15. Map the remaining log switches

**Priority P2 (was P3) · Status PARTLY ANSWERED · Blocked by nothing**

**Goal.** Find what `bzrnetlog` and `netlog` do — and what the *higher levels*
emit, since everything captured so far is level 1.

**Why.** `-netpktlog` turned out to expose the application layer, the send
decision, the position-broadcast interval and the game's own RTT — none of
which anyone in this project knew existed, and all of which were decisive
(§A5). Two switches remain unexplored, and both take a **value**, so they are
probably verbosity levels:

```
netlog / netlog= / nonetlog
bzrnetlog / bzrnetlog= / nobzrnetlog
bzrnetport=   bzrserver=   ipdirect   iprelay
```

The transport-level lines named in the binary — `BZRNet P2P REL/UNR/BAS/CON
Sent Packet`, and the inbound `Received Packet … sent at %llu, received at
%llu, diff %lld` — do **not** appear with `-netpktlog` alone. One of these
should produce them, and that inbound line carries per-packet one-way timing,
which would give [T1](#t1-sharedsend_dampenh--the-duplicate-suppressor) a
second, independent RTT source.

**Mostly answered from a decompilation** — see
[`resources/RELATED_PROJECTS.md`](resources/RELATED_PROJECTS.md). The parser at
`FUN_007d5120` sets two independent log levels:

| switch | effect |
|---|---|
| `-netpktlog`, `-netlog` | `if (DAT_009180d8 < 1) DAT_009180d8 = 1` |
| `-netlog=<n>` | `DAT_009180d8 = atoi(n)` — **arbitrary level** |
| `-bzrnetlog` | `DAT_008eda28 = 1` |
| `-bzrnetlog=<n>` | `DAT_008eda28 = atoi(n)` — **arbitrary level** |
| `-nonetpktlog`, `-nonetlog`, `-nobzrnetlog` | set the respective global to 0 |

So everything captured so far is **packet log level 1** — the floor. `-netlog=2`
and above is untried, and is the likely home of the transport-level
`REL`/`UNR`/`BAS`/`CON` lines and the inbound `Received Packet … diff` timing.
`DAT_008eda28` is untouched entirely.

**How.** Two-minute cycles: launch, main menu, quit, grep `BZLogger.txt` for
`REL Sent` / `UNR Sent`. Try `-netlog=2`, then `=3`, `=9`; then `-bzrnetlog=1`
upward. The game echoes its own command line
(`Battlezone98 Command-Line:`), so a typo shows immediately. Record findings in
[`docs/PACKET_LOG_TEST.md`](docs/PACKET_LOG_TEST.md).

**Both are plain data addresses**, so the proxy could raise them at runtime
instead of requiring a launch-option change — worth considering once the levels
are mapped, since it would let the wrapper enable capture for one session
without the crew editing Steam settings.

**Do not** run an unknown switch in a real match until its log volume is
known — `-netpktlog` alone turned a 6 MB log into 83 MB.

---
## Field sessions

Everything here needs people in a lobby. The tooling for all of it exists.

**Three rules that apply to every session, learned the hard way:**

1. **One host for every arm.** Host role is a controlled variable
   ([`docs/TESTING.md`](docs/TESTING.md)). Every anomaly in the dataset splits
   on host vs client, and no A/B has ever held it constant.
2. **Everyone on the same build.** Check the proxy build id line before
   starting. Three evenings have been muddied by skew; [T6](#t6-build-skew-guard)
   automates the check.
3. **Announce each arm in the channel with a timestamp.** That is the only way
   the arms get separated afterwards. Bundles carry no arm label unless
   `bznet.env` sets one.

---

### F1. Nav beacon storm test

**Priority P0 · Status DONE 2026-08-10 — cause found; fix not yet deployed**

Superseded by events. The storm was reproduced on demand, traced to two defects
in the mod, patched locally and re-measured; see §A5 and
[`resources/navfix/`](resources/navfix/). The arms below were designed before
the trigger was known and are kept as the record of how it was narrowed —
driving, selection and placement were all eliminated by them.

**The fix is not in anyone's hands but ours.** See
[T12](#t12-file-the-mod-bug-upstream).

<details><summary>Original task text</summary>

**Blocked by crew availability**

The full runbook is [`docs/NAV_STORM_TEST.md`](docs/NAV_STORM_TEST.md) — four
8-minute arms, no code, same map and player count throughout.

**What it settles.** Whether the runaway beacon is caused by placement (slope,
stacked, mid-air) or by delivery (armory-launched navs, items snapped onto
them). Both are consistent with the logs and they point at different fixes.

**Score it on per-beacon emission rate**, not retransmit totals: ~1 Hz settled,
anything above ~5 Hz is a runaway. [T7](#t7-analyzer-per-object-emission-rate)
makes that a standard output line; until then use the decoder in
`resources/CAMERAPOD_STORM.md`.

**Cheaper alternative worth trying first.** Two short-lived runaways in the
existing data self-terminate — `apcamr342` (08-08, 16:39–16:41) and
`apcamr595` (08-09, 14:53–14:54). If anyone can make a beacon spike for twenty
seconds and then settle *on demand*, that is worth more than four clean arms
and takes minutes.

**Feeds:** [T12](#t12-file-the-mod-bug-upstream).

</details>

---

### F2. Ramp knob A/B

**Priority P1 · Status TODO · Blocked by T3, T6**

**Hypothesis.** `DownCount` 50→200 and `UpCount` 100→50 make the governor back
off ~4× faster, shortening a storm without capping headroom.

**Arms**, set on the **host** via `bznet.env` in the config directory
(`%APPDATA%\bz-netcode\` / `~/.config/bz-netcode/`), one `BZ_KEY=value` per
line:

| arm | `BZ_NET_DOWNCOUNT` | `BZ_NET_UPCOUNT` |
|---|---|---|
| A (current) | 50 | 100 |
| B (proposed) | 200 | 50 |

**This is the first real use of `bznet.env`.** It has shipped since V4.92 and
`overrides=` is empty in every bundle ever collected. Verify before starting
that `meta.txt` actually records the override — if it comes back empty, the arm
did not apply and the evening is void.

**Score on:** time from storm onset to storm end, and the governor's descent
rate. **Do not score on vbgthykuj-style data** — that match was aborted mid
descent and shows nothing about recovery. Peppy and the 08-03 session are the
comparable shapes.

**Confound to watch.** If [T1](#t1-sharedsend_dampenh--the-duplicate-suppressor)
has shipped by then, the damper also shortens storms. Run this arm with
`BZ_SEND_DAMPEN=0` on every machine, or you are measuring two changes at once.

---

### F3. `BZ_GOV_START` A/B — 16000 / 40000 / 80000

**Priority P2 · Status TODO · Blocked by T6**

**Score the host's log.** Confirmed twice now that the governor value is
host-driven and every client mirrors it, so a client-side arm mostly measures
the host's setting.

**Validity gate.** The report's `actual send rate … % of budget` line decides
whether the sample counts: under 60% the budget was never the constraint and
the arm proves nothing; over 90% it is a real sample. Discard, don't average.

**Arms** via `bznet.env` on the host. Same map, same peers, same host.

---

### F4. Buffer capture re-run

**Priority P3 · Status TODO**

The only good capture covers 8.1 of 15.2 minutes and misses the match start.
Cause is known and instrumented: `BZ_BUFFER_LOG_RING` was not in the game's
environment because the game launched before the launch options were pasted.

`stop` now prints `CAPTURE IS NOT CLEAN` and writes `capture_verify.txt`, and
`BZ_BUFFER_LOG_PEER` genuinely filters, so a smaller ring covers more match.

**Read `capture_verify.txt` before relying on the capture.** Two open
sub-items: the wrapper does not include `capture_verify.txt` in the bundle, and
the meta should state `wrapped=yes, first_retained_event=N` plainly so nobody
scores a partial capture by accident.

---

### F6. Re-measure once the mod fix ships

**Priority P1 · Status BLOCKED · Blocked by
[T12](#t12-file-the-mod-bug-upstream) — the mod author publishing the fix, and
the crew updating**

**Cannot run yet.** The fix is verified locally but unpublished, so a crew
evening today still measures the flooded baseline. Do not treat local patched
runs as the new normal either — only one machine has them.

Every bandwidth number this project has published was measured on a mod that
was flooding the reliable channel. With both defects fixed (§A5), the baseline
has moved and nobody knows where to.

**What to re-measure**, one ordinary crew evening with the fixed mod:

- Does the governor still climb into the 100k–145k range, or does it settle
  lower now the reliable stream is quiet?
- Is `MaxBandwidth = 320000` still justified, or was it compensating for the
  storms? [T3](#t3-ship-the-ramp-knobs-as-defaults) and
  [F2](#f2-ramp-knob-ab) are both scored against pre-fix behaviour.
- Do the `#0 expected` desync signatures and the discard rates change?
- What does [T1](#t1-sharedsend_dampenh--the-duplicate-suppressor) actually
  save in a healthy match? That number decides whether the damper ships on by
  default.

**Nothing else should be A/B'd until this is done** — the control arm for every
existing comparison is contaminated.

### F5. Map the NPPI keys

**Priority P3 · Status TODO**

`MaxPingsLost`, `LimitLowNPPI`, `LimitHiNPPI`, `DivisorMPPI2NPPI`,
`DivisorPing2NPPI` (`net-ini/README.md`) have no located globals. Find them the
way the ten known keys were found. Even stock-value confirmation closes the
file. `MaxPingsLost` now has a confirmed live address from the 2026-08-03
values-as-found read, so this is partially started.

---
## Evidence archive

Why the tasks say what they say. Findings per session, in date order, including
the ones later retracted — **struck, not deleted**, because the record of how a
wrong conclusion was reached is worth as much as the right one.

Numbering: **A1** = 2026-08-02, **A1b** = 2026-08-03, **A2** = 2026-08-04,
**A3** = 2026-08-08/09, **A5** = 2026-08-10 (the cause, found and fixed),
**A4** = the corrections log, which sits last because it spans all of them.
Sub-items keep their original numbers, so "A3.3" is item 3 of the 2026-08-09
findings.

**Before you cite a number from here, re-derive it.** Four figures in the first
draft of A3 were wrong, every one because it was copied forward rather than
measured. See [§A4](#a4-corrections-log).

---

## A1. V4.91 — from the 2026-08-02 evening (first `gamewait` bundles, 7 players, 9 matches)

Three bundles arrived from one evening: the host (Windows), one Snap client,
one native-Linux client. First uploads ever from the new wrapper generation on
all three platforms — the pipeline works. What the data says, in priority
order:

### 1. The Windows bundle was empty — `game_dir` resolves to the launcher folder

The host's bundle contained `meta.txt` and nothing else, and the meta shows
why: `game_dir=...\Battlezone 98 Redux\\launcher`. Steam's `%command%` names
`launcher\BZLauncher.exe`; `Get-GameDirFromCommand` returns that exe's parent
and is never corrected, so the wrapper harvested `BZLogger.txt` /
`winmm_proxy.log` from the launcher folder, where the game writes nothing.
The launch-side code already walks up to the folder holding
`battlezone98redux.exe` for the working directory (`bz_wrap.ps1` ~line 485);
apply the same correction to `$gameDir` itself. Also bundle the tail of
`bz_wrap.log` so a future empty bundle explains itself. **This blocks the
"one Windows bundle, of any kind" item below — the host's proxy measurements
are the single most wanted dataset and they evaporated on harvest.**

### 2. Governor read-back cries wolf — every real match is declared invalid

Eight matches logged `POKE DID NOT HOLD — wrote 40000, reads 39650..39900 a
few seconds later`. A read of 39650–39900 is not a revert: it is 40000 minus
a few `DownCount=50` steps — the game's own governor adjusting off the poked
baseline, which is the poke *working*. The only matches that "held" were the
idle ones where no traffic ever moved the value. Fix the verdict: fail only
on a revert toward stock (read ≤ 16000, or == 4000), otherwise report "poke
held, governor active (read N)". `analyze_drops.py` must stop discarding
these as invalid samples — as shipped, every meaningful `BZ_GOV_START` A/B
arm would throw itself out.

### 3. The client-log governor line looks host-synced — A/Bs must score the host's log

Both client BZLoggers show byte-identical governor trajectories (301/296
adjustments, 36200 → 159400 B/s, 40000 at 0.0 min, 80000 at ~4 min) on
different machines, different links. That reads as a host-driven value every
client mirrors, not a local measurement — meaning a client-side
`BZ_GOV_START` A/B mostly measures the host's setting. Verify against the
host's own log (blocked on item 1), then make `analyze_drops.py` say whose
governor it is scoring. Strengthens the standing "host role is a controlled
variable" rule.

### 4. One proxy never poked at all on the Snap box — and the analyzer mislabeled it

The Snap client's proxy is a fresh V4.9 build (built one minute before the
session) with read-back on, yet logged **no** `cold-start caught`, no
`MaxBandwidth 4000 -> 320000`, no verdict lines, across two sessions with
traffic. Either the globals were already at target when it looked (net.ini
mod present → nothing to change → silence) or the catch missed. Add one
unconditional line at match start stating the values as found — silence is
currently unreadable. Separately: `analyze_drops.py` printed "pre-V4.9
proxy" for this log purely because no verdict lines exist; wrong label,
fresh build. Distinguish "old proxy" from "no poke happened".

### 5. Host→Snap-client link anomaly: resend 112/min, `ahead` 9.8%

In the Snap client's log, traffic **from the host** shows resend=1864
(112/min — thirty times the other links) and ahead=470, which the analyzer
itself flags as breaking its sequence model (baseline ~0.2%). Every other
sender pair in both logs is quiet. Real reverse-path loss (client's acks not
reaching the host) would look like this — and so would a sequence-model bug.
The host-side view of the same link would settle it, and it was in the
bundle that came back empty (item 1). Re-run after item 1 ships.

### 6. Buffer capture wrapped again — ring sized for half an evening

`bz_buffer_log.meta.txt`: ring_records=65536, records_written=65536,
total_events_seen=126715 — the ring wrapped and the capture lost everything
before the ~52% mark, missing the match starts a second time. The tooling to
prevent this exists (`BZ_BUFFER_LOG_PEER` filter, bigger ring,
`capture_verify.txt`) but the wrapper bundle does not include
`capture_verify.txt` — add it to the harvest list, and consider having the
meta state plainly `wrapped=yes, first_retained_event=N` so nobody scores a
partial capture by accident.

### 7. Analyzer scores one match per log; the evening had nine

`analyze_drops.py` reports the **last** lobby in a BZLogger (16.6 min scored
out of a ~2 h, 9-match log). The first match of this evening was the
2-player configuration the retransmit-storm item wants (storm was only ever
seen at 2 players; this session's 7-player matches all sat at a quiet
1.30–1.38 copies/message — the player-count hypothesis survives another
dataset). That 2-player match is sitting unscored in the same file. Make the
analyzer emit a per-match table (map, players, duration, discards/min,
retransmit copies, governor span) instead of last-match-only.

### 8. Small but real

- The proxy logs `session end: send_stats` / `reorder_stats` **twice** at
  teardown on both Linux boxes — double-fire in the unload path, harmless
  but it doubles counters for anything that greps naively.
- Pacing measure-only counters came back with data: peak_bps ≈ 150–180k,
  `burst_seconds` ≈ 69% of wall-clock on both clients. Whatever pacing
  decision V4.91 makes should start from these two logs.
- The observer flood is now tied to the crew's own loadout: both collected
  `multi.ini`s say `vehicle = bvobserv`, and the flood ran 4.5–10k lines/min
  (75k–171k lines) — it is why BZLoggers balloon to tens of MB. This session
  ended cleanly, so the flood alone does not crash the game. Cheapest next
  probe stays: one session on a non-observer vehicle, watch both the log
  size and the storm/crash items.
- No kick or lag-removal event again, 7 players for 2 hours — the auto-kick
  relax (below) remains unobserved in the wild.
- One tester's uploader is still blocked by their AV (README now documents
  the exact signature and recovery); their bundle is the missing fourth view
  of this session.

---

## A1b. 2026-08-03 — the first complete Windows bundle

The highest-value item on the V4.91 list was "one Windows bundle, of any kind":
every proxy-side measurement until then came from one Linux box while three of
four regular testers were on Windows. It arrived on 2026-08-03 — 27 MB
BZLogger, the first `winmm_proxy.log` ever, the wrap-log tail, split into three
parts by the oversize path and reassembled clean. The 95 MB prelaunch snapshot
also recovered the **entire** lost 2026-08-02 host-side evening, all nine
matches.

What the recovered + fresh data settled:

- **Host governor: poke held in all 5 matches** (reads 40000–40100, V4.91
  verdict logic), and the host log's trajectory (36200 → 159400) matched what
  both clients mirrored on 2026-08-02 — host-sync confirmed, governor A/Bs
  score the host's log. Host ran at 81% of its own budget, so it is a valid
  `BZ_GOV_START=40000` sample.
  *(See [§A4](#a4-corrections-log) item 11: "byte-identical" was the wording
  used here and it is too strong — host-synced, sampled at different phases.)*
- **Ground truth on the `[Net]` globals as found**, identical on the Windows
  host and a Linux client, pre-write, menu phase: `MinBandwidth=1000
  MaxBandwidth=4000 UpCount=10 DownCount=5 MaxPing=700 MaxPingsLost=20
  AutoKickStart=10000 AutoKickPing=1000 AutoKickLoss=50 AutoKickTime=10000`.
  Several **differ from the documented stock** (750/25/15000, MaxPing 300,
  MaxBandwidth 16000). `shared/net_globals.h` now carries the comparison table
  and the phase caveat. `MaxPingsLost` gained a confirmed live address, which
  partly closes [F5](#f5-map-the-nppi-keys).
- **First host-side retransmit view**: 254 datagrams/min at 2.70 copies per
  message — an order of magnitude more than any client, and concentrated
  per-peer (68% aimed at one player). Read at the time as "host-to-lossy-peer,
  not lobby-wide"; superseded by A3.1, which identifies the object rather than
  the link.
- **Player-count hypothesis weakened**: five 2-player matches were all
  storm-quiet (8 retx/min), so 2 players alone does not cause it. Killed
  outright a week later — see [§A4](#a4-corrections-log) item 4.
- **A distinct crash class**: the host's log ends mid map-load ("Preload Meshes
  Results … 165 additional meshes required", then nothing) directly after a
  clean match teardown. Not the same shape as the 2026-08-09 in-match crash
  (A3.5). Map-load after teardown is its own suspect and still unexplained.
- **Stale-capture gotcha**: the wrapper harvested whatever `bz_buffer_log.*`
  sat in the game dir, so a session with capture disabled shipped the previous
  day's ring looking current. Fixed in V4.92 — files older than session start
  are skipped.

---

## A2. V4.93 — from the 2026-08-04 game (5 players, host bundle + 3 clients)

Bundles in `new logs/`: King (host, V4.91), Monkey (V4.92), jackassbison
(V4.91), PiercingXX (V4.92); judgeguns was the 5th player and sent nothing.
Two matches — 01:32–01:47Z with 4 players, 01:49–02:06Z with 5. All four
bundles exit 0, no crash, no UAF signature.

**This is the session where a storm was survived rather than fatal.** It is
also the first host-side proxy dataset of a storm in progress, which is what
makes the rest of this section possible.

### 1. The auto-kick relax is confirmed working — close item 6 below

Retransmit storm 01:56:16Z → 02:01:08Z, ~4m52s. Zero kicks, zero timeouts,
zero disconnects; all 5 players present at exit. The relaxed thresholds
(2000ms/200/60s/60s) were confirmed applied on the host, and auto-kick is
host-enforced, so the host's build is what governed. This is the observation
"Auto-kick relax has never been observed working" was waiting for — it did not
need the deliberate throttle test.

Caveat for the write-up: King's `values as found` were 1000/50/10000/10000,
**not** stock 750/25/15000/10000 — his net.ini had already partly relaxed
them. The measured delta is 1000→2000ms and 50→200, not the full stock gap.
And BZLogger carries no ping/loss telemetry outside the match-start blocks, so
"stock would have kicked" stays inference, not measurement.

### 2. Player count confirmed as the storm trigger — but it is a *different* storm

Item 2 under play sessions ranked player count first. The data agrees, in the
same session with everything else held constant:

| | players | peers | governor peak | storm |
|---|---|---|---|---|
| Match 1 | 4 | 3 | plateaued **86,650**, 5 min stable | no |
| Match 2 | 5 | 4 | reached **100,150** | **yes** |

The host sends one copy per peer — King's proxy measured peak 254 kB/s against
clients' 78–98 kB/s, almost exactly 3–4×. The 5th player added a fourth
outbound copy of everything and pushed the governor past match 1's stable
plateau into the breaking regime. **Host upstream is the scaling wall.**

> **RETRACTED 2026-08-09 — see V4.94 §2.** Both the "two distinct phenomena"
> conclusion and the 4,336 figure below are wrong. Re-scored, this storm is
> 54,996 distinct messages at 3.83 copies in a 48 ms window — the same shape as
> 07-26 and 08-08 — and the maximum copies of any single `(peer, seq)` in the
> log is **13**, not 4,336. Left in place as the record of the error.

~~**Do not merge this with `resources/RETRANSMIT_STORM.md`.** That documents
proactive redundancy — every message 3.57× inside 46 ms, never again. This one
is the opposite shape: one packet resent **4,336 times** over minutes. They may
be two distinct phenomena and the file should say so until proven otherwise.~~

### 3. Mechanism: go-back-N with no congestion window

Over the storm the host's **seq advanced 1,081 → 16,240 while its ack field
moved only 790 → 1,440** (~2 acks/sec). It resends from the oldest unacked
packet forward on every retry timeout, so the backlog grows, every retry pass
gets longer, and load grows quadratically. Peak 2,964 retransmits/s; 231k of
the session's 238k total inside those 5 minutes; spread evenly across all four
peers (31/31/27/11%), so no single bad client.

89% of client-side "drops" were duplicates (delta −1..−11), only ~11% real
gaps — consistent with the standing rule that drop totals overstate loss.
Same `#0 expected` desync signature as 2026-08-01, on all four logs including
the host, concentrated 01:56–02:00.

Recovery came from the governor: send budget walked 100,150 → 43,250 and the
storm stopped *exactly* as it bottomed out. Starving the retry loop is what
broke it — the kick relax only bought the time.

### 4. The ramp knobs are tuned backwards — cheapest fix, no code

`UpCount` is patched 10→**100** and `DownCount` 5→**50**: the governor climbs
twice as fast as it backs off, and 10× faster than stock in both directions.
That is inverted for a congestion controller, and it ramps until it induces
loss because nothing in the loop treats loss as a stop signal.

The patch did not create the pathology — 2026-08-01 stormed at ~360/s — but
raising MaxBandwidth 20× and UpCount 10× lets the governor climb into a regime
where it is far more violent (2,964/s here). The fix is not to revert.

**Next session's A/B, in priority order (this supersedes item 5 below with a
motivated hypothesis rather than a blind sweep):**

- `BZ_NET_DOWNCOUNT` 50 → **200**, `BZ_NET_UPCOUNT` 100 → **50**. At the
  observed −3000/15s this should give ~−12,000/15s: the 100k→43k walk that
  took 285 s becomes ~70 s, cutting this storm from ~5 min to ~1.5. Same
  recovery mechanism that already works, four times faster.
- `BZ_NET_MAXBANDWIDTH` 320000 → **~85000** on the host — just under match 1's
  stable plateau, so the governor cannot climb into the breaking regime.
  320k is far above what King's link demonstrably carries.

Hold host role constant (King) or the arms are not comparable.

### 5. Send-side retransmit damper — the real fix, and the primitives exist

Verified against tonight's payloads, so this is not speculative:
`reorder_is_retransmit()` (bit 7 of byte 0) fired on **144,228 of 144,232**
storm packets, and `reorder_seq_from_payload()` at offset 10 reads
`00 00 1c d8` = 7384, exactly matching the logged `(7384,1220)`. Ack offset 14
reads 1220, also matching. Both offsets are correct on live field data.

So the `WSASendTo` hook can already identify a retransmit and its sequence.
Add a per-(peer, seq) table with exponential backoff — a given seq goes out at
most once per 50 ms, then 100, 200, 400. Since ~89% of what arrived was
duplicate, suppressing most of it costs almost nothing and frees the uplink for
the acks that break the loop. Fail-safe: suppress wrongly and the game's own
retry timer fires again. Belongs in `shared/` with a unit test, same pattern as
`send_pace.h`.

### 6. Make `gov_trace` correct, not just observe

Its header says *"This observes; it does not correct... the A/B has not been
run yet."* It has been run now. The proxy already writes the send-rate global
at cold start and asserts `[Net]` every 100 ms, and `send_pace` already
measures outbound pps — so it can do multiplicative decrease from outside: on
storm signature, halve the global. That is what item 4 approximates with knobs.

### 7. Do **not** turn on `BZ_SEND_PACE` for this

A token bucket spreads the storm rather than stopping it — the backlog still
grows, the 256-slot queue would thrash `queue_full_passed` at 2,900 pps, and it
adds latency to exactly the ping traffic auto-kick measures. The pacer is right
for a bursty sender; this is a runaway window.

### 8. Smaller things from this session

- **Governor freeze is a useful tell.** It is host-synced, so a client that
  stops receiving updates freezes at its last value — jackassbison sat at
  83,700 for ~3.5 min mid-storm, then snapped to the host's current value.
  Worth teaching `analyze_drops.py` to flag: it identifies starved clients
  without needing their own telemetry.
- **`POKE DID NOT HOLD` still cries wolf on Linux.** PiercingXX logged it at
  match start (reads 4000 after 2504 ms) and jackassbison at 100 ms, yet both
  traces show ~39,000 at the next sample. Item 2 under V4.91 narrowed the rule
  but this case — a client re-syncing to the host's value at join — is not
  covered.
- **Version skew:** King and jackassbison were still on V4.91-harvest. Get
  everyone onto one build before the next A/B or the arms are muddied.

---

## A3. V4.94 — from the 2026-08-08 and 2026-08-09 evenings (13 bundles, 3 dates)

Bundles in `new logs/8-3`, `new logs/8-8`, `new logs/8-9`. 8-3 is the session
already written up as V4.93 above, re-scored here against the new finding.
8-8 is a 2-player KFK/PiercingXX evening, 6 matches. 8-9 is two sessions, six
players, five matches, **one crash to desktop and two lag spikes**.

**This is the session that named the storm.** The retransmit storm is one object
type, in every storm on every date: the SBP mod's Nav beacon
(`apcamr`/`spcamr`/`cpcamr`, `classLabel = "camerapod"`). Full analysis, exact
files and lines: [`resources/CAMERAPOD_STORM.md`](resources/CAMERAPOD_STORM.md).

### 1. The storm is the Nav beacon — 91–99% of every storm's datagrams

| date | match | players | sender | retx | share `*camr*_camerapod` |
|---|---|---|---|---:|---:|
| 08-03 | control | 5 | KFK (host) | 210,673 | 94.8% |
| 08-08 | host | **2** | KFK (host) | 100,476 | 94.8% |
| 08-09 | vbgthykuj | 6 | KFK (host) | 122,918 | 96.8% |
| 08-09 | Peppy | 6 | KFK (**client**) | 50,170 | 87.1% |

Shares are of *all* retransmitted datagrams; the remainder is mostly 35-byte
control messages with no name field. The message is the 78-byte `7a75` type
`RETRANSMIT_STORM.md` already identified.

**Do not repeat the "storm window equals beacon lifetime" line** from the first
draft of this section — it is circular. The only source for a beacon's lifetime
*is* its own payload stream, so it reduces to "the storm's packets span the
storm". In two of the four cases the apparent "beacon death" is just the abort
button (08-08 at 16:54:23, vbgthykuj at 14:49:31).

Three things this overturns:

- **Player count is dead as the trigger.** The 08-08 storm was 2 players,
  100,476 datagrams, 4.06 copies. Supersedes V4.93 §2 and item 2 under play
  sessions below. More peers multiply the cost; they do not start it.
- **It is not a host phenomenon.** In `Peppy`, Awildbison hosted and sent 1,647;
  KFK as an ordinary client sent 50,170. Whoever owns the beacon pays. This also
  explains why every storm in the set is KFK's machine — he places the navs, and
  the mod's own lag manager refuses to move camerapod locality
  (`ZTEMP_SBPLagMgr.lua`).
- **It is one beacon, not the class.** 08-08, same map, same ODF, same 12
  minutes: `apcamr346` = 92,090 datagrams, `apcamr349` = 2,636. A runaway beacon
  is a per-instance condition.

Beacons split cleanly into two populations by update rate, measured from the
sender's own log:

| beacon | lifetime | update rate |
|---|---:|---:|
| `apcamr342`, `apcamr349` | 110 s, 716 s | **0.8, 1.0 Hz** |
| `apcamr585`, `apcamr346`, `apcamr236`, `spcamr1127` | 254–675 s | **20, 33, 43, 51 Hz** |

A settled beacon sends ~1 Hz; a runaway sends one reliable message per frame to
every peer for its whole life — `apcamr346` never calmed down once across
twelve minutes.

Two traps, both of which the first draft of this section fell into:

- **Rate is the only discriminator.** The per-message state bytes are unique on
  every message for the *quiet* beacons too (`apcamr349`: 739 messages, all
  distinct), so "its transform never settles" proves nothing on its own.
- **`omegaSpin = 1.0` is falsified twice over.** The game's own object
  snapshots show every beacon's rotation matrix differing at every sample while
  position holds still — they all spin — and the quiet ones still sync at 1 Hz.

The quiet rate is not arbitrary: 739 messages / ~700 s = **1.05/s**, which is
exactly `NavComputeFrequency = 1`. The mod's own 1 Hz sweep sets the floor, and
nothing in the Lua touches a nav per-frame — so the runaway source is physics,
a beacon that never comes to rest. Object snapshots confirm unsettled beacons
exist inside the storm windows (08-03 `F06004BA` drifting with height
oscillating below its rest value; vbgthykuj `F4A0039F` moved ~55 m mid-storm),
though handle-to-instance mapping does not exist so they cannot be tied to a
named runaway.

The redundant copies alone cost **55–71% of the sender's governor budget** in
the two worst matches (27.3 kB/s against a 38.6 kB/s budget on 08-09
vbgthykuj), before counting the original transmissions.

### 2. Two storm shapes are one shape

V4.93 §2 said *"do not merge this with RETRANSMIT_STORM.md — they may be two
distinct phenomena."* They are not. Re-scored, the 08-03 storm is 54,996 distinct
messages at **3.83 copies inside a 48 ms median window** — the proactive-
redundancy shape, same as 2026-07-26 (3.57 / 46 ms) and 08-08 (4.06 / 50 ms).
V4.93's "one packet resent **4,336 times**" is **not reproducible from that log
at all** — the maximum copies of any single `(peer, seq)` in the 08-03 storm is
**13**. Wherever that number came from, retract it. Merge the two files, keeping
`CAMERAPOD_STORM.md` as the mechanism and `RETRANSMIT_STORM.md` as its history.

The ack-stagnation half of V4.93 §3 is real, and worse on 08-09 than on 08-03:
in vbgthykuj, seq to peer 76.82.183.105 climbed to 11,587 while that peer's ack
field never exceeded 690. But it is the consequence of a ~200 msg/s source, not
an independent cause. Note also that vbgthykuj's own copies/window (2.47 copies,
17 ms) sits *between* the two supposed "shapes", which is a further argument
that there was only ever one.

### 3. The down-walk rate is measured — but vbgthykuj is NOT a recovery

08-09 `vbgthykuj`, KFK hosting:

```
14:43:23    match start, poke 4000 -> 40000, held
14:44:35    governor climbing:  39,500
14:45:27    peak:               56,900   <- storm ignites this minute (33,809 retx)
14:45:29 .. 14:49:30    -400 every 2 s, strictly monotonic (119x -400, 2x -450)
14:49:30    last in-match value: 8,400, STILL DESCENDING
14:49:31.3  last retransmit of the storm
14:49:31.5  "Game stopped in multiplayer due to abort button"
```

**Correction to the first draft of this section: there is no floor and the
storm did not stop at one.** 8,150 appears nowhere in BZLogger — it is a
*post-abort* proxy trace sample at 14:49:36. The match ended because somebody
hit abort while the walk was still going and the storm was still at full rate.
vbgthykuj therefore demonstrates the **down-walk rate** (−400/2 s = −3,000/15 s)
and nothing about recovery.

Recovery-by-starvation is supported only by `Peppy` (budget bottoms at
**10,650** at 14:59:55, storm stops ~15:00, stays dead for 19 min — note 19,050
is the post-storm *recovery plateau*, not the floor) and by the 08-03 session.

`BZ_NET_DOWNCOUNT` 50 → 200 still predicts ~−12,000/15 s, but score it against
Peppy and 08-03, not vbgthykuj.

Note the peak is not a fixed threshold — 56,900 stormed here, and KFK's host
governor reached **145,900 in session A of the same evening without storming**.
The governor level is not the trigger; it is the amplifier and, sometimes, the
brake.

### 4. Host-sync confirmed across OSes — but not "byte-for-byte"

KFK's and Awildbison's `governor_trace` during `Peppy` track the same
host-driven value on different machines, links and OSes (Windows / Snap Linux).
They are **not identical**: aligned by wall-second, about half the samples
differ, some by a lot — 30,850 vs 28,600, 18,900 vs 16,600. That is two clients
sampling different phases of a −400/2 s walk, which is strong evidence for
host-sync and no evidence for "identical". V4.91 §3 inferred host-sync; this
confirms it. Governor A/Bs score the host's log, full stop.

### 5. The crash to desktop — one sample, and no dump could have existed

KingFurykiller, hosting `push` (uesrtst1, 6 players), 2m40s in:

```
14:37:04.015  Chat Message: Player Left: aggressor (3,3)
14:37:04.015  Removed Player aggressor (ID 3, Team 0)  5 players remaining
14:37:04.017  BZRNet P2P Removing Player S76561199317457354 From P2P Handler
14:37:04.04   burst of Dropping Packet #491..#502 (#503 expected) from PiercingXX
14:37:08.247  last line written; no 'Exiting Game With Return Code', no teardown
14:37:10.7    peers see the host gone; host migration
```

(The final line is *complete*, terminator and all — the first draft said
"stops mid-line", which is wrong. What is missing is the shutdown sequence.)

Not the storm — that match was quiet (35 pod datagrams in the minute). Not the
`observer.mesh` flood — the whole log contains **one** such error. The shape is
peer-removal teardown, 4.2 s after the removal, but **it is one occurrence** and
the only comparable mid-match departure in the set (14:30:41, `comeback`) landed
on a match that was ending anyway. Treat as a lead, not a diagnosis.

Two blockers it exposes, both ours:

- **KFK is on the V4.91 wrapper**, which does not write `crash_capture=` at all.
  No dump exists and nobody could have known.
- **`game_exit_code=0` is the launcher's exit code, not the game's.**
  `bz_wrap.log` cheerfully logged `game exited with 0` for a process that died
  without running `DLL_PROCESS_DETACH`. The wrapper currently cannot distinguish
  a CTD from a clean quit, on either platform.
- **A denominator trap for the analyzer.** KFK's `winmm_proxy.log` is
  append-only across sessions and holds **ten** `session end:` blocks from
  earlier processes; only the crashed pid (27248) has none. Anything that takes
  "largest of N sessions" from that file silently scores a different, older
  session. Fix alongside D below.

### 6. The `observer.mesh` flood is largely exonerated

- 08-08 KFK: flood at 17:21 (4,033 errors). The storm was 16:43–16:54. No
  overlap.
- 08-09 vbgthykuj storm (14:45–14:49): no flood at all.
- The crash session contains 1 observer.mesh error, total.
- **But it does overlap the Peppy storm.** The flood starts 14:57 (998, then
  19,581 and 18,228) while Peppy's storm ran through 14:59. Roughly three
  minutes of overlap — so "does not overlap either storm" would be false. What
  still holds: the flood continues for 19 minutes *after* that storm dies, and
  is absent entirely from vbgthykuj, so it is not driving the storms.

The analyzer's standing warning line — *"this flood precedes both committed hard
stops"* — is now misleading and should be reworded or dropped. It also counts
whole-log errors against the selected match and divides by the match duration:
it reported 116,556/min for a 6-minute match whose real peak was 58,398/min.
Both are bugs.

Everyone still runs an observer vehicle (`multi.ini`: `avobserv` / `bvobserv` /
`cvobserv`), so the flood is not going away on its own — but it is a log-size
problem, not a netcode one.

### 7. Version skew and the unused A/B mechanism

- KFK and Awildbison: proxy V4.91 (`7abc2b8053b7`), wrapper `V4.91-harvest`.
- Monkey and PiercingXX: proxy V4.92 (`709a4b5cdbcb`), wrapper `V4.92-arms`.
- **`overrides=` is empty in every bundle.** V4.92's whole reason for existing —
  per-session `bznet.env` A/B arms — has never once been exercised in the field.

### 8. Still crying wolf

`POKE DID NOT HOLD` fired on 13 matches in PiercingXX's Linux log and 6 in
Awildbison's, while the traces show the governor healthy at the next sample.
The V4.91 §2 rule narrowed it; the client-rejoin case in V4.93 §8 is still open
and is now the single noisiest false positive in the analyzer output.
---

## A5. 2026-08-10 — the cause found, reproduced, and fixed locally

> **The fix is not deployed.** It is written, tested on one machine, and handed
> to the mod author. Until it is published and the crew updates, the storms are
> live for everyone and this project plans accordingly. See
> [T12](#t12-file-the-mod-bug-upstream).

The day the storm stopped being a mystery. Nine controlled runs, a switch
nobody knew about, and two defects fixed at source. Full writeup:
[`resources/CAMERAPOD_STORM.md`](resources/CAMERAPOD_STORM.md).

### 1. `-netpktlog` — the game logs its own application layer

An undocumented command-line switch adds three line kinds to `BZLogger.txt`:

```
TX SRC 2 DST 1  Sent: Yes Packet: 7a75... Send Type: 1
RX SRC 1 DST 1            Packet: 0101...
Chat Message: Bandwidth = 39950, used rate = 120
```

plus the send scheduler and ping logging. `Send Type` is the channel, verified
by cross-reference rather than assumed: **every** type that appears in a `TRY`
retransmit is Send Type 1, and no Send Type 0 type ever has.

| | Send Type 0 | Send Type 1 |
|---|---|---|
| share | 95.1% of packets, **99.0% of bytes** | 4.9% / 1.0% |
| dominant type | `5f00`, ~220 B, several objects' position+velocity | `7a75`, one object identity |
| retransmitted? | **never** | all of them |

So a broad per-object position/velocity broadcast is real and is almost all the
bytes — unreliable, droppable, and *not* the storms. It also gave us
`Net::NextPositionPacketInterval 48` (20.8 Hz, constant over 10,672 samples,
matching a measured vehicle at 20.79/s) and the game's own RTT via
`PONG RECEIVED: NET DELAY FROM PING`.

### 2. The trigger: an armory item landing on a nav beacon

```
15:28:46  beacon created ......................... 2 packets, then silence
15:28:51  placed on a geyser, nothing else ....... still silent
15:29:13  armory item sent to it   <- command
          ...14 s of flight time...
15:29:27  26 packets   <- ignition
15:29:28  73 packets
```

It never stopped — 5,779 packets at 25.9/s for the rest of the match, against a
normal cost of **2 packets for a beacon's entire life**. The same run contains
its own control: placing the beacon and leaving it alone kept it silent.

### 3. Two defects, both fixed and measured

| | unpatched | + fix 1 | + fix 2 |
|---|---:|---:|---:|
| camerapod reliable packets | **5,779** | 669 | **14** |
| worst beacon | 5,779 @ 25.9/s | 656 @ 1.47/s | **2** |
| dropped before send | 4,321 (7.52%) | 30 (0.03%) | 25 (0.06%) |
| retransmits | 15,770 | 135 | **110** |

**Fix 1** — `SBPNavLogic.lua:260` snapped an incoming powerup to the beacon's
*exact* coordinates. Interpenetration, collision response, the beacon never
rests, and an object that never rests has its full state re-sent reliably every
frame. Land it 3 m to the side instead.

**Fix 2, and the more broadly useful finding** — `SetObjectiveName` **dirties an
object for replication even when the string is identical**. `UpdateNavInfo()`
rewrites each nav's name once a second with a live scrap count in it. Caching
the last name and only writing on a real change took one beacon from 656
packets to 2. This affects any script that refreshes a label on a timer.

**A third defect**, surfaced by the fix crashing: `ClearDeadNavs()` nils entries
inside `NavManager`, leaving holes, so `NavManager[x]` can be nil in
`UpdateNavInfo`. The original `SetObjectiveName` swallowed a nil handle
silently. `#` on a table with holes is undefined in Lua and can silently
truncate.

### 4. The amplifier, now quantified against the game's own RTT

The reliable retry timer is **~10 ms flat with no backoff** (measured from the
per-copy send clock at header offset 2), against an RTT the game reports as
**56–91 ms**. So every reliable message goes out **6–9×** before an ack can
return — always, in every session. That is the engine defect
[T1](#t1-sharedsend_dampenh--the-duplicate-suppressor) exists to blunt, and it
is unfixable from outside.

### 5. Method notes worth keeping

- **A patch that silently fails to load produces a clean result that means
  nothing.** An override placed in `addon/` was never read — `require()` does
  not search it — and the resulting quiet run had to be discarded. Every patch
  now announces itself.
- **`print()` does not reach BZLogger under Proton.** `DisplayMessage` does,
  arriving as `Chat Message:`.
- **Steam restores the workshop copy on re-sync** — three times during one
  afternoon. Re-check before every run.
- **Narrate the run in chat.** The analyzer segments on the tester's own
  callouts, which beats reconstructing from memory — the first runaway we
  caught was in a session nobody could remember.

---

## A4. Corrections log

Claims this project published and later had to withdraw. Kept so nobody
re-derives a retracted result, and as a standing argument for re-measuring
rather than copying forward.

| # | Claim | Status | What is true instead | Found by |
|---|---|---|---|---|
| 1 | V4.8: duplication/loss figures from the packet header | **withdrawn** | They were read from the ack field. Sequence is u32 BE at offset 10. `resources/BZ_P2P_HEADER.md` | V4.9 ground-truth pass |
| 2 | The inbound reorder buffer will help | **withdrawn** | The protocol's sequence counts messages, not datagrams — there is no per-datagram key to order by. Buffer stays off | V4.9 |
| 3 | `syncJoin = 1` explains the retransmit storm | **disproved** | All five sessions log `Sync: On`; `multi.ini` was only captured in two bundles | V4.9, from existing data |
| 4 | Player count is the storm trigger (V4.93 §2) | **disproved** | The 2026-08-08 storm was a **2-player** match: 100,476 datagrams at 4.06 copies | 2026-08-09 |
| 5 | The 08-03 storm is a structurally different "go-back-N" shape (V4.93 §2) | **retracted** | Re-scored at 3.83 copies in a 48 ms window — the same proactive-redundancy shape as 07-26 and 08-08 | 2026-08-09 |
| 6 | "One packet resent **4,336 times**" (V4.93 §2) | **not reproducible** | Maximum copies of any single `(peer, seq)` in that log is **13** | 2026-08-09 review |
| 7 | Camerapod shares 93 / 99 / 99 / 91% | **wrong** | 94.8 / 94.8 / 96.8 / 87.1% of all retransmitted datagrams. The original column mixed denominators | 2026-08-09 review |
| 8 | "Storm window equals beacon lifetime, to the minute" | **circular** | The only source for a beacon's lifetime is its own payload stream. Two of the four "deaths" are the abort button | 2026-08-09 review |
| 9 | vbgthykuj shows the governor breaking the storm at a floor of 8,150 | **wrong** | The match was **aborted** at 14:49:31.5 with the budget still descending (8,400 and falling). 8,150 is a post-abort proxy sample. Only Peppy and 08-03 support recovery-by-starvation | 2026-08-09 review |
| 10 | Peppy's governor floor was 19,050 | **wrong** | Minimum was **10,650**; 19,050 is the post-storm recovery plateau | 2026-08-09 review |
| 11 | Host/client governor traces are identical "byte-for-byte" | **overstated** | Host-*synced* but not identical — about half the samples differ, some by 2,300. Two clients sampling different phases of a −400/2 s walk | 2026-08-09 review |
| 12 | The crash log "stops mid-line" | **wrong** | The final line is complete, terminator and all. What is missing is the shutdown sequence | 2026-08-09 review |
| 13 | The `observer.mesh` flood does not overlap either storm | **wrong** | It overlaps the Peppy storm by ~3 minutes (14:57–14:59). The broader exoneration still holds | 2026-08-09 review |
| 14 | `UpdateNavInfo()` runs "hundreds of times a second" | **unsupported** | Object creation runs 0.4–3.4/s, so it is a 2–5× multiplier on the 1 Hz sweep | 2026-08-09 review |
| 15 | `omegaSpin = 1.0` causes the beacon storm | **falsified** | `apcamr349` had the identical ODF, was spinning, and sat at 1 Hz for twelve minutes. Every beacon's rotation matrix changes at every snapshot; the quiet ones still sync at 1 Hz | 2026-08-09 review |
| 16 | "The beacon's transform never settles" identifies a runaway | **discriminates nothing** | Quiet beacons' state bytes are unique on every message too. **Emission rate is the only signal** | 2026-08-09 review |
| 17 | Damper saves 60–75% of storm bytes | **best case only** | 46–75% depending on copies-per-message: 08-08 ~75%, vbgthykuj ~59%, Peppy ~46% | 2026-08-09 review |
| 18 | "BZ sends each reliable message several times up front, on principle — proactive redundancy" (`RETRANSMIT_STORM.md`, the headline finding of 2026-07-26) | **retracted** | It is a retransmission timeout fixed at **~10 ms with no backoff**, measured from the per-copy send clock. Against 40–80 ms WAN RTT it cannot do anything but multiply. Same observation, wrong cause | 2026-08-10 |
| 19 | Byte 0 bit 7 = "this datagram is a retransmit" (`BZ_P2P_HEADER.md`, `reorder_is_retransmit()`) | **wrong** | `0x80` = **reliable**, `0x40` = final. Byte 0 is `0xC0` on 100% of 100,820 TRY datagrams. Nothing in the header marks a resend | 2026-08-10 |
| 20 | Header offsets 2–4 = version (`00 00 01`), 5–9 = u40 clock | **wrong** | One **u64 epoch-ms send clock at offset 2**, stamped fresh per copy. The "version" bytes were its high bytes. Validated on captures from 07-26, 08-08 and 08-09 | 2026-08-10 |
| 21 | The storm might be the mod's Lua `Send`/`Receive` | **disproved** | Lua messages carry `0x23` `'#'` + type char and are identifiable; they *fell* during storms (2,859 datagrams across a quiet session vs 599 across eleven storm minutes) | 2026-08-10 |
| 22 | A "selected beacon slaves its yaw to the driver" explains the 33 Hz vs 1 Hz split | **withdrawn** | Rested on satellite view, which needs a CommSat that was never built in these sessions. Selection measured at 1.11/s against a 1.00/s baseline — no effect | 2026-08-10 |
| 23 | Player movement is the trigger | **disproved** | Identical 2 packets whether stationary or driving, on both stock and modded maps | 2026-08-10 |
| 24 | An `addon/` override tests the fix | **wrong, and it produced a false pass** | `require()` never searches `addon/`. The run looked clean and proved nothing. Patches must announce themselves | 2026-08-10 |
| 25 | `print()` from Lua is visible in BZLogger | **wrong** | It does not reach BZLogger under Proton. `DisplayMessage` does, as `Chat Message:` | 2026-08-10 |
| 26 | Any object above 5 Hz reliable is a runaway | **too loose** | Rate alone flagged a dozen objects spawning in one second as 24/s. The analyzer now requires ≥20 s and ≥100 packets | 2026-08-10 |

**The pattern worth noticing:** items 7–17 all came from one adversarial
re-derivation pass over data that had already been "analysed". Nine of eleven
were arithmetic or attribution errors in supporting detail, not in the headline
finding — which survived. Budget a review pass before publishing, and make it
someone's job to try to break the result.

---
## Open decisions

These need a person to decide, not an agent to implement. Do not action them
without asking.

### D1. The reorder buffer: keep it or delete it?

**The case for deleting.** V4.9 established that this protocol has no
per-datagram sequence to order by (`resources/BZ_P2P_HEADER.md`) — the sequence
counts *messages*. The buffer's founding premise is therefore false. It is
~760 lines in `shared/reorder_core.h` plus integration in both proxies plus
`tests/reorder_test.cpp`, all carried for a feature that is off by default and
has no validated purpose. V4.9 had to fix a bug in it (Defect E) that would
have discarded 98% of inbound traffic had anyone enabled it — dead code that
still costs review attention and can still be wrong.

**The case for keeping.** `reorder_core.h` is where
`reorder_is_reliable()` (renamed from `reorder_is_retransmit()`, see A4 #19),
`reorder_seq_from_payload()`, `reorder_send_time_ms()` and the offset
constants live, and those are load-bearing —
[T1](#t1-sharedsend_dampenh--the-duplicate-suppressor) depends on them and so
does the buffer-logging path. Deleting the *buffer* must not delete the
*header's* packet-parsing half.

**If deleted**, split the parsing primitives into `shared/bz_packet.h` first,
in a separate commit with tests, then remove the buffer in a second. Do not do
both at once.

**Recommendation:** decide after [T1](#t1-sharedsend_dampenh--the-duplicate-suppressor)
ships, since T1 will show how much of the header is actually shared.

### D2. FEC (`PATCH_OPTIONS_RESEARCH.md` §E)

Was gated on "the sequence field settled". It now is — and the finding moves
the goalposts rather than opening the gate:

- parity coding needs a per-datagram sequence to reconstruct against, and there
  is none, so any scheme would have to carry its own;
- the stream already carries 1.9–4.1 copies of every reliable message during a
  storm, so this would be redundancy on top of redundancy — and
  [T1](#t1-sharedsend_dampenh--the-duplicate-suppressor) exists to *remove*
  redundancy.

Still gated on a controlled test that unpatched receivers provably discard a
parity packet type harmlessly. **Leaning no.** The V4.94 finding makes FEC look
like the wrong direction entirely: the problem is a sender emitting too much,
not a receiver missing too much.

### D3. Do we correct the governor from outside, or only observe?

`shared/gov_trace.h` says in its header *"This observes; it does not correct."*
The proxy already writes the send-rate global at cold start and asserts `[Net]`
every 100 ms, so multiplicative decrease from outside is mechanically possible:
on a storm signature, halve the global.

[T3](#t3-ship-the-ramp-knobs-as-defaults) approximates this with knobs and is
much less invasive. **Do not build active correction until T3 has been measured
in the field ([F2](#f2-ramp-knob-ab)).** If the knobs are enough, this stays
unbuilt.

---

## Smaller things left over

Not worth a task spec yet. Promote one to a `T` number if it starts costing
time.

- **Windows has no `GetProcAddress` hook.** The Linux proxy has one; V4.9 gave
  Windows a retry loop over the EXE's IAT, which closes the common case, but a
  dynamically resolved winsock import still bypasses the Windows proxy
  entirely. Nothing in the field has hit this — it is a known hole, not a known
  failure.
- **`session end:` stats double-fire on both Linux boxes** (A1.8). Harmless but
  it doubles the counters for anything that greps naively. Fix it while doing
  [T2](#t2-wire-the-damper-into-both-proxies) rather than as its own change.
- **The wrapper does not bundle `capture_verify.txt`** (see
  [F4](#f4-buffer-capture-re-run)), so a partial buffer capture cannot be
  detected from the bundle alone.
- **Two old bundles are named `unknown-host`.** Fixed for future captures; the
  existing two keep the name. Nothing to do unless they get re-analysed.

Promoted out of this list into tasks:
[T11](#t11-analyzer-stop-guessing-which-proxy-session) (proxy session pairing)
and [T14](#t14-delete-or-regenerate-the-bogus-reorder-resources) (the
wrong-offset reorder resources).

---

## Done in V4.9 — recorded so nobody re-derives it

| V4.8 item | Outcome |
|---|---|
| 1. Sequence field unsettled | **Settled exactly**, 100% match on 65,860 samples. Two published conclusions withdrawn. `resources/BZ_P2P_HEADER.md` |
| 2. `BZ_GOV_START` didn't hold | Read-back instrumentation built; one verdict per match in the proxy log |
| 3. `analyze_drops.py` blind to host logs | All four bugs fixed, plus 6 new metrics |
| 4. Teardown crash / host role | Crash-capture preflight on both platforms; host role is a controlled variable |
| 5. Retransmit storm / `syncJoin` | Analysed; `syncJoin` hypothesis **disproved** from existing data |
| 6. IOCP path | Fixed in place: wrap bug, lock-order race, test coverage |
| 7. Non-atomic log writes | One write per line under a lock; teardown races closed |
| 8. Silent truncation | `WSAEMSGSIZE` + `MSG_PARTIAL` |
| 9. Ring size / `BZ_BUFFER_LOG_PEER` / hostname | All three fixed; captures now self-verify |
| 10+11. Windows unmeasured and behind | Six parity gaps closed; prebuilts refreshed |
| 12. CI | Exists, including the 32-bit test run and a record-layout check |
| 13. Version stamp | `BZ_BUILD_ID` in both binaries |
| 14. Installer hygiene | Integrity check, ref validation, uninstallers for both platforms |
| 15. Discord upload | `upload/bz_wrap.sh` / `.ps1`, tested end to end |
| P5 housekeeping | `new logs/` folded in, bundles deduplicated (38 MB → 2.4 MB), retention policy written, empty sessions labelled |
| `Source IP doesn't match any known player` | **Investigated and closed** — it is the connect handshake's multi-address probe, benign by construction |
