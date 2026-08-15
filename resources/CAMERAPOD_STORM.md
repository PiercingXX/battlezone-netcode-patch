# The reliable-channel retransmit storm

**Game:** Battlezone 98 Redux 2.2.301 (Rebellion) · **Measured:** 2026-08-03 to
08-10 · **Status:** cause found, fix written and verified locally — **not yet
released upstream**

---

## TL;DR

**Sending an armory item to a nav beacon makes that beacon flood the network
for the rest of the match.**

A nav beacon nobody touches costs **2 network messages for its entire life**,
then goes silent. Send one armory item to it and it starts emitting **20–50
messages a second, on the reliable channel, and never stops.** One beacon doing
this consumed up to 98% of a player's outbound bandwidth and made matches
unplayable.

**Why.** `SBPNavLogic.lua:260` in Steam Workshop mod 3406347034 teleports the
incoming item to the beacon's *exact* coordinates:

```lua
SetPosition(p, GetPosition(NavManager[x]))
```

Two solids at identical coordinates interpenetrate. Collision response keeps
pushing them apart, so the beacon never comes to rest — and an object that never
rests has its full state re-sent reliably every frame, forever.

**A second, separate defect** was hiding underneath it. `UpdateNavInfo()`
rewrites each nav's name once a second — and **`SetObjectiveName` dirties an
object for replication even when the string is identical**, so a redundant
write still re-sends the object's full state reliably.

**Both fixed, both measured** on the same map:

| | unpatched | + fix 1 | + fix 2 |
|---|---:|---:|---:|
| camerapod reliable packets | **5,779** | 669 | **14** |
| worst single beacon | 5,779 @ 25.9/s | 656 @ 1.47/s | **2** |
| packets the engine had to drop | 4,321 (**7.52%**) | 30 (0.03%) | 25 (0.06%) |
| retransmits | **15,770** | 135 | **110** |

With both applied, seven beacons emitted 2 packets each — the floor — through
several armory deliveries. Patch and apply/revert script:
[`navfix/`](navfix/).

> **Not yet in anyone's game.** These fixes are verified on one machine and
> have been passed to the mod author; they are not in a published version of
> the mod. Anyone playing today still has both defects.

**Amplifier, not cause.** BZRNet retries an unacknowledged reliable message
every **~10 ms flat with no backoff**, against a measured 62–91 ms round trip.
So every reliable message goes out **6–9 times** before an acknowledgement can
physically return. That is always true; it only becomes visible when something
generates reliable traffic at speed.

**Not the cause**, each ruled out by measurement: player count · `omegaSpin` ·
Lua `Send`/`Receive` · player movement · beacon selection · the `observer.mesh`
flood · scrap deletion.

**A third defect, found while testing:** `ClearDeadNavs()` nils entries inside
`NavManager`, leaving holes, so `NavManager[x]` can be nil in `UpdateNavInfo`.
The original `SetObjectiveName` swallowed a nil handle silently. `#` on a table
with holes is undefined in Lua and can silently truncate.

---

Everything below is the detail: what is on the wire, how each conclusion was
reached, and what was retracted along the way. It supersedes
[`RETRANSMIT_STORM.md`](RETRANSMIT_STORM.md), whose central conclusion is
retracted below.

## The shape of the problem

Four multiplayer sessions were captured at packet level on every machine. In
each, the game became unplayable while **66–98% of everything the affected
player sent was retransmission**, and 87–97% of that retransmission carried a
single game object.

Two things combine.

**1. The reliable-channel retry timer is set below the round-trip time.**
BZRNet retransmits an unacknowledged reliable message after **~10 ms, flat, with
no backoff**. These peers are cross-country US, RTT 40–80 ms. So every reliable
message goes out **4–8 times before an acknowledgement can physically return** —
always, in every session, storm or not.

**2. Something generates reliable object-state for one `camerapod` at ~30/s.**
Normally reliable traffic is a trickle and 4× a trickle is nothing. At 30
messages/second for one object, 4–8× saturates the uplink, the send window
stops draining, acks stall at ~5%, and go-back-N piles on top.

The retry timer is the amplifier and is always present. The object is the
trigger. **Neither alone is sufficient**, which is why the same players go
whole matches without incident.

---

## What is measured

### The storms

