#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GAME_ROOT="${1:-$PWD}"
APP_ID="${BATTLEZONE_APP_ID:-301650}"
PATCH_TIMEOUT="${RUNTIME_PATCH_TIMEOUT_SECS:-180}"

if [[ ! -f "$GAME_ROOT/battlezone98redux.exe" ]]; then
  echo "Missing executable: $GAME_ROOT/battlezone98redux.exe" >&2
  echo "Usage: $0 /path/to/Battlezone\ 98\ Redux" >&2
  exit 1
fi

if ! command -v steam >/dev/null 2>&1; then
  echo "Steam CLI not found in PATH. Launch Steam manually, then run runtime_patch_linux.sh." >&2
  exit 1
fi

echo "Launching Battlezone 98 Redux via Steam (AppID: $APP_ID)..."
steam -applaunch "$APP_ID" >/dev/null 2>&1 &

# Give Steam a brief moment to hand off to Proton/game process.
sleep 2

echo "Waiting for game process, then applying runtime patch..."
RUNTIME_PATCH_TIMEOUT_SECS="$PATCH_TIMEOUT" "$SCRIPT_DIR/runtime_patch_linux.sh" "$GAME_ROOT"
rc=$?

if [[ "$rc" -eq 0 ]]; then
  echo "Patch complete. Enter multiplayer once, then verify if needed:"
  echo "  cd \"$GAME_ROOT\" && VERIFY_RUNTIME_ONLY=1 \"$SCRIPT_DIR/verify_net_patch.sh\""
fi

exit "$rc"
