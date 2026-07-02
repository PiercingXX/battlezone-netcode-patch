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
echo 'WINEDLLOVERRIDES="dsound=n,b" %command% -nointro'