| date | match | players | sender | retransmitted | carrying a `camerapod` |
|---|---|---|---|---:|---:|
| 08-03 | control (ulllowar) | 5 | host | 210,673 dg / 17.7 MB | 94.8% |
| 08-08 | host (ulsfelix) | **2** | host | 100,476 dg / 7.7 MB | 94.8% |
| 08-09 | vbgthykuj (uesrtst1) | 6 | host | 122,918 dg / 9.4 MB | 96.8% |
| 08-09 | Peppy (uesrtst1) | 6 | **client** | 50,170 dg / 3.7 MB | 87.1% |

Shares are of *all* retransmitted datagrams; most of the remainder is short
control traffic carrying no object identity at all.

### Retransmission against total outbound

Retransmit bytes come from BZLogger's own burst totals; total outbound comes
from the game's independent `Actual Used` counter. Two unrelated sources.

| match | retransmitted | total sent | share |
|---|---:|---:|---:|
| 08-09 `comeback` — **quiet, same host, same evening** | 0.11 MB @ 0.1 kB/s | 52.7 MB @ **53.6 kB/s** | **0.2%** |
| 08-08 `host` — storm | 7.52 MB @ 11.1 kB/s | 11.3 MB @ 16.8 kB/s | **66%** |
| 08-09 `vbgthykuj` — storm | 9.43 MB @ 34.1 kB/s | 9.64 MB @ 34.9 kB/s | **98%** |

The quiet match is the control that matters: the same host pushed **more**
absolute throughput (53.6 kB/s) than either storm and was fine. Raw volume is
not the problem. `vbgthykuj` was abandoned via the abort button six minutes in,
with the send budget still descending.

### The retry cadence — the mechanism

The header carries the sender's wall clock at offset 2, stamped **fresh on
every copy**, so retry timing is directly measurable
(see [`BZ_P2P_HEADER.md`](BZ_P2P_HEADER.md)). Gaps between successive copies of
the same `(peer, sequence)`:

| gap | storm (n=21,638) | quiet session |
|---|---:|---:|
| copy 1 → 2 | **12 ms** | 34 ms |
| copy 2 → 3 | 11 ms | 14 ms |
| copy 3 → 4 | 10 ms | 12 ms |
| copy 4 → 5 | 10 ms | 12 ms |
| copy 5 → 6 | **9 ms** | 7 ms |

Flat, and if anything slightly *faster* with each retry. There is no backoff. A
fixed ~10 ms timeout against a 40–80 ms RTT cannot do anything but multiply.

### The consequence: the window stops draining

`vbgthykuj`, host, per peer, across the storm:

```
peer               seq range          ack range      ack advance
206.168.27.165     542 -> 12,101      145 -> 684         4.7%
69.138.91.228      513 -> 11,816      465 -> 1,032       5.0%
216.255.2.226      514 -> 11,801      152 -> 717         5.0%
108.207.108.119    516 -> 11,694      125 -> 671         4.9%
76.82.183.105      513 -> 11,587      129 -> 690         5.1%
```

Sequence runs ~11,500 forward while acks advance ~550 — **4.7–5.1% on all five
peers simultaneously**. That uniformity rules out a bad link; it is one
sender's window failing to drain.

### It is one object instance, not the class

Same map, same match, same owner, twelve minutes side by side:

| beacon | messages | lifetime | rate |
|---|---:|---:|---:|
| `apcamr342` (08-08) | 88 | 110 s | **0.8 Hz** |
| `apcamr349` (08-08) | 739 | 716 s | **1.0 Hz** |
| `apcamr585` (08-09) | 5,154 | 259 s | **19.9 Hz** |
| `apcamr346` (08-08) | 22,438 | 675 s | **33.2 Hz** |
| `apcamr236` (08-09) | 10,919 | 254 s | **43.0 Hz** |
| `spcamr1127` (08-03) | 14,780 | 289 s | **51.1 Hz** |

`apcamr346` never calmed down once in twelve minutes; `apcamr349` beside it
never sped up. And vehicles — dozens of them, actually manoeuvring — are
**0.2%** of this stream in every storm. Whatever this is, it is not the ordinary
per-object update rate.

---

## The channels

The game binary names five send paths and one receive path:

