# Task: Net.ini Bandwidth / Update-Rate Tuning

## Goal
Create a custom `net.ini` configuration file that ships alongside the netcode patch to increase the game's internal network update frequency and raise bandwidth headroom beyond the workshop mod defaults.

## Background
Battlezone 98 Redux reads network parameters from a `net.ini` file at startup.  The game's built-in BZRNet broadcast interval is controlled by `MinBandwidth`/`MaxBandwidth` and translated into a packet interval (in ms).  The current workshop mod (1895622040 — "BZ98R Multiplayer Improvements") ships a `net.ini` that targets 4 kbps and results in a **52 ms** broadcast interval (≈19 Hz).  Raising `MinBandwidth` increases update frequency; the observed ceiling before player kick auto-triggers is around 32 kbps (`MaxBandwidth`).

## Current net.ini (workshop mod 1895622040)

Path on disk (typical Linux/Proton install):
```
~/.local/share/Steam/steamapps/workshop/content/301650/1895622040/net.ini
```

Content:
```ini
[Net]
MaxPing        = 300
UpCount        = 100
DownCount      = 50
MinBandwidth   = 4000
MaxBandwidth   = 32000
AutoKickStart  = 60000
AutoKickPing   = 750
AutoKickLoss   = 75
AutoKickTime   = 45000
UseCompression = 1
```

BZLogger line produced at session start with current settings:
```
Net: Bandwidth usage now set to 4000, Interval 52 ms
```

## What to Investigate

1. **Interval formula** — reverse-engineer or test what BZLogger prints at 8000, 16000, 32000 `MinBandwidth`.  The target is an interval ≤ 33 ms (≥30 Hz).
2. **MaxBandwidth ceiling** — 32000 is the existing cap.  Test whether the engine accepts higher values without crashing or saturating a typical home connection.
3. **AutoKick thresholds** — `AutoKickPing = 750` is generous.  Tighten to 500 or 400 after confirming the host's avg ping (PiercingXX avg 20 ms, max 55.7 ms) stays well below.
4. **UseCompression = 1** — measure whether disabling compression (`0`) at higher bandwidth reduces CPU overhead and whether packet loss drops.

## Delivery

- Produce a tuned `net.ini` with comments explaining each change.
- Deploy it to `Linux/net.ini` (and `Microslop/net.ini`) in this repo alongside the DLLs.
- Update `README.md` with install path instructions (game reads from the game root or `Data/` directory — confirm which).
- Add a verification step to `verify_net_patch.sh` / `verify_net_patch.ps1` that greps BZLogger for `Interval` and confirms ≤ 33 ms.

## Done When
BZLogger shows `Interval` ≤ 33 ms in a live session and the verify scripts pass the new check.

## Reference Data
- All 4 test sessions used the existing 52 ms interval.
- Session 1 (232149Z): 6,857 total P2P drops over ~62 min.
- Warp event timestamps averaged 0.166 s apart in session 1 — a tighter update rate may reduce visible warp magnitude.
- Test bundle parent dir: `/path/to/test-bundles/`
