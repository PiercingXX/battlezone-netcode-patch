# BZ_SEND_DUP live test series — 2026-07-03

## VERDICT (V4.4): duplication is deprecated

Across ~10 games (the V4.2 naive-dup series below, plus a clean V4.3 A/B on the
same map/opponent), outbound duplication **does not help this game and hurts busy
uplinks.** The decisive V4.3 A/B (PiercingXX vs KFK, both on V4.3, map `ulntrogn`):

| your outbound → KFK | dup OFF | dup ON |
|---|---|---|
| real stale drops | **3.6/min** | **47.9/min** (sustained flat all game) |

Why it still hurt even with V4.3's delay + cap: BZ sends ~30 packets/sec and the
`BZ_DUP_MAX_PPS` cap defaults to 40, so the cap almost never engages — duplication
still ~doubles the packet rate, which is the exact thing that overloads a filling
queue. The 25 ms delay spreads copies in time but doesn't reduce their count.

Separately, KFK's uplink blew out catastrophically in *both* dup-off and dup-on
games (12k–18k drops in 1–2 minute bursts, autokick in one), one-directionally —
his send died while PiercingXX's send to him stayed clean. That is a saturated /
unstable uplink on his end (fix: wired ethernet, router QoS/SQM, kill background
uploads), which no receiver-side patch can address.

**Action taken:** `BZ_SEND_DUP` removed from all recommended launch options and
installer prompts (still present, opt-in, off). The validated, always-on core is
reorder + bigger buffers + DSCP marking. See Version History V4.4 in the main
README.

---

Seven multiplayer games in one afternoon, designed to measure what outbound packet
duplication (`BZ_SEND_DUP=1`) actually does to delivery quality. Players:

| Player | Platform | Receive path |
|---|---|---|
| PiercingXX | Arch Linux / Proton, dsound proxy | reorder buffer active (100 ms adaptive window) |
| KingFurykiller (KFK) | Windows, winmm proxy | vanilla (reorder inert on real Windows — IOCP bypass) |
| Monkey | Windows, winmm proxy | vanilla |
| Bajablastbison (Bison) | Linux Snap / Proton | patched (proxy state unverified) |

All games on workshop 3406347034 maps. Raw logs in this directory.

## Metric

BZRNet logs every packet it rejects as
`Dropping Packet Type 0 For Client <steamid> (Packet #R received, #E expected)`.
Raw counts are meaningless once duplication is involved; every number below is
derived per **sender→receiver link** with this classification:

- **echo** (`E−R == 1`): re-delivery of the packet just accepted — the discard of an
  intentional duplicate. Harmless by design; counts ~300/min per link when the
  sender duplicates, ~8/min baseline noise without dup (engine resends). Echoes
  from a player's *own* Steam ID are dup applied to the game's loopback
  self-connection.
- **real** (`E−R ≥ 2`): a genuinely stale/out-of-order arrival — the number that
  matters. When the sender was duplicating, each stale packet is logged twice
  (original + copy), so those counts are halved ("unique").

## Games and results

### 1v1s, PiercingXX vs KFK, map `ulllowar`

| Game | Config | KFK→PiercingXX real drops |
|---|---|---|
| 1 (16:03) | both dup off | *logs lost (overwritten at relaunch)* |
| 2 (16:14, ~6 min) | PiercingXX dup ON, KFK off | **24 (~4/min)** ← baseline for this direction |
| 3 (16:24, ~7.5 min) | KFK dup ON | **~210 unique (~30/min)** + 518-drop burst at connect |
| 4 (16:35, ~8.5 min) | both dup ON | **~84 unique (~10.5/min)** + 558-drop burst at connect |

KFK's duplication degraded his own stream **2.5–8x**, sustained through the whole
match, and both dup-on games opened with a massive connect-phase drop burst absent
in the dup-off configuration.

### 2v2s, all four players (full mesh: 6 links, 12 directions)

Real stale drops per minute, per link:

