#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_GAME_ROOT="$HOME/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
GAME_ROOT="${1:-$DEFAULT_GAME_ROOT}"
PING_TARGET="${2:-1.1.1.1}"

if [[ ! -d "$GAME_ROOT" ]]; then
  echo "ERROR: game folder not found: $GAME_ROOT" >&2
  echo "Usage: $0 /path/to/Battlezone\ 98\ Redux [ping_target]" >&2
  exit 1
fi

state_root="$REPO_ROOT/test_bundles/deep_diag_state"
mkdir -p "$state_root"

if [[ -f "$state_root/linux_current_session.txt" ]]; then
  existing="$(cat "$state_root/linux_current_session.txt" || true)"
  if [[ -n "$existing" && -d "$existing" ]]; then
    echo "ERROR: deep diagnostics already running: $existing" >&2
    echo "Run Linux/stop_deep_diag.sh first." >&2
    exit 1
  fi
fi

utc_stamp="$(date -u +"%Y%m%dT%H%M%SZ")"
start_iso="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
host_name="$(hostname 2>/dev/null || echo unknown-host)"

session_dir="$REPO_ROOT/test_bundles/deep_linux_${host_name}_${utc_stamp}"
mkdir -p "$session_dir"

echo "$session_dir" >"$state_root/linux_current_session.txt"
echo "$GAME_ROOT" >"$session_dir/game_root.txt"
echo "$PING_TARGET" >"$session_dir/ping_target.txt"
echo "$start_iso" >"$session_dir/start_utc.txt"
touch "$session_dir/start.marker"

{
  echo "start_utc=$start_iso"
  echo "host_name=$host_name"
  echo "user_name=${USER:-unknown}"
  echo "game_root=$GAME_ROOT"
  echo "ping_target=$PING_TARGET"
  echo "kernel=$(uname -srmo 2>/dev/null || true)"
  echo "desktop_session=${XDG_CURRENT_DESKTOP:-unknown}"
  echo "steam_path=$(command -v steam || echo not-found)"
} >"$session_dir/session_info.txt"

{
  echo "# route"
  ip route 2>/dev/null || true
  echo
  echo "# ip addr"
  ip addr 2>/dev/null || true
} >"$session_dir/network_start.txt"

# Continuous ping latency/loss timeline.
(
  while true; do
    echo "===== $(date -u +"%Y-%m-%dT%H:%M:%SZ") ====="
    ping -n -c 1 -W 2 "$PING_TARGET" || true
    sleep 1
  done
) >"$session_dir/ping_timeline.log" 2>&1 &
ping_pid="$!"

# Periodic socket summary for connection health.
(
  while true; do
    echo "===== $(date -u +"%Y-%m-%dT%H:%M:%SZ") ====="
    ss -s || true
    echo
    ss -tupn || true
    echo
    sleep 5
  done
) >"$session_dir/socket_timeline.log" 2>&1 &
socket_pid="$!"

echo "$ping_pid" >"$session_dir/ping.pid"
echo "$socket_pid" >"$session_dir/socket.pid"

cat <<EOF
Deep diagnostics started.
Session dir: $session_dir
Ping target: $PING_TARGET

Next:
1) Run your test match.
2) If possible, set Steam launch options to include: PROTON_LOG=1 %command%
3) Run: $SCRIPT_DIR/stop_deep_diag.sh
EOF