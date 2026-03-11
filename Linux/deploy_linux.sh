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

echo
echo "Deployment complete."
echo "Steam launch options should be:"
echo 'WINEDLLOVERRIDES="dsound=n,b" %command% -nointro'