```
BZRNet P2P BAS Sent Packet (%u,%u) to %s: %s
BZRNet P2P CON Sent Packet (%u,%u) to %s: %s
BZRNet P2P REL Sent Packet (%u,%u) to %s: %s      reliable
BZRNet P2P TRY Sent Packet (%u,%u) to %s: %s      retry of an unacked reliable
BZRNet P2P UNR Sent Packet (%u,%u) to %s: %s      unreliable
BZRNet P2P Received Packet (%u,%u) expecting (%u,%u) from %s
           sent at %llu, received at %llu, diff %lld: %s
```

**Everything in the storm captures is `TRY`** — the retry path, which by
construction only carries messages that were sent `REL`. Per-tick position and
rotation for ordinary objects belongs on `UNR`, is never retransmitted, and
does not appear in those captures at all. The two are easy to conflate and are
not the same traffic.

### Confirmed directly, 2026-08-10

The game ships an undocumented `-netpktlog` switch that logs the application
layer with its channel and send decision — see
[`../docs/PACKET_LOG_TEST.md`](../docs/PACKET_LOG_TEST.md):

```
TX SRC  2 DST  1  Sent: Yes Packet: 7a7501000c0000...  Send Type: 1
TX SRC  1 DST  1  Sent: Yes Packet: 5f003433194106...  Send Type: 0
```

One 9-minute two-player session settles the channel question outright:

| | Send Type 0 | Send Type 1 |
|---|---|---|
| share of packets | **95.1%** | 4.9% |
| share of **bytes** | **99.0%** | 1.0% |
| dominant type | `5f00`, ~220 B | `7a75`, 17–60 B |
| payload | several objects' position and velocity per packet | one object identity, e.g. `apcamr230_camerapod` |
| appears in `TRY`? | **never** | **every retransmitted type is one of these** |

So **Send Type 0 = unreliable, Send Type 1 = reliable**, verified by
cross-reference rather than assumed. Three consequences:

1. **A broad per-object position/velocity broadcast is real and is 99% of the
   bytes.** Anyone describing the engine that way is right, and it is not the
   storm — it is unreliable and never retransmitted.
2. **`camerapod` state is on the *reliable* channel**, unlike vehicle position.
   The two are not treated the same, which is the structural asymmetry the
   whole investigation was circling.
3. **A high rate is only a problem on the reliable channel.** In that same
   session `avremp` ran at **20.9/s unreliable** and was completely fine, while
   a settled `apcamr230_camerapod` sat at **0.73/s reliable**. The storms are
   the same object class at 20–51/s *reliable*.

The switch also exposes `Sent: Yes/No` — the engine's own prioritise-and-drop
(15 of 27,187 in that session, 14 of them reliable) — and its per-tick
bandwidth accounting.

That split has ancestry. BZ98R descends from Battlezone (1998), which used
Activision's Anet library — released under the LGPL, source public. Anet's own
packet annotator documents BZ1's type `_` as *"used by Battlezone for periodic
unreliable status broadcast."*

**BZ1's replication model is documented first-hand**, by BZ2 programmer Nathan
Mates in his `.plan` of
[1999-01-28](https://matesfamily.org/cgi-bin/plan01_1999.html) (retrieved and
verified 2026-08-10):

> "BZ1's network model (which I had nothing to do with — I joined the BZ2 team
> after BZ1 shipped) **sent positions, velocities, etc of all objects in the
> world**, and in a big firefight (lots of ordinance going off), it had to
> **prioritize packets, and could drop some lesser-priority packets in order to
> keep bandwidth reasonable.**"

So a broad per-object position/velocity broadcast is the documented BZ1 design,
and it is **prioritised and droppable under bandwidth pressure** rather than
unconditional. That is the `UNR` stream. It is not what these captures contain,
and the two should not be confused — but anyone describing BZ98R as "sending
positions and velocities for all objects" is describing something real.

Caveats worth stating whenever this is quoted: Mates explicitly disclaims
direct knowledge, and he is describing BZ1 retail in 1999 — before Ken Miller's
1.5 rework and long before 2.2.301 replaced the transport with BZRNet.

> **Do not import Battlezone II / Combat Commander reasoning here.** The same
> `.plan` settles the lineage: *"For BZ2, in talking with the lead programmers
> and producer, **all of the BZ1 network source code was scrapped. Deleted,
> removed, it's gone.**"* and *"BZ1 used Activenet… BZ2 replaces that with MS's
> DirectPlay."* Zero netcode inheritance in either direction. BZ2/BZCC's
> lockstep move packets, shared per-tick RNG seed and "Multiworld" visual
> resync describe a different engine — `SetLocal`/`IsLocal`/`IsRemote` appear
> **zero times** in BZCC's scripting API, and `Multiworld` zero times in
> BZ98R's. BZ98R uses per-object ownership with per-player state streams; its
> official docs describe `AddPlayer` as *"called when a player starts sending
> state updates."*

