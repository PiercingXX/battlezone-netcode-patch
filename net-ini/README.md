# Host-Side net.ini Tuning

The DLL proxy fixes the *receive* side (out-of-order drops). This `net.ini`
fixes the *send* side: BZ98R's `Net::AdjustBandwidth` governor cuts the send
rate every time ping exceeds `MaxPing` and caps it at `MaxBandwidth`. With
the stock (or the popular workshop "Auto-Kick Reduction Patch") values, one
jitter spike can collapse the send rate to a trickle and the lag feeds
itself.

**This only takes effect on the machine hosting the game.** Clients gain
nothing from installing it, but it never hurts.

## Install

Copy `net.ini` into the game folder (next to `battlezone98redux.exe`).

## Precedence Warning

Workshop mods can ship their own `net.ini` and win over the local file.
After launching, check `BZLogger.txt` for the line:

```
MOD FOUND net.ini at ...
```

If it points at a `workshop\content\301650\...` path, the workshop copy is
being used — **unsubscribe** from that mod (commonly `1895622040`, the
"Auto-Kick Reduction Patch") to let this file load instead. Disabling the
mod in the in-game mod manager is NOT enough: its net.ini still loads
(confirmed in live testing). This file already includes that mod's
auto-kick relaxations, so you lose nothing.

## Verify (Host)

After hosting a match, check `BZLogger.txt` for:

```
Net: Bandwidth usage now set to ...
```

With the stock/workshop config it starts at `4000` (4 KB/s). With this file
it starts at `16000`. That number is the fastest way to confirm which
net.ini actually won.

## Keys Not Set Here

The engine also reads `MaxPingsLost`, `LimitLowNPPI`, `LimitHiNPPI`,
`DivisorMPPI2NPPI`, and `DivisorPing2NPPI` (packets-per-interval pacing).
Their stock defaults and safe ranges are unmapped; left alone until someone
instruments them.