**Game 5 — all dup ON, `ulsfelix`, ~11 min** (Bison's receive side unavailable)

| sender ↓ rx → | PiercingXX | KFK | Monkey |
|---|---|---|---|
| KFK | 63 | — | 72 |
| PiercingXX | — | 32 | 48 |
| Monkey | 44 | 29 | — |
| Bison | 26 | 17 | 34 |

**Game 6 — all dup ON, `uesrtst1`, ~12 min** (Bison rx unavailable)

| sender ↓ rx → | PiercingXX | KFK | Monkey |
|---|---|---|---|
| KFK | 29 | — | 40 |
| PiercingXX | — | 24 | 53 |
| Monkey | 48 | 27 | — |
| Bison | 33 | 28 | 62 |

**Game 7 — all dup OFF (control), `uomadpro`, ~5.5 min**

| sender ↓ rx → | PiercingXX | KFK | Monkey |
|---|---|---|---|
| KFK | 22 | — | 291 |
| PiercingXX | — | 125 | 7 |
| Monkey | 10 | 150 | — |
| Bison | 6 | 139 | 17 |

Game 7 caveat: every link *into* KFK ran 125–150/min uniformly, and KFK↔Monkey was
catastrophic both ways — a KFK-local event (suspected: Steam resumed a queued
download after the forced restart to apply `setx`). All dups were verified off
(echo audit). KFK's cells are contaminated; the PiercingXX receive column is the
valid control.

## Conclusions

1. **Always-on back-to-back duplication is a net negative in mesh play.** Turning
   dup off cut every clean link 2–5x (PiercingXX rx: 63/44/26 & 29/48/33 → 22/10/6).
   The 1v1 A/B (games 2→3) showed the same direction at smaller scale.
2. **The mechanism is not raw bandwidth.** Game traffic is governor-limited to
   ~4–10 KB/s; doubling that cannot saturate broadband. What doubles is
   **packets per second**, at the exact moment queues are forming — classic
   bufferbloat/airtime aggravation — and a copy sent in the same burst as the
   original dies in the same burst (stale ladders with gaps of 30–140 packets in
   the logs = wholesale delayed chunks, far beyond any reorder window).
3. **The 100 ms reorder window is not the binding constraint under congestion.**
   PiercingXX (reorder on) and Monkey (vanilla) received KFK's game-5 stream at
   63 vs 72/min — when arrivals are hundreds of ms late, the window is a rounding
   error. Reordering fixes *reordering*; it cannot fix queueing delay.
4. **Whoever has the least connection headroom becomes the hotspot** (KFK in
   games 3–5, Monkey in game 6), and per-game local conditions (background
   downloads) can dwarf the test variable entirely.
5. Baselines: 1v1 link ≈ 4 real drops/min; clean 4-player mesh link ≈ 6–22/min
   even with no duplication anywhere.

## Operational lessons (hard-won)

- `BZLogger.txt` is **overwritten on every game launch**. Copy it out after every
  game, before relaunching. Game 1 and several receive-sides were lost to this.
- Chat-pasted logs truncate (5–46 MB files). Zipped file attachments only.
- The per-launch `send_dup:`/`winmm_proxy` log line + the echo audit make player
  config self-verifying after the fact — keep both.

## Research: what the wider world does about this

- **Time-shifted redundancy (RFC 2198 / WebRTC RED):** redundant audio data is sent
  *piggybacked on a later packet*, not back-to-back — precisely so one loss/queue
  event cannot claim both copies. ([RFC 2198](https://datatracker.ietf.org/doc/html/rfc2198),
  [webrtcHacks on RED](https://webrtchacks.com/red-improving-audio-quality-with-redundancy/),
  [getstream.io RED overview](https://getstream.io/resources/projects/webrtc/advanced/red/))
- **FEC instead of duplication (UDPspeeder):** Reed-Solomon parity over groups of
  packets (`-f20:10` = +50% overhead recovers any 10 of 30) with an `-i` interleave
  option against burst loss; recovers most loss at a fraction of 2x cost.
  ([UDPspeeder](https://github.com/wangyu-/UDPspeeder),
  [FEC parameters](https://github.com/wangyu-/UDPspeeder/wiki/Fine-grained-FEC-Parameters))
  General FEC-vs-latency tradeoffs: [F5 FEC concepts](https://techdocs.f5.com/kb/en-us/products/big-ip-aam/manuals/product/bigip-acceleration-concepts-13-0-0/27.html),
  [Maister's FEC streaming experiments](https://themaister.net/blog/2024/02/12/real-time-video-streaming-experiments-with-forward-error-correction/)
- **Bufferbloat / airtime:** on WiFi and bloated home links, queueing delay under
  load — not bandwidth — is the lag mechanism; more packets per second means more
  queue and airtime contention. ([bufferbloat.net FAQ](https://www.bufferbloat.net/projects/bloat/wiki/Bufferbloat_FAQs/),
  [Ending the Anomaly (FQ-CoDel/airtime fairness paper)](https://arxiv.org/pdf/1703.00064))

## Changes shipped in response (v4.3, both proxies)

Identical env vars on Windows (`winmm.dll`) and Proton (`dsound.dll`) except
where noted. All built + compile-verified; the dup/DSCP/setsockopt changes are
safe against unpatched receivers.

**Dup semantics** (sender-side, addresses the core finding):

1. **Loopback skip** — the game's P2P self-connection is never duplicated
   (removes pointless traffic and the self-echo metric pollution).
2. **Delayed duplicate** — `BZ_DUP_DELAY_MS` (default 25 ms): copies are queued and
   transmitted by a pacer thread, RFC 2198-style, so a queue spike can't kill both
   copies and instantaneous burst PPS is not doubled. `0` restores the old
   back-to-back behaviour.
3. **Duplicate budget** — `BZ_DUP_MAX_PPS` (default 40/s): under load, bulk bursts
   shed their duplicates while low-rate control traffic keeps redundancy.

**QoS** (targets the queueing-delay mechanism directly):

4. **DSCP marking** — `BZ_DSCP` (default 46 = EF): marks the P2P socket via IP_TOS
   so WMM (WiFi voice queue) and SQM/fq_codel routers serve game packets ahead of
   bulk traffic. Real effect under Proton; no-op on stock Windows (needs qWAVE or
   a router rule). `0` disables.

**Correctness / parity:**

5. **Windows `setsockopt` hook** — the winmm proxy now intercepts the game's own
   `setsockopt(SO_SNDBUF, 32768)` and re-forces 512 KB, so the enlarged send
   buffer survives on real Windows (previously only the Proton proxy did this).

**Diagnostics / tooling:**

6. **Governor scanner** — `BZ_GOV_SCAN=1` (default off, Proton proxy): 15 s after
   launch, scans the DRM-decrypted `.text` for the 4000 B/s start constant and logs
   candidate sites. Read-only. Unlocks the runtime governor patch next iteration.
7. **`tools/analyze_drops.py`** — the per-link echo/real classification used
   throughout this report, as a committed CLI (auto-detects sender-dup state and
   stale-session files; reproduces every table above).

Future candidate (needs both ends patched + wire-format safety testing against
unpatched peers): proxy-level XOR/Reed-Solomon FEC with receive-side
reconstruction injected through the existing reorder buffer — UDPspeeder-style
recovery at ~10–30% overhead instead of 100%.

## Next test session (v4.3 is built + deployed to PiercingXX's install)

Analyze every game with `tools/analyze_drops.py <logs> --launch <lobby>`.

1. **both-off 1v1** on `ulllowar` — bank the lost game-1 baseline first (control).
2. **1v1 dup A/B with v4.3 defaults** (delay 25 / cap 40 / loopback-skip): does
   delayed+budgeted dup still degrade KFK's outbound, or is it now neutral-to-
   positive vs game 2's 4/min? This is the headline question.
3. **DSCP alone** (dup off, `BZ_DSCP=46` — already the default): compare a 1v1
   against baseline. Isolates whether packet prioritization alone helps. Ask
   PiercingXX to also enable SQM/fq_codel on the router for the real effect.
4. **`BZ_GOV_SCAN=1` for one launch** — capture the governor candidate addresses
   from `dsound_proxy.log` so the runtime governor-start patch can be built.
5. If (2) looks clean, an **all-dup 2v2** to compare against games 5/6.
6. Windows testers: fresh installer (winmm hash updated) picks up the setsockopt
   fix — confirm `setsockopt IAT patched OK` in `winmm_proxy.log`.
7. Bison: locate the real `BZLogger.txt` (newest-modified, inside the live game
   install) — his receive side is still a blind spot. `analyze_drops.py` will flag
   a stale file (wrong lobby/map in the header).