### Body format, offset 18 onward

Public reverse engineering stops at offset 18 — the game-level payload is
documented nowhere. What we can decode of it:

| bytes 18–19 | meaning |
|---|---|
| `0x23` `'#'` + one char | a Lua `Send(to, "<char>", …)` script message |
| `0x7a 0x75` `'zu'` + dpid + `0x00` | engine object state, body ends with the object identity string |

The Lua form is confirmed by decoding a live packet as `#n` + `"ready"`,
matching `Send(nil, "n", "ready", team)` in the mod's own source. The dpid byte
matches the game's 8-bit `myNetID`.

**Script messages are not the storm.** Type mix on the same host:

| type | quiet session | storm 08-08 | storm 08-09 |
|---|---:|---:|---:|
| `zu` engine object state | 45.4% | **98.7%** | **99.2%** |
| `#n` Lua `Send(…,"n",…)` | 39.3% | 0.0% | — |
| `#l` Lua `Send(…,"l",…)` | 5.7% | 0.6% | 0.6% |

In absolute counts the mod's script traffic *fell* during the storms — 2,859
datagrams across a whole quiet session versus 599 across eleven storm minutes —
while `zu` rose from 2,882 for an entire session to 98,442 in eleven minutes.
Worth noting separately that script messages are ~45% of reliable retransmits
in a *normal* session, which is not a problem but is not free either.

---

## The fix

### 1. Suppress sub-RTT duplicate copies at the socket — the one we can ship

A DLL proxy cannot change the engine's retry timer. It can decline to put the
extra copies on the wire.

Keep a small per-`(peer, sequence)` table in the `WSASendTo`/`sendto` hook. The
first transmission of a sequence always goes. A repeat of the same sequence
inside a suppression window is dropped; after the window elapses it goes and the
window doubles.

**Set the first window from measured RTT, not from a constant.** This is the
whole point: the engine is retrying faster than an ack can return, so the
correct window is "long enough that an ack had a fair chance". The proxy can
measure RTT itself — it sees the outbound sequence and the inbound ack that
covers it — and the game's own `Received Packet … diff %lld` line gives a second
source. Suggested `window = clamp(1.2 × RTT_ewma, 60 ms, 400 ms)`, then
doubling to a ceiling.

Properties that make this safe:

- It **never drops a distinct message**, only a redundant copy of one already
  sent. Reliable delivery is unaffected.
- It is **fail-safe**: suppress wrongly and the engine's own retry timer fires
  again a few milliseconds later.
- Datagrams shorter than 18 bytes, and any datagram without the reliable flag,
  are never touched — that is the control and ping traffic the host's auto-kick
  measures latency on.

Expected saving is bounded by copies-per-message and is **not uniform**: 08-08
(4.06 copies) and 08-03 (3.83) give ~75% and ~74%; `vbgthykuj` (2.47) caps at
~59%; `Peppy` (1.87) at ~46%. Quote the range.

Implementation is specified in [`../todo.md`](../todo.md) §T1/§T2.

### 2. Stop teleporting powerups into beacons — the actual cure

**This is the fix, and it is a few lines of Lua.** Replace
`SBPNavLogic.lua:260` so the item lands *beside* the beacon rather than inside
it, and so an item already in place is not re-teleported:

```lua
-- before
                            SetPosition(p, GetPosition(NavManager[x]))
```

```lua
-- after
                            -- Snap the item just above the beacon rather than
                            -- inside it.  Identical coordinates make the two
                            -- solids interpenetrate; the collision response
                            -- then never lets the beacon come to rest, and a
                            -- beacon that never rests has its full state
                            -- re-sent reliably every frame for the rest of the
                            -- match.  Measured: 2 packets before an armory
                            -- delivery, 5,779 after.
                            local navPos = GetPosition(NavManager[x])
                            local snapPos = navPos + SetVector(0, 2.0, 0)
                            if Distance2D(GetPosition(p), snapPos) > 0.5
                            or math.abs(GetPosition(p).y - snapPos.y) > 0.5 then
                                SetPosition(p, snapPos)
                                SetVelocity(p, SetVector(0, 0, 0))
                            end
```

