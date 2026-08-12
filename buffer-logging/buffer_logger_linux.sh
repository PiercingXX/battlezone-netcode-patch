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
  ./buffer-logging/buffer_logger_linux.sh start "/path/to/game" 32 65536 "203.0.113.44:37218"

Notes:
  - This is a lightweight collector for the new buffer logger only.
  - It does NOT run the heavy deep diagnostics tooling.
  - The proxy must implement bz_buffer_log.bin generation for packet records to appear.
EOF
}

# Both committed bundles are named "unknown-host" because this used only
# `hostname`, which is not installed by default on Arch (and several other
# distros ship it only in net-tools). Every other source was right there.
resolve_hostname() {
  local h=""
  if command -v hostnamectl >/dev/null 2>&1; then
    h="$(hostnamectl --static 2>/dev/null || true)"
  fi
  if [[ -z "$h" ]] && command -v hostname >/dev/null 2>&1; then
    h="$(hostname 2>/dev/null || true)"
  fi
  if [[ -z "$h" && -r /etc/hostname ]]; then
    h="$(tr -d '\n' </etc/hostname 2>/dev/null || true)"
  fi
  if [[ -z "$h" && -r /proc/sys/kernel/hostname ]]; then
    h="$(tr -d '\n' </proc/sys/kernel/hostname 2>/dev/null || true)"
  fi
  [[ -z "$h" ]] && h="${HOSTNAME:-}"
  [[ -z "$h" ]] && h="unknown-host"
  # Keep it safe as a path component.
  printf '%s' "$h" | tr -c 'A-Za-z0-9._-' '-'
}

# Did the capture actually run with the settings we asked for?
#
# The one successful capture of 2026-07-26 asked for BZ_BUFFER_LOG_RING=500000
# and ran with 65,536 -- the default -- discarding 48% of its events including
# the entire match start. Nothing said so, and it took reading two files side
# by side days later to notice. The proxy now records what it was asked for in
# the meta file; this compares that against what the tester requested and
# refuses to be quiet about a mismatch, while they can still re-run it.
verify_capture() {
  local session_dir="$1"
  local meta="$session_dir/bz_buffer_log.meta.txt"
  local report="$session_dir/capture_verify.txt"
  local ok=1

  {
    if [[ ! -s "$meta" ]]; then
      echo "meta=MISSING"
      echo "The game did not flush the ring. An unclean exit never writes it."
      ok=0
    else
      # The proxy writes literal CRLF; normalise before matching.
      local want_ring want_bytes got_ring got_bytes ring_env seen wrote
      want_ring="$(cat "$session_dir/ring_records.txt" 2>/dev/null | tr -d '\r\n')"
      want_bytes="$(cat "$session_dir/payload_bytes.txt" 2>/dev/null | tr -d '\r\n')"
      got_ring="$(tr -d '\r' <"$meta" | sed -n 's/^ring_records=//p' | head -1)"
      got_bytes="$(tr -d '\r' <"$meta" | sed -n 's/^payload_bytes=//p' | head -1)"
      ring_env="$(tr -d '\r' <"$meta" | sed -n 's/^ring_env=//p' | head -1)"
      seen="$(tr -d '\r' <"$meta" | sed -n 's/^total_events_seen=//p' | head -1)"
      wrote="$(tr -d '\r' <"$meta" | sed -n 's/^records_written=//p' | head -1)"

      echo "requested_ring=$want_ring effective_ring=${got_ring:-unknown}"
      echo "requested_payload=$want_bytes effective_payload=${got_bytes:-unknown}"
      [[ -n "$ring_env" ]] && echo "ring_env=$ring_env"

      if [[ -n "$want_ring" && -n "$got_ring" && "$want_ring" != "$got_ring" ]]; then
        echo "MISMATCH: ring"
        ok=0
      fi
      if [[ -n "$want_bytes" && -n "$got_bytes" && "$want_bytes" != "$got_bytes" ]]; then
        echo "MISMATCH: payload"
        ok=0
      fi
      if [[ -n "$seen" && -n "$wrote" && "$seen" -gt "$wrote" ]]; then
        echo "ring wrapped: $seen events seen, last $wrote kept ($(( (seen - wrote) * 100 / seen ))% discarded)"
        ok=0
      fi
    fi
    echo "ok=$ok"
  } >"$report" 2>&1

  if [[ "$ok" == "1" ]]; then
    echo
    echo "  capture verified: ran with the settings you asked for, ring did not wrap"
    return 0
  fi

  echo
  echo "  ####################################################################"
  echo "  #  CAPTURE IS NOT CLEAN - read it before you rely on it            #"
  echo "  ####################################################################"
  sed 's/^/    /' "$report"
  echo
  echo "    A ring that wrapped, or settings that did not take, means the"
  echo "    capture is missing events - most likely the earliest ones, which"
  echo "    is where the match start lives."
  echo "    If ring_env says NOT SET, the game was launched before the launch"
  echo "    options were pasted. Close the game, paste them, run this again."
  echo
  return 0
}

write_launch_options() {
  local out_file="$1"
  local payload_bytes="$2"
  local ring_records="$3"
  local peer_filter="$4"

  # This file must contain the paste-ready line and NOTHING else.  It used to
  # carry a human-readable header above the line, and a select-all paste then
  # fed Steam the header too.  Explanation lives in README_NEXT_STEPS.txt.
  {
    printf 'WINEDLLOVERRIDES=dsound=n,b BZ_BUFFER_LOG=1 BZ_BUFFER_LOG_BYTES=%s BZ_BUFFER_LOG_RING=%s' "$payload_bytes" "$ring_records"
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
  host="$(resolve_hostname)"
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
1. Paste the WHOLE single line from launch_options.txt into the Steam launch
   options for Battlezone 98 Redux, replacing what is there.
2. Start Battlezone 98 Redux.
3. Reproduce the packet-order issue.
4. Exit the game normally. An unclean exit never flushes the ring buffer and
   bz_buffer_log.bin will be missing or empty.
5. Run ./buffer-logging/buffer_logger_linux.sh stop

ORDER MATTERS. Everything before %command% is an environment variable;
everything after it is an argument handed to the game. Put BZ_BUFFER_LOG=1
after %command% and the game tries to open it as a mission file and stops
with: Could not load "BZ_BUFFER_LOG=1".

Expected lightweight outputs from the game folder:
- dsound_proxy.log
- bz_buffer_log.bin
- bz_buffer_log.meta.txt
- BZLogger.txt

Decode the capture with:
  python3 buffer-logging/decode_buffer_log.py <bundle>/bz_buffer_log.bin --seq-scan
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

  verify_capture "$session_dir"

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