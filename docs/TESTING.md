# Running a test session

The numbers that matter are **visible warps per minute** and **retransmits per
MB**, not the raw drop count. Collecting them takes two files per player, per
game.

## Before you play

1. Everyone installs the patch and confirms `VERIFY RESULT: PASS`.
2. Agree who is hosting. Auto-kick thresholds are enforced by the **host only**,
   so the host's install decides whether anyone gets kicked.
3. If anyone is subscribed to a Steam Workshop mod that ships its own `net.ini`
   — notably **"Auto-Kick Reduction Patch" (`1895622040`)** — unsubscribe. It
   overrides the patch's copy and caps send rate at 32 KB/s. *Disabling it
   in-game is not enough.*

## After each match

> **`BZLogger.txt` is overwritten every time the game launches.** Copy it aside
> *before* anyone relaunches, or the session is gone.

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
| retransmits per MB | low | the log's real loss signal, normalised so a bandwidth change can't masquerade as a loss change |
| discards per peer | — | **not loss.** Every line is a duplicate or a late retransmission of a packet already consumed |
| `governor` range | opens at 40000 | still opening at 4000 means the cold-start fix didn't apply |
| `hold_ms max` | well under 100 | latency *this patch* is adding to you; only non-zero with `BZ_REORDER=1` |
| `evicted` / `emsgsize` | 0 | queue overflowed, or datagrams were too big for the drain buffer — report it |
| `peak_pps` / `burst_seconds` | low | *your* machine is producing retransmit floods |

Two traps worth naming:

- **A drop-count improvement bought with a large `hold_ms max` is a
  regression.** Holding a packet stops the game counting it as dropped whether
  or not you're better off. Every earlier round of testing walked into this.
- **Warp rates are strongly map-dependent.** Only ever compare the same map.

### A caveat on retransmits-per-MB

The denominator is the *median governor budget*, not bytes actually sent. The
budget is known to overstate real sending — one session measured 64 kB/s actual
against a budget running to 112,700 — and the ratio isn't fixed. The figure is
only comparable between runs whose budgets are in the same range.

## Testing without the game

```bash
make -C tests run    # no game required
```

## Optional: raw packet capture

Only needed for deep analysis — see [../logging_readme.md](../logging_readme.md).