Tune the `2.0` to whatever clears the beacon's collision hull; the point is only
that it must not be zero. Syntax-checked under Lua 5.4, using only idioms
already in the module (vector addition as at `SBPNavLogic.lua:454`).

Worth pairing with a guard that pins a landed beacon, so a beacon nudged by
anything else cannot enter the same state — see
[`../todo.md`](../todo.md) for the `Update()` velocity clamp.

**This is stock content, not a mod invention.** `omegaSpin` is stored as
plaintext in `bzone.zfs`, and the stock `apcamr` definition reads
`unitName = "Nav"`, `aiName = "PowerUpProcess"`, `[CameraPodClass]`,
`omegaSpin …` — the nav beacon *is* a camerapod in stock BZ98R, and the
workshop mod's ODF is substantially the stock one. Any earlier framing of this
as a mod defect was wrong.

**Leading unverified hypothesis for the 33 Hz vs 1.0 Hz split:** that a
*selected* beacon slaves its yaw to the driver's view, so the one beacon a
player currently has targeted changes orientation continuously while the rest
sit still. This came second-hand from web research and **we have not confirmed
it**; it is stated here because it fits the data better than anything we
generated ourselves and because it is cheap for someone with engine knowledge
to confirm or kill. It also predicts the storm should begin at *selection*
rather than at creation, which the `netpktlog` run can test.

### 3. What we are *not* recommending

- **`omegaSpin = 0.0` on the beacon ODF.** Falsified twice: `apcamr349` has the
  identical ODF, is spinning, and sits at 1 Hz; and the game's own object
  snapshots show every beacon's rotation matrix changing at every sample while
  the quiet ones still sync at 1 Hz. Rotation does not drive this — and since
  the value is stock, changing it is a content fork, not a fix.
- **Pacing/token-bucketing the sender.** A pacer spreads the storm rather than
  stopping it, and adds latency to exactly the ping traffic auto-kick measures.
- **FEC.** Adding parity to a stream that is already sending 4–8 copies of
  everything is redundancy on top of redundancy.

---

## Verifying, and the outstanding experiment

Object mix from any player's `BZLogger.txt` after a match:

```python
import re, binascii, collections
pat = re.compile(r'TRY Sent Packet \((\d+),(\d+)\) to (\S+): ([0-9a-f]+)')
obj = collections.Counter()
for line in open('BZLogger.txt', errors='replace'):
    if 'TRY Sent Packet' not in line:
        continue
    for m in pat.finditer(line):
        b = binascii.unhexlify(m.group(4))
        s = ''.join(chr(x) if 32 <= x < 127 else '.' for x in b)
        names = re.findall(r'[A-Za-z0-9_]{5,}', s)
        obj[names[-1] if names else '(none)'] += 1
for k, v in obj.most_common(10):
    print(f'{v:9d}  {k}')
```

BZLogger has torn/concatenated lines, so **count regex matches, not lines**.
Timestamps are each machine's local time.

Score on **per-object emission rate**, not on retransmit totals: divide each
object's distinct `(peer, seq)` count by its first-to-last span. ~1 Hz is
normal; **anything above ~5 Hz is the fault**.

### The experiment that would close this

**Partly done.** `-netpktlog` (above) already settled the channel question and
gave a normal-session baseline. What remains is to catch a *runaway* beacon with
that logging on, and the protocol is
[`../docs/PACKET_LOG_TEST.md`](../docs/PACKET_LOG_TEST.md).

The decisive step is **select a beacon and hold it selected**, then deselect.
The leading hypothesis for the 33 Hz vs 1 Hz split is that a selected beacon
slaves its yaw to the driver's view; if its reliable rate jumps on selection and
falls on deselection, the question closes in a single run.

Two switches remain unexplored — `bzrnetlog`/`bzrnetlog=` and
`netlog`/`netlog=` (both take values, so probably verbosity levels). Those
would add the transport-level `REL`/`UNR`/`BAS`/`CON` lines and the inbound
`Received Packet … diff` timing, which `-netpktlog` alone does not produce.

---

## REPRODUCED, then FIXED and re-tested

**2026-08-10, controlled two-player matches, `-netpktlog`, narrated in chat.**

