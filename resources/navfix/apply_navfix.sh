#!/usr/bin/env bash
# Apply (or revert) the nav-beacon fix to a locally installed copy of Steam
# Workshop 3406347034, for the purpose of testing it.
#
# WHAT THIS CHANGES
#   SBPNavLogic.lua, one statement.  The mod teleports an incoming armory
#   powerup to a nav beacon's EXACT coordinates; the two solids interpenetrate,
#   collision response never lets the beacon come to rest, and a beacon that
#   never rests has its full state re-sent on the reliable channel every frame
#   for the rest of the match.  Measured 2026-08-10: a beacon costs 2 packets
#   for its entire life, and 5,779 after a single armory delivery.
#   The fix places the item 3 m to the side instead, still well within pickup
#   range, and skips items already in place.
#
# WHY IT EDITS THE MOD IN PLACE
#   `addon/` is not searched for require()'d Lua modules -- tested 2026-08-10,
#   the override was ignored.  The game resolves SBPNavLogic.lua from the
#   workshop folder (confirmed in crc32host.log), so that is the copy that has
#   to change for a test.
#
# SAFETY
#   * Every file is backed up to <name>.orig before the first edit, and
#     --revert restores from it.
#   * Only SBPNavLogic.lua is touched.
#   * Nothing here is CRC-checked by the game: crc32host.log lists only the map
#     script (uesrtst1.lua), and zero SBP modules, so a patched copy still
#     joins a normal lobby.
#   * Steam may restore the workshop copy on a game validate or a workshop
#     re-sync.  Re-run this script if the NAVFIX markers stop appearing.
#
# VERIFYING IT LOADED
#   The patch adds two DisplayMessage calls, which land in BZLogger.txt as
#   "Chat Message: NAVFIX ...".  print() does NOT reach BZLogger under Proton.
#     NAVFIX ACTIVE ...                    at mission start
#     NAVFIX: snap fired ...               first time the snap path runs
#     NAVFIX2: rename guard active ...     first time a nav name really changes
#   If neither appears, the patch is not loaded and any result is meaningless.
#
# USAGE
#   resources/navfix/apply_navfix.sh            apply
#   resources/navfix/apply_navfix.sh --revert   restore the originals

set -euo pipefail

MOD=3406347034
CANDIDATES=(
  "$HOME/.local/share/Steam/steamapps/workshop/content/301650/$MOD"
  "$HOME/.local/share/Steam/steamapps/common/Battlezone 98 Redux/packaged_mods/$MOD"
  "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/workshop/content/301650/$MOD"
  "$HOME/snap/steam/common/.local/share/Steam/steamapps/workshop/content/301650/$MOD"
)

revert=0
[[ "${1:-}" == "--revert" ]] && revert=1

found=0
for dir in "${CANDIDATES[@]}"; do
  f="$dir/SBPNavLogic.lua"
  [[ -f "$f" ]] || continue
  found=1
  if (( revert )); then
    if [[ -f "$f.orig" ]]; then
      cp "$f.orig" "$f"
      echo "reverted  $f"
    else
      echo "no backup, left alone:  $f"
    fi
    continue
  fi
  [[ -f "$f.orig" ]] || cp "$f" "$f.orig"
  if grep -q NAVFIX2 "$f"; then
    echo "already patched  $f"
    continue
  fi
  python3 - "$f" <<'PY'
import sys
p = sys.argv[1]
eol = b'\r\n'
b = open(p, 'rb').read()

old = (b"\t\t\t\t\t\tif math.abs(GetPosition(NavManager[x]).y - GetPosition(p).y) < 4 and ValidForNavSnapping == true then" + eol +
       b"\t\t\t\t\t\t\tSetPosition(p, GetPosition(NavManager[x]))" + eol +
       b"\t\t\t\t\t\tend")
