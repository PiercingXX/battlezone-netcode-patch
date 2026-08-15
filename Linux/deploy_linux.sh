#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GAME_ROOT="${1:-$PWD}"
PROXY_DIR="$SCRIPT_DIR/proton_dsound_proxy"
DLL_SRC="$PROXY_DIR/build/dsound.dll"
DLL_DST="$GAME_ROOT/dsound.dll"

if [[ ! -f "$GAME_ROOT/battlezone98redux.exe" ]]; then
  echo "Missing game executable in: $GAME_ROOT" >&2
  echo "Usage: $0 /path/to/Battlezone\ 98\ Redux" >&2
  exit 1
fi

if ! command -v i686-w64-mingw32-g++ >/dev/null 2>&1; then
  echo "Missing i686-w64-mingw32-g++ in PATH." >&2
  echo "Install a 32-bit MinGW toolchain first." >&2
  exit 2
fi

echo "Building Proton dsound proxy..."
(
  cd "$PROXY_DIR"
  make clean
  make
)

echo "Deploying dsound.dll to: $GAME_ROOT"
command cp -f "$DLL_SRC" "$DLL_DST"
rm -f "$GAME_ROOT/dsound_proxy.log"

# net.ini send-governor tuning.  The game only loads net.ini through the
# mod system - a copy in the game folder root is silently ignored - so it
# is installed as a local packaged mod.
NET_INI_SRC="$SCRIPT_DIR/../net-ini/net.ini"
if [[ -f "$NET_INI_SRC" ]]; then
  mkdir -p "$GAME_ROOT/packaged_mods/9990001"
  command cp -f "$NET_INI_SRC" "$GAME_ROOT/packaged_mods/9990001/net.ini"
  echo "Installed net.ini tuning mod to packaged_mods/9990001/"
fi

# The kernel silently clamps setsockopt to these limits; below the patch
# targets the enlarged socket buffers are mostly fictional under Proton.
rmem_max="$(sysctl -n net.core.rmem_max 2>/dev/null || echo 0)"
wmem_max="$(sysctl -n net.core.wmem_max 2>/dev/null || echo 0)"
if [[ "$rmem_max" -lt 4194304 || "$wmem_max" -lt 524288 ]]; then
  echo
  echo "WARNING: kernel UDP buffer limits are below the patch targets:" >&2
  echo "  net.core.rmem_max=$rmem_max (need >= 4194304)" >&2
  echo "  net.core.wmem_max=$wmem_max (need >= 524288)" >&2
  echo "Apply with:" >&2
  echo "  sudo sysctl -w net.core.rmem_max=4194304 net.core.wmem_max=524288" >&2
  echo "Persist across reboots with:" >&2
  echo "  printf 'net.core.rmem_max=4194304\\nnet.core.wmem_max=524288\\n' | sudo tee /etc/sysctl.d/99-battlezone-netcode.conf" >&2
fi

if [[ -x "$SCRIPT_DIR/repair_exu_linux.sh" ]]; then
  echo "Running Linux EXU compatibility repair (best effort)..."
  if ! "$SCRIPT_DIR/repair_exu_linux.sh" --game-path "$GAME_ROOT"; then
    echo "Warning: EXU compatibility repair failed; continuing with dsound patch deploy." >&2
  fi
fi

echo
echo "Deployment complete."
echo "Steam launch options should be:"
echo 'WINEDLLOVERRIDES=dsound=n,b %command% -nointro'
echo '(Bigger buffers, DSCP priority, the [Net] tuning poke, the governor cold-start'
echo ' fix and the host auto-kick relax are all on by default - nothing else to set.'
echo ' BZ_NET_TUNE=0 / BZ_AUTOKICK_RELAX=0 / BZ_GOV_START=0 opt out. The inbound'
echo ' reorder buffer is OFF since V4.8; see resources/BZ_P2P_HEADER.md for why it'
echo ' stays off.)'
echo '(After a match, check dsound_proxy.log for net_patch: / reorder_stats: /'
echo ' send_stats: lines, then run:'
echo '   tools/analyze_drops.py <game>/dsound_proxy.log <game>/BZLogger.txt)'
echo '(BZ_SEND_DUP=1 exists but is deprecated: live testing showed duplication does not'
echo ' help this game and degrades busy uplinks. Leave it off.)'