```
15:28:46   apcamr241 created ....................... 2 packets, then silence
15:28:51   "nav on geyser"  (placed, nothing else) .. still silent, 2.73 rel/s
15:29:13   "armory sent to nav on geyser"  <-- command issued
           ... 14 s of flight time ...
15:29:26   1 packet
15:29:27   26 packets   <-- ignition
15:29:28   73 packets
15:29:29   22 packets
```

It never stopped. The beacon ran for the remaining **nine minutes** of the
match — 3,307 messages in the next 81 s, then 1,021, then 1,522 — **5,779
packets at 25.9/s average on the reliable channel**, against a normal cost of
two packets for a beacon's entire life.

The same run contains its own control: placing the beacon on a geyser and doing
nothing else left it silent. **Placement is not the trigger. Delivery is.**

Whole-run consequences, all measured in the same capture:

| | value |
|---|---|
| packets dropped before send | **4,321 (7.52%)**, 4,269 of them `7a75` |
| governor budget | fell 39,950 → **26,650 B/s** |
| retransmits | 15,770, of which 15,596 `7a75` |
| reliable share of packets | 14.5%, up from ~1% in a quiet match |

The drop count is the engine's own prioritise-and-drop finally visible under
real pressure — in every quiet session it was ~0.05%.

### Mechanism

`SBPNavLogic.lua:260`:

```lua
SetPosition(p, GetPosition(NavManager[x]))
```

The mod teleports an incoming powerup to the beacon's **exact** coordinates.
The two solids interpenetrate, collision response pushes them apart, and the
beacon never reaches a resting transform again — so the engine re-sends its
full reliable state record every frame, for the rest of the match. The state
block (bytes 17–29) changes on every message throughout, exactly as in the
storms.

This mechanism was proposed as "Defect 2" in the first draft of this document
and then demoted on the grounds that the timing argued against it. The timing
was the powerup's 14-second flight; the wrong end of it was being measured.

### The fix, verified

`resources/navfix/` carries the patch and an apply/revert script. It lands the
item 3 m to the side rather than at the beacon's coordinates, and skips items
already in place. The patched module announces itself twice via
`DisplayMessage` — at mission start, and the first time the snapping branch
actually runs — because a fix that silently fails to load produces a clean
result that means nothing. That happened once: an override placed in `addon/`
was never read, and the resulting quiet run had to be discarded.

Both markers fired on the verification run:

```
16:14:43  NAVFIX ACTIVE - patched SBPNavLogic loaded
16:19:57  NAVFIX: snap fired, item placed beside nav
```

| | unpatched (test 5) | patched (test 8) |
|---|---:|---:|
| match length | 13 min | **18 min** |
| worst beacon | `apcamr241` **5,779 packets @ 25.9/s** | `apcamr234` 656 @ **1.47/s** |
| flagged as a runaway | **yes** | no |
| packets dropped before send | 4,321 (**7.52%**) | **30 (0.03%)** |
| retransmits | **15,770** | **135** |
| reliable share of bytes | 4.1% | **0.7%** |

250x fewer drops and 117x fewer retransmits, over a longer match with several
armory deliveries.

**Not a clean kill.** Every other beacon in that run sat at exactly 2 packets,
but `apcamr234` still trickled at 1.47/s, peaking 2.10/s. The storm and its
network consequences are gone; something still nudges that one beacon
occasionally. Open question.

## The normal cost of a nav beacon, and what has been eliminated

Measured 2026-08-10 with `-netpktlog` on the modded map, narrated in chat so
each segment could be scored separately
([`../docs/PACKET_LOG_TEST.md`](../docs/PACKET_LOG_TEST.md)):

| beacon | packets | when |
|---|---:|---|
| `apcamr229` | 2 | at creation, then nothing |
| `apcamr231` | 2 | at creation, then nothing |
| `apcamr232` | 2 | at creation, then nothing |

**A nav beacon sends two packets and is then silent for the rest of the match**
— not even the ~20 Hz unreliable position stream that vehicles send. So a
runaway is not a chatty object getting chattier. It is a *permanently silent*
object suddenly emitting 20–50 times a second on the reliable channel.

Reliable traffic held flat at **1.00–1.18/s across all seven segments** — still,
driving, selected, driving+selected, deselected, recycler-to-geyser, remote
build. The beacons were provably alive throughout and one was selected.

