# Packet-log capture — `-netpktlog`

The game ships an undocumented switch that logs every application-layer packet
it considers sending, with the channel and the send decision. Nothing in this
project knew about it until 2026-08-10. It answers in one short match what
transport-level analysis could not answer in weeks.

Score with:

```bash
tools/analyze_netpktlog.py "<game dir>/BZLogger.txt"
```

Background and what the fields mean:
[`../resources/CAMERAPOD_STORM.md`](../resources/CAMERAPOD_STORM.md).

---

## What the switch gives you

Three line kinds appear in `BZLogger.txt`:

```
TX SRC  2 DST  1  Sent: Yes Packet: 7a7501000c0000... Send Type: 1
RX SRC  1 DST  1 Packet: 0101fb147a6a00000000
Chat Message: Bandwidth = 39950, used rate = 120
```

- **`Send Type`** is the channel. `0` = unreliable, `1` = reliable. Verified:
  every type that appears in a `TRY` retransmit line is Send Type 1, and no
  Send Type 0 type has ever appeared in one.
- **`Sent: Yes/No`** is the engine's own prioritisation decision — it drops
  lower-priority packets under bandwidth pressure.
- **`Packet:`** is the full application payload, starting where the 18-byte
  transport header ends. `7a75` bodies carry an object identity such as
  `apcamr230_camerapod`.
- **`Bandwidth = N, used rate = M`** is the game's own accounting, per tick.

## Baseline, from a 9-minute two-player session (2026-08-10)

Use these as the reference for "normal". Anything wildly off is the finding.

| | value |
|---|---|
| unreliable | 95.1% of packets, **99.0% of bytes**, 47.4 pkt/s |
| reliable | 4.9% of packets, 1.0% of bytes, 2.5 pkt/s |
| dominant unreliable type | `5f00` (~220 B, several objects' position/velocity per packet) |
| dominant reliable type | `7a75` (object identity) |
| a *settled* nav beacon | `apcamr230_camerapod` at **0.73/s reliable** |
| a busy vehicle | `avremp` at **20.9/s unreliable** — high and completely fine |
| dropped before send | 15 of 27,187 (0.06%) |
| used rate | median 11.2 kB/s, p90 14.8 kB/s, max 70.6 kB/s against a ~39.9 kB/s budget |

The contrast to keep in mind: **a high rate on the unreliable channel is
normal.** The fault condition is a high rate on the *reliable* channel, because
only reliable traffic enters the retry path, and the retry timer multiplies it
4–8×.

---

## Setup

**Use the install you actually play on.** Replication is owner-based, so the
machine placing the beacons must be the machine logging. The other player needs
no flag, no wrapper and no special build.

Native Steam launch options for the test:

```
WINEDLLOVERRIDES=dsound=n,b %command% -nointro -netpktlog
```

Restore afterwards:

```
WINEDLLOVERRIDES=dsound=n,b "${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode/bz_wrap.sh" %command% -nointro
```

### Three things that will bite you

1. **The game truncates `BZLogger.txt` on every launch.** Copy the log out
   before launching again, or the capture is gone.
2. **Drop the upload wrapper**, as above. A wrapped run will try to Discord-post
   a very large log and split it into dozens of webhook parts. Analyse locally
   instead.
3. **Without the wrapper, `bznet.env` is not read.** Put any `BZ_*` override
   directly in the launch line instead.

Size: the pre-flight produced 25 MB for 9 minutes at a low packet rate. A busy
match will run higher — budget a few hundred MB and check free space first.

---

## The arms

Same map, same two players, same host throughout. Five minutes each is plenty;
the storms in the record were fully developed inside 90 seconds.

### Arm 1 — the discriminator *(the important one)*

**What this is separating.** On 2026-08-10 a beacon was caught running away —
90 reliable messages at a steady 0.70 s cadence for 58 s, against a normal cost
of **2 messages for a beacon's entire life**. Three things were true at once in
that window, and any of them could be the trigger:

- the player was **driving**;
- a **constructor had been ordered to build a silo at that nav**, which changes
  the nav's name and its live scrap count every second
  (`SBPNavLogic.UpdateNavInfo`);
- the nav may have been **selected** in the HUD.

It stopped the moment the player left the vehicle and went to pilot on foot.

Run four ~60 s windows, one variable at a time, with a nav beacon nearby and
**nothing else happening**. Announce each in chat so the timestamps can be cut
apart afterwards.

| # | window | what to do | tests |
|---|---|---|---|
| 1 | **still** | sit in a vehicle, nothing selected, no construction | baseline — expect ~0 |
| 2 | **driving** | drive back and forth past the nav, nothing selected | player movement |
| 3 | **selected** | stop, then select the nav so it shows in your HUD | selection |
| 4 | **renaming** | deselect, stay still, order a constructor to build at that nav | the mod's per-second rename |

Whichever window lights up is the trigger. If more than one does, we have the
relative contribution. If none does, the trigger is something none of us has
thought of yet, and that is still worth knowing.

Score each window separately:

```bash
for w in "1 14:30:00 14:31:00" "2 14:31:10 14:32:10" \
         "3 14:32:20 14:33:20" "4 14:33:30 14:34:30"; do
  set -- $w; echo "== window $1"
  tools/analyze_netpktlog.py BZLogger.txt --since $2 --until $3 --top 6 | sed -n '/OBJECTS/,/^$/p'
done
```

### Arm 2 — placement

Only if arm 1 does not reproduce. Place beacons deliberately badly: on slopes,
crater rims, on top of powerup stacks, against building walls, stacked on each
other. Leave 5 minutes.

### Arm 3 — does our own patch unmask this?

```
WINEDLLOVERRIDES=dsound=n,b BZ_NET_TUNE=0 %command% -nointro -netpktlog
```

BZ1's design *"had to prioritize packets, and could drop some lesser-priority
packets in order to keep bandwidth reasonable"* (Nathan Mates, 1999-01-28). Our
patch raises `MaxBandwidth` from 16,000 to 320,000, which removes exactly that
pressure.

So it is possible the storms we have been measuring are partly **unmasked by
the patch** — traffic the engine would have shed at stock bandwidth. Repeat
whichever arm reproduced, with tuning off, and compare the `SEND DECISIONS`
block. If drops rise sharply and the beacon rate falls, that is a finding about
our own patch, and an uncomfortable one worth having.

---

## Scoring

```bash
tools/analyze_netpktlog.py "<game dir>/BZLogger.txt" --top 20
# narrow to one segment of the run:
tools/analyze_netpktlog.py BZLogger.txt --since 14:20:00 --until 14:21:00
```

Read it in this order:

1. **`OBJECTS` table.** Any object flagged `RUNAWAY` — sustained ≥5 Hz on the
   reliable channel — is the fault. Compare the selected beacon against its
   neighbours.
2. **`CHANNEL SPLIT`.** Reliable should stay around 1% of bytes. If it climbs
   toward parity with unreliable, the storm is running.
3. **`SEND DECISIONS`.** Drops near zero means the sender was never
   bandwidth-limited. This is the arm-3 comparison.
4. **`RETRANSMITTED TYPES`.** The tool warns if an unreliable type ever appears
   in the retry path, which would contradict the channel model and would be a
   significant finding in its own right.
5. **The game's own bandwidth accounting**, as a sanity check against the
   proxy's numbers.

Take the `--since`/`--until` window from the chat callouts so each step of arm 1
is scored separately. The selected-beacon segment is the one that matters.

## What to send back

The `BZLogger.txt` itself is large and contains peer IP addresses — **do not
post it publicly.** The analyzer output is small and carries only player ids, so
that is the thing to share.
