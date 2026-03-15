#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPLOY_SCRIPT="$REPO_ROOT/Linux/deploy_linux.sh"

if [[ ! -x "$DEPLOY_SCRIPT" ]]; then
  echo "Missing deploy script: $DEPLOY_SCRIPT" >&2
  exit 1
fi

GAME_PATH="${1:-}"

if [[ -z "$GAME_PATH" ]]; then
  CANDIDATES=(
    "$HOME/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
    "$HOME/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
    "$HOME/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
  )

  for c in "${CANDIDATES[@]}"; do
    if [[ -f "$c/battlezone98redux.exe" ]]; then
      GAME_PATH="$c"
      break
    fi
  done
fi

if [[ -z "$GAME_PATH" ]]; then
  echo "Could not auto-detect Battlezone 98 Redux path." >&2
  echo "Usage:" >&2
  echo "  $0 \"/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux\"" >&2
  exit 2
fi

if [[ ! -f "$GAME_PATH/battlezone98redux.exe" ]]; then
  echo "Invalid game path: $GAME_PATH" >&2
  echo "Missing file: $GAME_PATH/battlezone98redux.exe" >&2
  exit 3
fi

echo "Deploying patched dsound.dll to: $GAME_PATH"
"$DEPLOY_SCRIPT" "$GAME_PATH"

echo
echo "Ready for buffer logging. Use this Steam launch option:"
echo 'WINEDLLOVERRIDES="dsound=n,b" BZ_BUFFER_LOG=1 BZ_BUFFER_LOG_BYTES=32 BZ_BUFFER_LOG_RING=65536 %command% -nointro'