Eliminated as triggers, each with a measurement rather than an argument:

| candidate | how it died |
|---|---|
| player count | the 08-08 storm was a **2-player** match |
| `omegaSpin = 1.0` | `apcamr349` had the identical ODF, spinning, at 1 Hz |
| Lua `Send`/`Receive` | script messages carry a `#`+type marker and *fell* during storms |
| player movement | identical 2 packets whether stationary or driving |
| selection (nav in HUD) | 1.11/s vs a 1.00/s baseline — no change |
| the `observer.mesh` flood | absent from one storm entirely; 1 error in the crash log |
| scrap deletion | a separate, long-known lag issue. Through the 08-08 storm the destruction rate is flat at 56–203/min and **peaks at 468/min when retransmits are zero** — anticorrelated |

**Not eliminated, and never actually tested:** the mod's per-second nav
renaming. In the one run that ordered a remote build, the silo completed at
15:13:36 and the log ended at 15:13:40 — four seconds. The pre-flight runaway
began roughly 60 s *after* its build order and ran for 58 s while the silo was
going up.

Every quiet two-player test has come back flat, while all four storms came out
of real matches — including a two-player one that ran 17.6 minutes. The next
capture should be a live crew match, on the machine that has produced every
storm in the dataset.

## What is not established

- **Why one instance runs away and an identical one does not.** Every mechanism
  we can test from outside — rotation, placement, the mod's own scripting —
  either fails or is unfalsifiable from logs.
- **Whether the reliable volume is cause or symptom.** If primary `UNR` traffic
  were already saturating the link and starving the ack path, the reliable
  backlog would look exactly like this. Nothing in a `TRY`-only capture
  distinguishes the two orderings. The `netpktlog` run would.
- **The `zu` label.** "Engine object state" is inference: it carries an engine
  object identity and lacks the `#` script-message marker. Not confirmed.
- **What `BAS` and `CON` carry**, and the meaning of the `Type %u` field in the
  inbound drop message. We have only ever observed type 0.

## Corrections to earlier work

| claim | status | what is true |
|---|---|---|
| "BZ sends each reliable message several times up front, on principle — proactive redundancy" (`RETRANSMIT_STORM.md`) | **retracted** | It is a retransmission timeout fixed at ~10 ms with no RTT adaptation. Same observation, wrong cause — and the correct one is actionable. |
| Byte 0 bit 7 = "this datagram is a retransmit" | **wrong** | `0x80` = reliable, `0x40` = final. Nothing in the header marks a resend. See [`BZ_P2P_HEADER.md`](BZ_P2P_HEADER.md). |
| Offsets 2–4 = version, 5–9 = u40 clock | **wrong** | One u64 epoch-ms send clock at offset 2, stamped per copy. |
| Player count is the storm trigger | **disproved** | The 08-08 storm was a two-player match. |
| `omegaSpin = 1.0` causes it | **falsified** | Identical ODF, spinning, 1 Hz. |
| "Storm window equals the beacon's lifetime" | **circular** | The only source for the lifetime is the storm's own packets. |
| The `observer.mesh` flood is implicated | **no** | Absent from one storm entirely, one error total in the crash log. It overlaps one storm by ~3 minutes and continues 19 minutes past it. |
| The workshop mod repurposed a camera pod as a nav beacon | **wrong** | Stock BZ98R already ships `apcamr` as `unitName = "Nav"` with a `[CameraPodClass]` and `omegaSpin`. The mod's ODF is substantially stock. |
| The mod's Lua could be driving it, so pin the beacon from `Update()` | **withdrawn** | Script messages are identifiable (`#` + type char) and *fell* during storms. The earlier proposed Lua fix was built on a mechanism that no longer stands. |

## Provenance note

Findings here come from three sources, and they are not equally strong.
**Direct measurement** of our own captures, and **strings from the shipped
binary**, are first-hand and reproducible. **Web research** was used for the
Mates `.plan` and the BZ1/BZ2 lineage — that one quote was retrieved and
verified independently before being relied on, after an automated research pass
produced a fabricated version of it, then wrongly retracted the real one. Treat
any web-sourced claim in this document as load-bearing only where a URL is
given and the retrieval is stated. The "selected beacon slaves its yaw"
hypothesis is explicitly **not** in that category.

Raw captures are available on request — they contain player IP addresses, so
they are not published.
