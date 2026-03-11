#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GAME_ROOT="${1:-$PWD}"

if [[ ! -f "$GAME_ROOT/battlezone98redux.exe" ]]; then
  echo "Missing game executable in: $GAME_ROOT" >&2
  echo "Usage: $0 /path/to/Battlezone\ 98\ Redux" >&2
  exit 1
fi

echo "Linux runtime test"
echo "Game root: $GAME_ROOT"
echo
echo "1) Launch the game from Steam and wait at the in-game main menu."
read -r -p "Press Enter when ready... "

"$SCRIPT_DIR/runtime_patch_linux.sh" "$GAME_ROOT"

echo
echo "2) In game, host or join one multiplayer session."
read -r -p "Press Enter after you have entered MP once... "

(
  cd "$GAME_ROOT"
  VERIFY_RUNTIME_ONLY=1 "$SCRIPT_DIR/verify_net_patch.sh"
)
