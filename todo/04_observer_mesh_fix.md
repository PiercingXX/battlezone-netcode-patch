# Task: Fix SBPAi Mod Observer Mesh Load Errors (workshop 3406347034)

## Goal
Eliminate the 107,000+ `could not load observer.mesh` errors and `AnimObj_Start` animation-not-found errors that fire throughout every session when the SBPAi mod is active.  These errors produce ~12,000 warp events per session and constant log spam that buries real diagnostic data.

## Background
The SBPAi workshop mod (ID **3406347034**) adds AI-controlled observer/spectator units that use a mesh called `observer.mesh`.  Despite the mesh file existing on disk, the engine fails to load it every time a unit spawns.  All warp events in sessions 3 and 4 are these observer units teleporting to off-map coordinates (375/600, 302.699, 500) because their visuals never initialise.

## Error Signatures in BZLogger

### Startup (once per session load):
```
2026-03-14 19:59:42.973965 ERROR: could not preload observer.mesh
```

### Per-unit warp (107k+ times in session 4):
```
2026-03-14 20:12:36.456218 ERROR: could not load observer.mesh
2026-03-14 20:12:36.456218 ERROR: could not load observer.mesh
```

### Animation system (at session start, multiple unit variants):
```
2026-03-14 19:59:47.843050 AnimObj_Start - requested obj avobserv is not in any animation
2026-03-14 19:59:47.909051 AnimObj_Start - requested obj bvobserv is not in any animation
2026-03-14 19:59:47.931051 AnimObj_Start - requested obj avobserv is not in any animation
2026-03-14 19:59:47.947052 AnimObj_Start - requested obj svobserv is not in any animation
2026-03-14 19:59:47.950052 AnimObj_Start - requested obj bvobserv is not in any animation
```

Variants seen: `avobserv`, `bvobserv`, `svobserv` (and likely `cvobserv`).

## Error Counts Per Session

| Session | `could not load observer.mesh` | Warp events |
|---------|-------------------------------|-------------|
| 232149Z | ~90,000+                      | 2,292       |
| 234646Z | ~90,000+                      | 632         |
| 235824Z | ~90,000+                      | 3,970       |
| 001821Z | **107,000+**                  | **12,343**  |

## Mod File Inventory

Mod path on disk:
```
~/.local/share/Steam/steamapps/workshop/content/301650/3406347034/
```

Observer-related files present:
```
observer.mesh      1.3 KB   ← exists but fails to load
avobserv.mesh      2.4 KB
cvobserv.mesh      (present)
bvobserv.mesh      (present)
observer.odf       (ODF unit definition — classLabel = "wingman", no mesh= line)
observer.skeleton  (present)
avobserv.vdf       (binary visual-definition file — see note below)
observer.material  (present)
observer.des       (NSDF faction description text — not relevant)
```

## Key Findings

### `avobserv.vdf` binary analysis
The VDF (Visual Definition File) for these units references geometry `obs11bda` — it does **not** directly reference `observer.mesh`.  This suggests `observer.mesh` is invoked via a separate render codepath (perhaps the unit's base class or a global observer-type resolver), not from the VDF.

### `observer.odf` does not specify a mesh
The ODF sets `classLabel = "wingman"` with no `mesh=` line.  The `wingman` class may inherit a default mesh path, and BZ98R's `wingman` loader may hard-code loading `observer.mesh` for units of that class.

### The 1.3 KB `observer.mesh` is likely corrupt / wrong format
A valid BZ98R `.mesh` file for a human-sized prop is typically 5–50 KB.  At 1.3 KB this file is almost certainly a placeholder, stub, or mesh from an incompatible game version.  The engine silently fails the size/header validation and falls back to logging the error on every access.

## What to Do

### Option A — Provide a valid stub mesh (recommended)
1. Find any valid 1.3–5 KB BZ98R `.mesh` file from the base game (e.g. a small prop or invisible bounding-box mesh).
2. Rename/copy it to `observer.mesh` and place it in the mod folder, OR ship it in this patch repo so it can be deployed alongside the DLLs.
3. Confirm the engine stops logging the error with zero `preload` / `load` errors.

**Where to find a candidate mesh:**
- Base game asset path (Linux/Proton): `~/.local/share/Steam/steamapps/common/Battlezone 98 Redux/Data/`
- Look for small `.mesh` files: `find ~/.../Data/ -name "*.mesh" -size -10k | head -n 20`

### Option B — Fix the ODF/VDF to use the correct mesh reference
Determine exactly how BZ98R resolves `observer.mesh` from a `wingman`-class ODF (static class lookup table in the EXE or data-driven?), then redirect the reference to `avobserv.mesh` which is 2.4 KB and more plausible.

### Option C — Provide a custom mesh via the patch DLL
Hook `CreateFile` / `fopen` in the proxy; intercept any open request for `observer.mesh` and redirect it to `avobserv.mesh` (which may have the correct format).  This avoids touching mod files.

## AnimObj Fix
The `AnimObj_Start` errors (`avobserv is not in any animation`) are a separate issue: the animation state machine does not have an entry for these object IDs.  The fix likely requires adding entries to the mod's animation definition file (`.ani` or similar).  Resolving the mesh load error first is higher priority since the animation errors appear to be a downstream symptom.

## Done When
A session with SBPAi active produces **zero** `could not load observer.mesh` lines in BZLogger and warp event count from `*observ*_walker` units at coord `(375/600, 302.699, 500)` drops to 0.

## Test Bundle Reference
Parent dir: `/home/piercingxx/Downloads/Testing 1 with 2mb receive/`
Worst session: `deep_linux_unknown-host_20260315T001821Z/BZLogger.txt` (12,343 warp events)
Sessions 3 & 4 are the cleanest for comparing before/after since they lack the wireless-drop noise of session 1.