fix = eol.join([
    b"\t\t\t\t\t\tif math.abs(GetPosition(NavManager[x]).y - GetPosition(p).y) < 4 and ValidForNavSnapping == true then",
    b"\t\t\t\t\t\t\t-- NAVFIX: land the item BESIDE the beacon, not inside it.",
    b"\t\t\t\t\t\t\t-- Identical coordinates make the two solids interpenetrate, and",
    b"\t\t\t\t\t\t\t-- collision response then never lets the beacon come to rest.  A",
    b"\t\t\t\t\t\t\t-- beacon that never rests has its full state re-sent on the",
    b"\t\t\t\t\t\t\t-- RELIABLE channel every frame for the rest of the match:",
    b"\t\t\t\t\t\t\t-- 2 packets for a beacon's whole life, 5779 after one delivery.",
    b"\t\t\t\t\t\t\t-- Lateral, not vertical -- an item above the beacon falls onto it.",
    b"\t\t\t\t\t\t\tlocal navPos = GetPosition(NavManager[x])",
    b"\t\t\t\t\t\t\tlocal snapPos = navPos + SetVector(3.0, 0, 0)",
    b"\t\t\t\t\t\t\tif Distance2D(GetPosition(p), snapPos) > 1.0 then",
    b"\t\t\t\t\t\t\t\tif NAVFIX_SNAPPED == nil then",
    b'\t\t\t\t\t\t\t\t\tDisplayMessage("NAVFIX: snap fired, item placed beside nav")',
    b"\t\t\t\t\t\t\t\t\tNAVFIX_SNAPPED = true",
    b"\t\t\t\t\t\t\t\tend",
    b"\t\t\t\t\t\t\t\tSetPosition(p, snapPos)",
    b"\t\t\t\t\t\t\t\tSetVelocity(p, SetVector(0, 0, 0))",
    b"\t\t\t\t\t\t\tend",
    b"\t\t\t\t\t\tend"])

olds = b"\tfunction Start()" + eol + b"\t\tLocalPlayerTeam = GetTeamNum(GetPlayerHandle())"
news = (b"\tfunction Start()" + eol +
        b'\t\tDisplayMessage("NAVFIX ACTIVE - patched SBPNavLogic loaded")' + eol +
        b"\t\tLocalPlayerTeam = GetTeamNum(GetPlayerHandle())")

if b.count(old) != 1 or b.count(olds) != 1:
    sys.exit(f"anchors not found as expected in {p} "
             f"(snap={b.count(old)}, start={b.count(olds)}) -- mod version changed?")
b = b.replace(old, fix).replace(olds, news)

# ---- PATCH 2: only rename a nav when the name actually changes -------------
# SetObjectiveName appears to dirty the object for network replication, so
# rewriting an identical string once a second costs a reliable packet per nav
# per sweep.  The composed name embeds a LIVE SCRAP COUNT within 125 m, so it
# churns whenever scrap moves near the beacon -- which is why exactly one nav
# of seven trickles at ~1.4/s while the rest sit at 2 packets for the match.
helper = eol.join([
    b"\t-- NAVFIX2: rename a nav only when the name actually changed.",
    b"\t-- Rewriting an identical string still dirties the object, and the",
    b"\t-- composed name carries a live scrap count, so it churns constantly.",
    b"\tNavNameCache = NavNameCache or {}",
    b"\tfunction NavSetName(h, name)",
    b"\t\t-- ClearDeadNavs() nils entries inside NavManager, leaving holes, so",
    b"\t\t-- NavManager[x] can be nil here.  The original SetObjectiveName call",
    b"\t\t-- silently no-ops on a nil handle; a table lookup does not.",
    b"\t\tif h == nil then return end",
    b"\t\tif NavNameCache[h] ~= name then",
    b"\t\t\tNavNameCache[h] = name",
    b'\t\t\tif NAVFIX2_FIRED == nil then',
    b'\t\t\t\tDisplayMessage("NAVFIX2: rename guard active")',
    b"\t\t\t\tNAVFIX2_FIRED = true",
    b"\t\t\tend",
    b"\t\t\tSetObjectiveName(h, name)",
    b"\t\tend",
    b"\tend",
    b"",
    b"\t-- Updates nav names to display useful info to player.",
    b"\tfunction UpdateNavInfo()"])
anchor2 = b"\t-- Updates nav names to display useful info to player." + eol + b"\tfunction UpdateNavInfo()"
if b.count(anchor2) != 1:
    sys.exit(f"rename-guard anchor not found in {p} ({b.count(anchor2)})")
b = b.replace(anchor2, helper)

n = b.count(b"SetObjectiveName(NavManager[x], ")
if n != 4:
    sys.exit(f"expected 4 nav rename call sites in {p}, found {n}")
b = b.replace(b"SetObjectiveName(NavManager[x], ", b"NavSetName(NavManager[x], ")

open(p, 'wb').write(b)
PY
  if command -v luac >/dev/null && ! luac -p "$f" >/dev/null 2>&1; then
    echo "SYNTAX ERROR after patching $f -- restoring" >&2
    cp "$f.orig" "$f"
    exit 1
  fi
  echo "patched   $f"
done

(( found )) || { echo "no installed copy of workshop $MOD found" >&2; exit 1; }
(( revert )) && echo "done -- originals restored" || cat <<'EOT'

done.  Now:
  1. launch with -netpktlog
  2. confirm BZLogger.txt contains "NAVFIX ACTIVE"  -- if it does not, the
     patch did not load and the run proves nothing
  3. place a nav ON A GEYSER, send an armory item to it, keep playing 2 min
  4. score with: tools/analyze_netpktlog.py BZLogger.txt --segments
EOT
