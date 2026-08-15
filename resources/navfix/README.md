# Nav beacon fix

Two one-line-ish changes to `SBPNavLogic.lua` in Steam Workshop mod
**3406347034**. Both were reproduced, patched and re-measured on the same map.
Cause and full evidence: [`../CAMERAPOD_STORM.md`](../CAMERAPOD_STORM.md).

A nav beacon nobody touches costs **2 network messages for its entire life**.
Before these fixes, one beacon reached **5,779**.

## Result

| | unpatched | + fix 1 | + fix 2 |
|---|---:|---:|---:|
| camerapod reliable packets | **5,779** | 669 | **14** |
| worst single beacon | 5,779 @ 25.9/s | 656 @ 1.47/s | **2** |
| packets dropped before send | 4,321 (7.52%) | 30 (0.03%) | 25 (0.06%) |
| retransmits | 15,770 | 135 | **110** |

With both applied, seven beacons emitted 2 packets each and nothing more —
the floor. Several armory deliveries during that run moved none of them.

## Fix 1 — don't teleport items into beacons

`SBPNavLogic.lua:260` snapped an incoming powerup to the beacon's *exact*
coordinates. Two solids at the same point interpenetrate, collision response
keeps pushing them apart, and a beacon that never comes to rest has its full
state re-sent on the **reliable** channel every frame for the rest of the
match.

Fix: land it 3 m to the side, still well inside pickup range, and skip items
already in place.

## Fix 2 — don't rewrite an unchanged name

`UpdateNavInfo()` composes a nav's name — including a live count of scrap
within 125 m — and calls `SetObjectiveName` once a second, every second.

**`SetObjectiveName` dirties an object for replication even when the string is
identical.** Skipping the redundant writes took one beacon from 656 packets to
2. This is the more broadly useful finding: it affects any script that
refreshes a label on a timer.

Fix: cache the last name per handle, only write on a real change.

The helper also guards `h == nil`, because `ClearDeadNavs()` nils entries
inside `NavManager` and leaves holes, so `NavManager[x]` can be nil. The
original `SetObjectiveName` call swallowed that silently; a table lookup does
not. Worth fixing separately — `#` on a table with holes is undefined in Lua
and can silently truncate.

## Testing it locally

```sh
./apply_navfix.sh            # patch every installed copy, backing up to .orig
./apply_navfix.sh --revert   # restore
```

Covers native, Flatpak and Snap installs. Refuses to run if the mod's code has
changed shape, and auto-restores if the result does not compile.

**Verify it loaded** — three markers appear in `BZLogger.txt`:

```
Chat Message: NAVFIX ACTIVE - patched SBPNavLogic loaded   mission start
Chat Message: NAVFIX: snap fired, item placed beside nav   first armory snap
Chat Message: NAVFIX2: rename guard active                 first real rename
```

If they do not appear the patch is not loaded and any result is meaningless.
That happened twice: an override in `addon/` is never read for `require()`d
modules, and `print()` does not reach BZLogger under Proton — `DisplayMessage`
does.

Nothing here is CRC-checked: `crc32host.log` lists only the map script and zero
SBP modules, so a patched copy still joins a normal lobby. **Steam restores the
workshop copy on re-sync** — this happened three times during testing. Re-run
the script if the markers stop appearing.

`SBPNavLogic.patch` is the diff, for sending upstream.
