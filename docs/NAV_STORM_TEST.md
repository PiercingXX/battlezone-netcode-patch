# Nav Beacon Storm Test — one evening, no code

Every retransmit storm in the dataset is one object type: the SBP mod's **Nav
beacon** (`apcamr`/`spcamr`/`cpcamr`, `classLabel = "camerapod"`). Analysis and
exact file/line references:
[`../resources/CAMERAPOD_STORM.md`](../resources/CAMERAPOD_STORM.md).

What is *not* established is why one beacon runs away while its identical
siblings stay quiet. This test answers that with four short matches. It needs
no patch changes, no code, and no throttling — just people placing navs on
purpose instead of by accident.

## Ground rules

- **One host for every arm.** Host role is a controlled variable
  (see [TESTING.md](TESTING.md)). If the host changes, the arms are not
  comparable and the evening is wasted.
- **Everyone on the same build.** As of 2026-08-09 the crew is split across
  V4.91 and V4.92 proxies. Re-run the install command before starting and check
  the first lines of your proxy log say the same build id as everyone else's.
- **Same map, same player count, every arm.** `uesrtst1.bzn` is the obvious
  choice — three of the four known storms happened on it.
- **8 minutes per arm** is enough. The 08-08 storm was fully developed inside
  90 seconds and ran 11 minutes.
- **Announce each arm in the channel with a timestamp.** That is how the arms
  get separated in the logs afterwards.
- Nobody quits mid-match unless the arm calls for it. A player leaving mid-match
  is the one event correlated with the 2026-08-09 crash and it will muddy this.

## The arms

Run them in this order. Each is a fresh match on the same map with the same
people.

### Arm A — no navs at all (baseline)

Nobody places a nav beacon for the whole match. Play normally otherwise.

**Expect:** no storm. If a storm happens here, the whole camerapod finding is
wrong and this is the most valuable result of the night — say so loudly.

### Arm B — navs on flat ground

Each player places 2–3 navs, all on **visibly flat, open ground**, away from
buildings, powerups and other navs. Leave them alone for the rest of the match.

**Expect:** no storm, or a small one. This is the arm that tests whether the
beacon storms *just by existing*.

### Arm C — navs on slopes and on top of things

Deliberately place navs badly, the way they get placed in a real game:
on hillsides, on crater rims, on top of powerup stacks, on top of each other,
against building walls. 2–3 each again.

**Expect: this is where the storm should appear.** The mod's own source comment
says navs on slopes slide, and a sliding beacon never reaches a resting
transform — which is exactly the signature in the payloads.

### Arm D — armory-delivered navs, and items sent to them

Two things at once, both about delivery rather than hand-placement:

1. Launch navs **from the armory** instead of placing them by hand, and let
   them land wherever they land.
2. Send armory items to navs repeatedly, the ordinary use of the feature. The
   mod teleports incoming powerups onto the beacon
   (`SBPNavLogic.lua:260`).

**Expect: this is the second-most-likely arm after C.** In every storm on
record the runaway ignites at the beacon's *first appearance* — which points at
how it arrived and where it came to rest, not at something that happened to it
later. That timing is also the main argument *against* the powerup-snap being
the trigger, so if D storms only when items are sent, that is a real result.

## Arm E — the ODF probe: **do not bother**

`omegaSpin = 1.0` looks like the culprit and the dataset falsifies it twice
over: `apcamr349` had that ODF, was spinning, and sat at 1 Hz for twelve
minutes while `apcamr346` beside it ran at 33 Hz — and the game's own object
snapshots show *every* beacon's rotation matrix changing at every sample while
the quiet ones still sync at 1 Hz. Rotation does not drive replication.

Kept here only so nobody re-proposes it. Skip it and spend the time on C.

A loose ODF in the game folder overrides the packaged mod. **Every player must
do this, or the game will desync.** In the Battlezone 98 Redux folder, create
`apcamr.odf` containing:

```ini
[GameObjectClass]
baseName = "apcamr"
classLabel = "camerapod"
scrapCost = 0
scrapValue = 0
maxHealth = 750
maxAmmo = 0
unitName = "Nav"
aiName = "PowerUpProcess"
heatSignature = 0.0
imageSignature = 1.0

[CameraPodClass]
omegaSpin = 0.0
rangeScan = 75.0
periodScan = 5.0
```

Copy it to `spcamr.odf` and `cpcamr.odf` as well (all three are identical in the
mod). Then re-run the arm that stormed hardest.

**Prediction: no change.** If arm E *does* fix it, that prediction was wrong
and the fix is a one-line pull request to the mod — say so loudly.

**Delete all three files afterwards** — they are a test override, not a config.

## What to collect

Nothing special: the normal bundle from every player, as always. Post them in
the channel with a note naming which arm was which and roughly when each
started.

## How it gets scored

```bash
python3 tools/analyze_drops.py <BZLogger.txt> <proxy log> --launch <lobbyname>
```

The line that decides the arm is the retransmit line — `datagrams/min` and
`copies each`. For reference, from the existing data:

| | retx/min | copies | verdict |
|---|---:|---:|---|
| quiet match | 50–130 | 1.1–1.5 | no storm |
| 08-09 `Peppy` (client storm) | 1,818 | 1.87 | storm |
| 08-08 `host` (2 players) | 5,780 | 4.06 | storm |
| 08-09 `vbgthykuj` | 20,317 | 2.47 | worst on record |

Then run the payload decoder in
[`../resources/CAMERAPOD_STORM.md`](../resources/CAMERAPOD_STORM.md) over each
arm's BZLogger to confirm what the retransmits were carrying. An arm that
storms on something *other* than a camerapod is a new finding.

**Score the arms on per-beacon emission rate, not on the retransmit total.**
That is the real fault line and the totals blur it. The same decoder gives it
if you keep timestamps: divide each beacon's distinct `(peer, seq)` count by
its first-to-last span.

- ~1 Hz — settled. This is the mod's own `NavComputeFrequency = 1` sweep and is
  the expected floor for every beacon.
- **anything above ~5 Hz — a runaway.** One of these is the whole bug.

Do **not** score on "the beacon's transform keeps changing" — it does that on
quiet beacons too, and it discriminates nothing.

## Reading the result

- **A quiet, C loud** → bad placement is the trigger, as expected. The fix is
  upstream in the mod (pin landed beacons) plus the proxy-side damper for
  everyone still running the current mod build.
- **D loud, B and C quiet** → delivery, not slope. If it only storms once items
  are *sent* to a nav, the powerup snap at `SBPNavLogic.lua:260` is implicated
  after all, against the timing evidence.
- **B and C both loud** → the beacon storms just by existing, which contradicts
  the 346-vs-349 pair. Re-check that both beacons in B really were on flat
  ground before concluding anything.
- **Nothing storms at all** → the storm needs something this test did not
  reproduce (combat intensity, object churn, a specific player's link). Post the
  bundles anyway; a clean negative narrows it.

### Cheaper than a full evening

Two short-lived runaways in the existing data self-terminate: `apcamr342`
(08-08, 500 datagrams over 16:39–16:41) and `apcamr595` (08-09 Peppy, 4,576
datagrams over 14:53–14:54). A brief runaway is a far cheaper repro target than
a twelve-minute one — whatever ends those ends the long ones too. If someone
can make a beacon spike for twenty seconds and then settle, on demand, that is
worth more than four clean 8-minute arms.
