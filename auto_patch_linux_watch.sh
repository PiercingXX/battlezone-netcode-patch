#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GAME_ROOT="${1:-$PWD}"
EXE_PATH="$GAME_ROOT/battlezone98redux.exe"
POLL_SECS="${RUNTIME_WATCH_POLL_SECS:-2}"

if [[ ! -f "$EXE_PATH" ]]; then
  echo "Missing executable: $EXE_PATH" >&2
  echo "Usage: $0 /path/to/Battlezone\ 98\ Redux" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  echo "Missing dependency: python3" >&2
  exit 1
fi

if [[ ! -f "$SCRIPT_DIR/runtime_patch_linux.py" ]]; then
  echo "Missing helper script: $SCRIPT_DIR/runtime_patch_linux.py" >&2
  exit 1
fi

echo "Linux auto patch watch started."
echo "Game root: $GAME_ROOT"
echo "Polling every ${POLL_SECS}s. Press Ctrl+C to stop."

# Track PIDs already patched so each launch gets patched once.
declare -A patched

while true; do
  pids="$(pgrep -f 'Battlezone98Redux\.exe' || true)"

  # Clean old PID markers.
  for seen_pid in "${!patched[@]}"; do
    if ! kill -0 "$seen_pid" 2>/dev/null; then
      unset 'patched[$seen_pid]'
    fi
  done

  if [[ -n "$pids" ]]; then
    while IFS= read -r pid; do
      [[ -z "$pid" ]] && continue
      if [[ -n "${patched[$pid]:-}" ]]; then
        continue
      fi

      echo "Detected PID $pid. Attempting runtime patch..."
      if python3 "$SCRIPT_DIR/runtime_patch_linux.py" --pid "$pid"; then
        patched[$pid]=1
        echo "PID $pid patched successfully."
      else
        rc=$?
        echo "Patch attempt failed for PID $pid (exit $rc). Will retry."
      fi
    done <<< "$pids"
  fi

  sleep "$POLL_SECS"
done
