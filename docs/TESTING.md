# Running a test session

The numbers that matter are **visible warps per minute** and **retransmits per
MB**, not the raw drop count. Collecting them takes two files per player, per
game.

## Before you play

1. Everyone installs the patch and confirms `VERIFY RESULT: PASS`.
2. **Agree who is hosting, write it down, and hold it constant across the A/B.**
   See "Host role is a variable" below — this is not just an auto-kick note.
3. If anyone is subscribed to a Steam Workshop mod that ships its own `net.ini`
   — notably **"Auto-Kick Reduction Patch" (`1895622040`)** — unsubscribe. It
   overrides the patch's copy and caps send rate at 32 KB/s. *Disabling it
   in-game is not enough.*

### Host role is a variable — control it

Every anomaly in the 2026-07-26 dataset splits on host vs client:

- The game's own log format differs. Only host logs carry `Actual Used`, the
  measured send rate — so only a host log can say whether the governor budget
  was ever the binding constraint.
- Both hard stops in the repo are host-side.
- The one extreme retransmit storm (786 datagrams/min, 20× anything else) is
  host-side.
- Auto-kick thresholds are enforced by the host only, so the host's install
  decides whether anyone gets kicked.

No A/B run so far has held the role constant, which means no A/B so far can
separate "the setting helped" from "a different person hosted". Record the host
in the session notes and keep it the same across the arms you intend to compare.

### Confirm a crash would actually be captured

`tester_diag start` now prints `crash capture: READY` or a loud `NOT READY`
banner and writes `crash_capture_status.txt` into the bundle. Read it. Both
hard stops in this repo produced no dump, no proxy log and no session-end line,
and nobody found out until the bundle was opened days later.

On Windows this needs `procdump.exe` on `PATH` or in `C:\Sysinternals\`; a full
dump of this game runs 1–2 GB. On Linux it needs `kernel.core_pattern` routed
to `systemd-coredump` and `ulimit -c` not set to 0 in the shell that launches
Steam.

## Automatic upload (recommended)

`upload/bz_wrap.sh` / `bz_wrap.ps1` wraps the game launch, bundles the session
on exit and posts it to the private Discord channel. It removes the two ways
sessions currently die — BZLogger overwritten by a relaunch, and crash bundles
never sent because the tester restarted first — because it snapshots BZLogger
*before* launching and fires even when the game crashes.

See [../upload/README.md](../upload/README.md) for setup. One line, once.

**What an opted-in launch option sends, plainly:** on every game exit it uploads
your proxy log, your `BZLogger.txt` from before and after that session,
`multi.ini`, any buffer capture, and a meta file with your player name,
hostname, the UTC time and your local UTC offset.

**Those logs contain every peer's public IP address** — yours and everyone
else's in the lobby. That is why the destination is a private channel and
nowhere else. Adding the wrapper to your launch options is the consent step;
there is no silent default, and it cannot run unless you pasted that line
yourself. Remove the line and nothing is ever sent again.

## After each match

> Without the upload wrapper, note that **`BZLogger.txt` is overwritten every
> time the game launches.** Copy it aside *before* anyone relaunches, or the
> session is gone. This is exactly how game 2 of 2026-07-26 lost its log.

From the game folder, copy both:

- `BZLogger.txt` — the game's own log (drops, warps, kicks, governor)
- `dsound_proxy.log` (Linux) or `winmm_proxy.log` (Windows) — the patch's counters

Rename per player and per game, e.g. `game3_piercingxx_BZLogger.txt`.

**Zip and attach.** Pasted logs always truncate — these run to tens of megabytes.

## Reading the results

```bash
python3 tools/analyze_drops.py game3_piercingxx_dsound_proxy.log game3_piercingxx_BZLogger.txt
```

| What it reports | Healthy | What a bad value means |
|---|---|---|
| visible warps/min (≥50 m) | low | the actual symptom — the raw warp count is ~90% sub-metre noise |
| retransmit share of outbound bytes | low | the log's real loss signal, against a real byte denominator |
| `actual send rate … % of budget` | — | **under 60%:** the governor was not the constraint, so raising it cannot help. **Over 90%:** genuinely governor-limited, a valid `BZ_GOV_START` sample |
| discards per peer | — | **not loss.** Every line is a duplicate or a late retransmission of a packet already consumed |
| `governor` range | opens at 40000 | still opening at 4000, or a `POKE DID NOT HOLD` line, means the session is not a sample of the setting you asked for |
| ending | `ended cleanly` | `ENDS ABRUPTLY` is a crash; `still in the lobby` means the file was copied mid-game |
| `observer.mesh` errors | 0 | this flood precedes both committed hard stops |
| torn log lines | 0 | concurrent proxy writers; a torn line is unparseable by everything downstream |
| `hold_ms max` | well under 100 | latency *this patch* is adding to you; only non-zero with `BZ_REORDER=1` |
| `evicted` / `emsgsize` | 0 | queue overflowed, or datagrams were too big for the drain buffer — report it |
| `peak_pps` / `burst_seconds` | low | *your* machine is producing retransmit floods |

Three traps worth naming:

- **A drop-count improvement bought with a large `hold_ms max` is a
  regression.** Holding a packet stops the game counting it as dropped whether
  or not you're better off. Every earlier round of testing walked into this.
- **Warp rates are strongly map-dependent.** Only ever compare the same map.
- **Hold the denominator fixed.** The retransmit share is computed against the
  proxy's byte counter when you pass the proxy log alongside, and against an
  estimate integrated from the host log's `Actual Used` when you don't. The
  tool names which one it used; a counter and an estimate are not comparable
  with each other.

## Testing without the game

```bash
make -C tests run    # no game required
```

## Optional: raw packet capture

Only needed for deep analysis — see [../logging_readme.md](../logging_readme.md).
