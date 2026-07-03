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

**The game only loads net.ini through the mod system** — a copy next to
`battlezone98redux.exe` is silently ignored (verified in live testing: file
present, no `MOD FOUND net.ini` line, stock governor values). Install it as
a local packaged mod instead:

```
<game folder>/packaged_mods/9990001/net.ini
```

The installers and `deploy_linux.sh` do this automatically. After the next
launch, `BZLogger.txt` should show
`MOD FOUND net.ini at ...packaged_mods\9990001`.

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

## Verify

Check `BZLogger.txt` after a launch for:

```
MOD FOUND net.ini at ...packaged_mods\9990001
```

That line confirms this file loaded. Note: the governor's
`Net: Bandwidth usage now set to 4000` start value does NOT change with
net.ini — live testing showed the 4,000 B/s starting rate is hardcoded and
`MinBandwidth` only acts as the floor for downward adjustments. In short
matches the send rate never reaches any ceiling, so this file's caps mainly
matter for long games. The untested lever for the slow start-of-match ramp
is `UpCount`.

## Keys Not Set Here

The engine also reads `MaxPingsLost`, `LimitLowNPPI`, `LimitHiNPPI`,
`DivisorMPPI2NPPI`, and `DivisorPing2NPPI` (packets-per-interval pacing).
Their stock defaults and safe ranges are unmapped; left alone until someone
instruments them.
