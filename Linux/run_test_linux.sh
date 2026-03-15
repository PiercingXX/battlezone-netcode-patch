#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GAME_ROOT="${1:-$PWD}"

if [[ ! -f "$GAME_ROOT/battlezone98redux.exe" ]]; then
  echo "Missing game executable in: $GAME_ROOT" >&2
  echo "Usage: $0 /path/to/Battlezone\ 98\ Redux" >&2
  exit 1
fi

echo "Linux Proton dsound proxy test"
echo "Game root: $GAME_ROOT"

"$SCRIPT_DIR/deploy_linux.sh" "$GAME_ROOT"

echo
echo "1) Confirm Steam launch options are set to:"
echo '   WINEDLLOVERRIDES="dsound=n,b" %command% -nointro'
read -r -p "Press Enter when launch options are ready... "

echo
echo "2) Launch the game from Steam, enter multiplayer once, then exit the game."
read -r -p "Press Enter after the run is complete... "

(
  cd "$GAME_ROOT"
  VERIFY_PROXY_READBACK=1 "$SCRIPT_DIR/verify_net_patch.sh"
)
