#!/usr/bin/env bash
set -euo pipefail

GAME_ROOT="${1:-$PWD}"
EXE_NAME="battlezone98redux.exe"
EXE_PATH="$(readlink -f "$GAME_ROOT/$EXE_NAME")"
TIMEOUT_SECS="${RUNTIME_PATCH_TIMEOUT_SECS:-60}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PY_HELPER="$SCRIPT_DIR/runtime_patch_linux.py"

if [[ ! -f "$EXE_PATH" ]]; then
  echo "Missing executable: $EXE_PATH" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "Missing dependency: python3" >&2
  exit 1
fi

if [[ ! -f "$PY_HELPER" ]]; then
  echo "Missing helper script: $PY_HELPER" >&2
  exit 1
fi

find_pid() {
  pgrep -f 'Battlezone98Redux\.exe' | head -n 1 || true
}

start_ts=$(date +%s)
echo "Waiting for running process: Battlezone98Redux.exe"
while true; do
  PID="$(find_pid)"
  if [[ -n "$PID" ]]; then
    break
  fi
  now=$(date +%s)
  if (( now - start_ts >= TIMEOUT_SECS )); then
    echo "Timed out after ${TIMEOUT_SECS}s waiting for running process: Battlezone98Redux.exe" >&2
    echo "Start the game first, wait at main menu, then rerun this script." >&2
    exit 2
  fi
  sleep 1
done

python3 "$PY_HELPER"
rc=$?
if [[ "$rc" -eq 0 ]]; then
  echo "Expected in fresh log after networking init:"
  echo "BZRNet P2P Socket Opened With 2097152 received buffer, 524288 send buffer"
fi
exit "$rc"
