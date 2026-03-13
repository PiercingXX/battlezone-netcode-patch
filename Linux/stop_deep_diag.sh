#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
state_root="$REPO_ROOT/test_bundles/deep_diag_state"
current_file="$state_root/linux_current_session.txt"

if [[ ! -f "$current_file" ]]; then
  echo "ERROR: no active Linux deep diagnostics session found." >&2
  echo "Run Linux/start_deep_diag.sh first." >&2
  exit 1
fi

session_dir="$(cat "$current_file")"
if [[ -z "$session_dir" || ! -d "$session_dir" ]]; then
  echo "ERROR: session directory missing: $session_dir" >&2
  rm -f "$current_file"
  exit 1
fi

game_root="$(cat "$session_dir/game_root.txt" 2>/dev/null || true)"
start_iso="$(cat "$session_dir/start_utc.txt" 2>/dev/null || true)"

stop_pid_file() {
  local pid_file="$1"
  if [[ -f "$pid_file" ]]; then
    local pid
    pid="$(cat "$pid_file" 2>/dev/null || true)"
    if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      sleep 1
      kill -9 "$pid" 2>/dev/null || true
    fi
  fi
}

stop_pid_file "$session_dir/ping.pid"
stop_pid_file "$session_dir/socket.pid"

{
  echo "# route"
  ip route 2>/dev/null || true
  echo
  echo "# ip addr"
  ip addr 2>/dev/null || true
} >"$session_dir/network_end.txt"

if [[ -n "$game_root" && -d "$game_root" ]]; then
  for f in BZLogger.txt dsound_proxy.log winmm_proxy.log multi.ini; do
    if [[ -f "$game_root/$f" ]]; then
      cp -f "$game_root/$f" "$session_dir/$f"
    fi
  done
fi

# Capture Proton logs generated in home folder during this session.
if [[ -f "$session_dir/start.marker" ]]; then
  while IFS= read -r -d '' log_file; do
    cp -f "$log_file" "$session_dir/$(basename "$log_file")"
  done < <(find "$HOME" -maxdepth 1 -type f -name 'steam-*.log' -newer "$session_dir/start.marker" -print0 2>/dev/null)
fi

# Capture a focused journal window for session timing correlation.
if command -v journalctl >/dev/null 2>&1; then
  if [[ -n "$start_iso" ]]; then
    journalctl --since "$start_iso" --no-pager >"$session_dir/journal_since_start.log" 2>/dev/null || true
  else
    journalctl -n 2000 --no-pager >"$session_dir/journal_since_start.log" 2>/dev/null || true
  fi
fi

# Capture coredump metadata when available.
if command -v coredumpctl >/dev/null 2>&1; then
  coredumpctl list --no-pager >"$session_dir/coredumps_list.txt" 2>/dev/null || true
  coredumpctl info --no-pager >"$session_dir/coredumps_info.txt" 2>/dev/null || true
fi

# Run verifier once more at stop time to preserve current patch state.
verify_log="$session_dir/verify_output.txt"
{
  echo "# verify command"
  echo "VERIFY_PROXY_READBACK=1 '$SCRIPT_DIR/verify_net_patch.sh'"
  echo
  VERIFY_PROXY_READBACK=1 "$SCRIPT_DIR/verify_net_patch.sh"
} >"$verify_log" 2>&1 || true

archive_path="${session_dir}.tar.gz"
tar -czf "$archive_path" -C "$(dirname "$session_dir")" "$(basename "$session_dir")"

rm -f "$current_file"

echo "Deep diagnostics stopped."
echo "Bundle created: $archive_path"
echo "Send this file back to the test coordinator."