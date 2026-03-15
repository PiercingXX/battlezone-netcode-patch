#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
STATE_ROOT="$REPO_ROOT/test_bundles/buffer_log_state"
CURRENT_FILE="$STATE_ROOT/linux_current_session.txt"
DEFAULT_GAME_ROOT="$HOME/.local/share/Steam/steamapps/common/Battlezone 98 Redux"

mkdir -p "$STATE_ROOT"

usage() {
  cat <<'EOF'
Usage:
  ./buffer-logging/buffer_logger_linux.sh start [game_path] [payload_bytes] [ring_records] [peer_filter]
  ./buffer-logging/buffer_logger_linux.sh stop

Examples:
  ./buffer-logging/buffer_logger_linux.sh start
  ./buffer-logging/buffer_logger_linux.sh start "/home/$USER/.local/share/Steam/steamapps/common/Battlezone 98 Redux" 32 65536
  ./buffer-logging/buffer_logger_linux.sh start "/path/to/game" 32 65536 "73.200.253.44:37218"

Notes:
  - This is a lightweight collector for the new buffer logger only.
  - It does NOT run the heavy deep diagnostics tooling.
  - The proxy must implement bz_buffer_log.bin generation for packet records to appear.
EOF
}

write_launch_options() {
  local out_file="$1"
  local payload_bytes="$2"
  local ring_records="$3"
  local peer_filter="$4"

  {
    echo "Linux Steam launch options for buffer logging:"
    echo
    printf 'WINEDLLOVERRIDES="dsound=n,b" BZ_BUFFER_LOG=1 BZ_BUFFER_LOG_BYTES=%s BZ_BUFFER_LOG_RING=%s' "$payload_bytes" "$ring_records"
    if [[ -n "$peer_filter" ]]; then
      printf ' BZ_BUFFER_LOG_PEER="%s"' "$peer_filter"
    fi
    echo ' %command% -nointro'
  } >"$out_file"
}

collect_file() {
  local src="$1"
  local dst_dir="$2"
  local status_file="$3"
  local base
  base="$(basename "$src")"
  if [[ -f "$src" ]]; then
    cp -f "$src" "$dst_dir/$base"
    printf 'found %s bytes=%s\n' "$base" "$(stat -c%s "$src" 2>/dev/null || echo 0)" >>"$status_file"
  else
    printf 'missing %s\n' "$base" >>"$status_file"
  fi
}

start_session() {
  local game_root="${1:-$DEFAULT_GAME_ROOT}"
  local payload_bytes="${2:-32}"
  local ring_records="${3:-65536}"
  local peer_filter="${4:-}"

  if [[ -f "$CURRENT_FILE" ]]; then
    local existing
    existing="$(cat "$CURRENT_FILE" 2>/dev/null || true)"
    if [[ -n "$existing" && -d "$existing" ]]; then
      echo "ERROR: buffer logging session already active: $existing" >&2
      exit 1
    fi
  fi

  if [[ ! -d "$game_root" ]]; then
    echo "ERROR: game folder not found: $game_root" >&2
    exit 1
  fi

  local host utc_stamp session_dir
  host="$(hostname 2>/dev/null || echo unknown-host)"
  utc_stamp="$(date -u +%Y%m%dT%H%M%SZ)"
  session_dir="$REPO_ROOT/test_bundles/buffer_linux_${host}_${utc_stamp}"
  mkdir -p "$session_dir"

  printf '%s\n' "$session_dir" >"$CURRENT_FILE"
  printf '%s\n' "$game_root" >"$session_dir/game_path.txt"
  printf '%s\n' "$payload_bytes" >"$session_dir/payload_bytes.txt"
  printf '%s\n' "$ring_records" >"$session_dir/ring_records.txt"
  printf '%s\n' "$peer_filter" >"$session_dir/peer_filter.txt"
  date -u +%Y-%m-%dT%H:%M:%SZ >"$session_dir/start_utc.txt"

  write_launch_options "$session_dir/launch_options.txt" "$payload_bytes" "$ring_records" "$peer_filter"

  cat >"$session_dir/README_NEXT_STEPS.txt" <<EOF
1. Copy the Steam launch options from launch_options.txt.
2. Start Battlezone 98 Redux.
3. Reproduce the packet-order issue.
4. Run ./buffer-logging/buffer_logger_linux.sh stop

Expected lightweight outputs from the game folder:
- dsound_proxy.log
- bz_buffer_log.bin
- bz_buffer_log.meta.txt
- BZLogger.txt
EOF

  echo "Buffer logging session started."
  echo "Session dir: $session_dir"
  echo "Launch options saved to: $session_dir/launch_options.txt"
}

stop_session() {
  if [[ ! -f "$CURRENT_FILE" ]]; then
    echo "ERROR: no active Linux buffer logging session found." >&2
    exit 1
  fi

  local session_dir game_root status_file archive_path
  session_dir="$(cat "$CURRENT_FILE")"
  if [[ -z "$session_dir" || ! -d "$session_dir" ]]; then
    echo "ERROR: session directory missing: $session_dir" >&2
    rm -f "$CURRENT_FILE"
    exit 1
  fi

  game_root="$(cat "$session_dir/game_path.txt" 2>/dev/null || true)"
  status_file="$session_dir/collection_status.txt"
  : >"$status_file"

  collect_file "$game_root/BZLogger.txt" "$session_dir" "$status_file"
  collect_file "$game_root/dsound_proxy.log" "$session_dir" "$status_file"
  collect_file "$game_root/bz_buffer_log.bin" "$session_dir" "$status_file"
  collect_file "$game_root/bz_buffer_log.meta.txt" "$session_dir" "$status_file"
  collect_file "$game_root/multi.ini" "$session_dir" "$status_file"

  archive_path="$session_dir.tar.gz"
  tar -czf "$archive_path" -C "$(dirname "$session_dir")" "$(basename "$session_dir")"

  rm -f "$CURRENT_FILE"
  echo "Buffer logging session stopped."
  echo "Bundle directory: $session_dir"
  echo "Archive created: $archive_path"
}

main() {
  if [[ $# -lt 1 ]]; then
    usage
    exit 1
  fi

  case "$1" in
    start)
      shift
      start_session "$@"
      ;;
    stop)
      stop_session
      ;;
    -h|--help|help)
      usage
      ;;
    *)
      usage
      exit 1
      ;;
  esac
}

main "$@"