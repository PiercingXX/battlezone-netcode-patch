#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_GAME_ROOT="$HOME/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
STATE_ROOT="$REPO_ROOT/test_bundles/deep_diag_state"
CURRENT_FILE="$STATE_ROOT/linux_current_session.txt"

mkdir -p "$STATE_ROOT"

usage() {
  cat <<'EOF'
Usage:
  ./Linux/tester_diag.sh start /path/to/Battlezone\ 98\ Redux [baseline_ping_target] [peer_ping_target]
  ./Linux/tester_diag.sh stop
  ./Linux/tester_diag.sh mark "short event note"

Notes:
  - start defaults baseline_ping_target to 1.1.1.1
  - peer_ping_target is optional; if omitted, the script will try to infer likely peer IPs from captured traffic
EOF
}

log_step() {
  echo "[tester_diag] $1"
}

run_with_timeout() {
  local seconds="$1"
  shift
  if command -v timeout >/dev/null 2>&1; then
    timeout "$seconds" "$@" || true
  else
    "$@" || true
  fi
}

is_public_ipv4() {
  local ip="$1"
  [[ "$ip" =~ ^([0-9]{1,3}\.){3}[0-9]{1,3}$ ]] || return 1
  case "$ip" in
    0.*|10.*|127.*|169.254.*|192.168.*|224.*|225.*|226.*|227.*|228.*|229.*|230.*|231.*|232.*|233.*|234.*|235.*|236.*|237.*|238.*|239.*)
      return 1
      ;;
  esac
  if [[ "$ip" =~ ^172\.([1][6-9]|2[0-9]|3[0-1])\. ]]; then
    return 1
  fi
  if [[ "$ip" =~ ^100\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])\. ]]; then
    return 1
  fi
  return 0
}

extract_peer_candidates_linux() {
  local socket_log="$1"
  local baseline_target="$2"
  local out_file="$3"
  if [[ ! -f "$socket_log" ]]; then
    : >"$out_file"
    return
  fi
  awk '
    {
      while (match($0, /([0-9]{1,3}\.){3}[0-9]{1,3}:[0-9*]+/)) {
        token = substr($0, RSTART, RLENGTH)
        split(token, parts, ":")
        print parts[1]
        $0 = substr($0, RSTART + RLENGTH)
      }
    }
  ' "$socket_log" |
    grep -v -F "$baseline_target" |
    sort | uniq -c | sort -nr |
    while read -r count ip; do
      if is_public_ipv4 "$ip"; then
        printf '%s %s\n' "$count" "$ip"
      fi
    done >"$out_file"
}

capture_route_snapshot() {
  local target="$1"
  local out_file="$2"
  {
    echo "target=$target"
    if command -v tracepath >/dev/null 2>&1; then
      tracepath -n "$target" || true
    elif command -v traceroute >/dev/null 2>&1; then
      traceroute -n -w 1 -q 1 "$target" || true
    else
      echo "tracepath/traceroute not found"
    fi
  } >"$out_file"
}

capture_interface_stats() {
  local out_file="$1"
  ip -s link 2>/dev/null >"$out_file" || true
}

capture_udp_queue_snapshot() {
  local out_file="$1"
  {
    echo "===== $(date -u +"%Y-%m-%dT%H:%M:%SZ") ====="
    if command -v tc >/dev/null 2>&1; then
      echo "# tc -s qdisc"
      tc -s qdisc 2>/dev/null || true
      echo
    fi
    echo "# ss -u -i"
    ss -u -i 2>/dev/null || true
    echo
  } >"$out_file"
}

capture_noise_profile() {
  local out_file="$1"
  local default_iface="$2"
  {
    echo "default_iface=$default_iface"
    echo "proxy_http_set=$( [[ -n "${http_proxy:-}${HTTP_PROXY:-}" ]] && echo 1 || echo 0 )"
    echo "proxy_https_set=$( [[ -n "${https_proxy:-}${HTTPS_PROXY:-}" ]] && echo 1 || echo 0 )"
    echo "proxy_no_proxy_set=$( [[ -n "${no_proxy:-}${NO_PROXY:-}" ]] && echo 1 || echo 0 )"
    echo "vpn_iface_count=$(ip -o link show 2>/dev/null | awk -F': ' '{print $2}' | grep -E '^(tun|tap|wg|ppp)' | wc -l || true)"
    if command -v nmcli >/dev/null 2>&1; then
      echo "nmcli_connection_type=$(nmcli -t -f DEVICE,TYPE device status 2>/dev/null | awk -F: -v dev="$default_iface" '$1==dev{print $2; exit}')"
    fi
    if [[ -n "$default_iface" ]] && command -v iw >/dev/null 2>&1; then
      echo "wifi_link_info_begin"
      iw dev "$default_iface" link 2>/dev/null || true
      echo "wifi_link_info_end"
    fi
  } >"$out_file"
}

capture_rate_sample() {
  local iface="$1"
  local out_file="$2"
  local rx1 tx1 rx2 tx2
  rx1=$(cat "/sys/class/net/$iface/statistics/rx_bytes" 2>/dev/null || echo 0)
  tx1=$(cat "/sys/class/net/$iface/statistics/tx_bytes" 2>/dev/null || echo 0)
  sleep 5
  rx2=$(cat "/sys/class/net/$iface/statistics/rx_bytes" 2>/dev/null || echo 0)
  tx2=$(cat "/sys/class/net/$iface/statistics/tx_bytes" 2>/dev/null || echo 0)
  {
    echo "window_seconds=5"
    echo "rx_bytes_start=$rx1"
    echo "rx_bytes_end=$rx2"
    echo "tx_bytes_start=$tx1"
    echo "tx_bytes_end=$tx2"
    echo "rx_bytes_per_sec=$(( (rx2-rx1)/5 ))"
    echo "tx_bytes_per_sec=$(( (tx2-tx1)/5 ))"
  } >"$out_file"
}

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

start_diag() {
  local game_root="${1:-$DEFAULT_GAME_ROOT}"
  local baseline_target="${2:-1.1.1.1}"
  local peer_target="${3:-}"

  if [[ ! -d "$game_root" ]]; then
    echo "ERROR: game folder not found: $game_root" >&2
    usage
    exit 1
  fi
  if [[ -f "$CURRENT_FILE" ]]; then
    local existing
    existing="$(cat "$CURRENT_FILE" || true)"
    if [[ -n "$existing" && -d "$existing" ]]; then
      echo "ERROR: deep diagnostics already running: $existing" >&2
      echo "Run ./Linux/tester_diag.sh stop first." >&2
      exit 1
    fi
  fi

  local utc_stamp start_iso host_name session_dir default_iface
  utc_stamp="$(date -u +"%Y%m%dT%H%M%SZ")"
  start_iso="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
  host_name="$(hostname 2>/dev/null || echo unknown-host)"
  session_dir="$REPO_ROOT/test_bundles/deep_linux_${host_name}_${utc_stamp}"
  default_iface="$(ip route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="dev") {print $(i+1); exit}}')"
  mkdir -p "$session_dir"

  echo "$session_dir" >"$CURRENT_FILE"
  echo "$game_root" >"$session_dir/game_root.txt"
  echo "$baseline_target" >"$session_dir/ping_target.txt"
  echo "$peer_target" >"$session_dir/peer_target.txt"
  echo "$start_iso" >"$session_dir/start_utc.txt"
  touch "$session_dir/start.marker"

  {
    echo "start_utc=$start_iso"
    echo "host_name=$host_name"
    echo "user_name=${USER:-unknown}"
    echo "game_root=$game_root"
    echo "ping_target=$baseline_target"
    echo "peer_ping_target=$peer_target"
    echo "peer_detection_mode=$( [[ -n "$peer_target" ]] && echo explicit || echo auto )"
    echo "kernel=$(uname -srmo 2>/dev/null || true)"
    echo "desktop_session=${XDG_CURRENT_DESKTOP:-unknown}"
    echo "steam_path=$(command -v steam || echo not-found)"
    echo "default_iface=$default_iface"
  } >"$session_dir/session_info.txt"

  {
    echo "# route"
    ip route 2>/dev/null || true
    echo
    echo "# ip addr"
    ip addr 2>/dev/null || true
  } >"$session_dir/network_start.txt"

  if [[ -n "$peer_target" ]]; then
    capture_route_snapshot "$peer_target" "$session_dir/route_peer_start.txt"
  fi
  capture_route_snapshot "$baseline_target" "$session_dir/route_baseline_start.txt"
  capture_interface_stats "$session_dir/interface_stats_start.txt"
  capture_udp_queue_snapshot "$session_dir/udp_qdisc_start.txt"
  capture_noise_profile "$session_dir/noise_profile_start.txt" "$default_iface"
  if [[ -n "$default_iface" ]]; then
    capture_rate_sample "$default_iface" "$session_dir/net_rate_start.txt"
  fi

  (
    while true; do
      echo "===== $(date -u +"%Y-%m-%dT%H:%M:%SZ") ====="
      ping -n -c 1 -W 2 "$baseline_target" || true
      sleep 1
    done
  ) >"$session_dir/ping_timeline.log" 2>&1 &
  echo "$!" >"$session_dir/ping.pid"

  if [[ -n "$peer_target" ]]; then
    (
      while true; do
        echo "===== $(date -u +"%Y-%m-%dT%H:%M:%SZ") ====="
        ping -n -c 1 -W 2 "$peer_target" || true
        sleep 1
      done
    ) >"$session_dir/peer_ping_timeline.log" 2>&1 &
    echo "$!" >"$session_dir/peer_ping.pid"
  fi

  if [[ -n "$peer_target" ]] && command -v mtr >/dev/null 2>&1; then
    (
      while true; do
        echo "===== $(date -u +"%Y-%m-%dT%H:%M:%SZ") ====="
        mtr -n -r -c 20 "$peer_target" || true
        sleep 15
      done
    ) >"$session_dir/mtr_peer_timeline.log" 2>&1 &
    echo "$!" >"$session_dir/mtr_peer.pid"

    (
      while true; do
        echo "===== $(date -u +"%Y-%m-%dT%H:%M:%SZ") ====="
        mtr -n -r -c 20 "$baseline_target" || true
        sleep 15
      done
    ) >"$session_dir/mtr_baseline_timeline.log" 2>&1 &
    echo "$!" >"$session_dir/mtr_baseline.pid"
  elif command -v mtr >/dev/null 2>&1; then
    (
      while true; do
        echo "===== $(date -u +"%Y-%m-%dT%H:%M:%SZ") ====="
        mtr -n -r -c 20 "$baseline_target" || true
        sleep 15
      done
    ) >"$session_dir/mtr_baseline_timeline.log" 2>&1 &
    echo "$!" >"$session_dir/mtr_baseline.pid"
  else
    echo "mtr_not_found=1" >"$session_dir/mtr_status.txt"
  fi

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
  echo "$!" >"$session_dir/socket.pid"

  if [[ -n "$default_iface" ]]; then
    (
      while true; do
        local_ts="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
        rx1=$(cat "/sys/class/net/$default_iface/statistics/rx_bytes" 2>/dev/null || echo 0)
        tx1=$(cat "/sys/class/net/$default_iface/statistics/tx_bytes" 2>/dev/null || echo 0)
        sleep 2
        rx2=$(cat "/sys/class/net/$default_iface/statistics/rx_bytes" 2>/dev/null || echo 0)
        tx2=$(cat "/sys/class/net/$default_iface/statistics/tx_bytes" 2>/dev/null || echo 0)
        echo "$local_ts iface=$default_iface rx_Bps=$(( (rx2-rx1)/2 )) tx_Bps=$(( (tx2-tx1)/2 ))"
      done
    ) >"$session_dir/net_rate_timeline.log" 2>&1 &
    echo "$!" >"$session_dir/netrate.pid"
  fi

  cat <<EOF
Deep diagnostics started.
Session dir: $session_dir
Baseline ping target: $baseline_target
Peer ping target: ${peer_target:-auto-detect}

Next:
1) Run your test match.
2) If possible, set Steam launch options to include: PROTON_LOG=1 WINEDLLOVERRIDES="dsound=n,b" %command% -nointro
3) Optional marker during lag spike: ./Linux/tester_diag.sh mark "lag spike while joining"
4) After the game is finished run: ./Linux/tester_diag.sh stop
EOF
}

stop_diag() {
  if [[ ! -f "$CURRENT_FILE" ]]; then
    echo "ERROR: no active Linux deep diagnostics session found." >&2
    echo "Run ./Linux/tester_diag.sh start first." >&2
    exit 1
  fi

  local session_dir game_root start_iso peer_target baseline_target default_iface verify_log archive_path
  session_dir="$(cat "$CURRENT_FILE")"
  if [[ -z "$session_dir" || ! -d "$session_dir" ]]; then
    echo "ERROR: session directory missing: $session_dir" >&2
    rm -f "$CURRENT_FILE"
    exit 1
  fi

  game_root="$(cat "$session_dir/game_root.txt" 2>/dev/null || true)"
  start_iso="$(cat "$session_dir/start_utc.txt" 2>/dev/null || true)"
  peer_target="$(cat "$session_dir/peer_target.txt" 2>/dev/null || true)"
  baseline_target="$(cat "$session_dir/ping_target.txt" 2>/dev/null || true)"
  default_iface="$(grep -E '^default_iface=' "$session_dir/noise_profile_start.txt" 2>/dev/null | cut -d= -f2- || true)"

  log_step "Stopping background collectors"
  stop_pid_file "$session_dir/ping.pid"
  stop_pid_file "$session_dir/peer_ping.pid"
  stop_pid_file "$session_dir/mtr_peer.pid"
  stop_pid_file "$session_dir/mtr_baseline.pid"
  stop_pid_file "$session_dir/netrate.pid"
  stop_pid_file "$session_dir/socket.pid"

  log_step "Capturing end-of-session network snapshot"
  {
    echo "# route"
    ip route 2>/dev/null || true
    echo
    echo "# ip addr"
    ip addr 2>/dev/null || true
  } >"$session_dir/network_end.txt"

  log_step "Inferring likely peer candidates"
  extract_peer_candidates_linux "$session_dir/socket_timeline.log" "$baseline_target" "$session_dir/peer_candidates.txt"
  if [[ -z "$peer_target" && -s "$session_dir/peer_candidates.txt" ]]; then
    peer_target="$(awk 'NR==1 {print $2}' "$session_dir/peer_candidates.txt")"
    echo "$peer_target" >"$session_dir/inferred_peer_target.txt"
  fi

  log_step "Capturing route/interface/queue snapshots"
  [[ -n "$peer_target" ]] && capture_route_snapshot "$peer_target" "$session_dir/route_peer_end.txt"
  [[ -n "$baseline_target" ]] && capture_route_snapshot "$baseline_target" "$session_dir/route_baseline_end.txt"
  capture_interface_stats "$session_dir/interface_stats_end.txt"
  capture_udp_queue_snapshot "$session_dir/udp_qdisc_end.txt"
  capture_noise_profile "$session_dir/noise_profile_end.txt" "$default_iface"
  if [[ -n "$default_iface" ]]; then
    capture_rate_sample "$default_iface" "$session_dir/net_rate_end.txt"
  fi

  log_step "Copying game and proxy logs"
  if [[ -n "$game_root" && -d "$game_root" ]]; then
    local f
    for f in BZLogger.txt dsound_proxy.log winmm_proxy.log multi.ini; do
      [[ -f "$game_root/$f" ]] && cp -f "$game_root/$f" "$session_dir/$f"
    done
  fi

  log_step "Collecting Steam Proton logs"
  if [[ -f "$session_dir/start.marker" ]]; then
    while IFS= read -r -d '' log_file; do
      cp -f "$log_file" "$session_dir/$(basename "$log_file")"
    done < <(find "$HOME" -maxdepth 1 -type f -name 'steam-*.log' -newer "$session_dir/start.marker" -print0 2>/dev/null)
  fi

  log_step "Collecting journal snapshot (up to 30s)"
  if command -v journalctl >/dev/null 2>&1; then
    if [[ -n "$start_iso" ]]; then
      run_with_timeout 30s journalctl --since "$start_iso" --no-pager >"$session_dir/journal_since_start.log" 2>/dev/null || true
    else
      run_with_timeout 30s journalctl -n 2000 --no-pager >"$session_dir/journal_since_start.log" 2>/dev/null || true
    fi
  fi

  log_step "Collecting coredump metadata (up to 20s each)"
  if command -v coredumpctl >/dev/null 2>&1; then
    run_with_timeout 20s coredumpctl list --no-pager >"$session_dir/coredumps_list.txt" 2>/dev/null || true
    run_with_timeout 20s coredumpctl info --no-pager >"$session_dir/coredumps_info.txt" 2>/dev/null || true
  fi

  log_step "Running verify snapshot"
  verify_log="$session_dir/verify_output.txt"
  if [[ -n "$game_root" && -d "$game_root" ]]; then
    (
      cd "$game_root"
      echo "# verify command"
      echo "VERIFY_PROXY_READBACK=1 '$SCRIPT_DIR/verify_net_patch.sh'"
      echo
      VERIFY_PROXY_READBACK=1 "$SCRIPT_DIR/verify_net_patch.sh"
    ) >"$verify_log" 2>&1 || true
  else
    {
      echo "# verify skipped"
      echo "game_root_missing=$game_root"
    } >"$verify_log"
  fi

  log_step "Creating bundle archive (this can take a minute)"
  archive_path="${session_dir}.tar.gz"
  tar -czf "$archive_path" -C "$(dirname "$session_dir")" "$(basename "$session_dir")"

  rm -f "$CURRENT_FILE"

  echo "Deep diagnostics stopped."
  echo "Bundle created: $archive_path"
  echo "Send this file back to the test coordinator."
}

mark_diag() {
  local message="${1:-}"
  if [[ ! -f "$CURRENT_FILE" ]]; then
    echo "ERROR: no active Linux deep diagnostics session found." >&2
    echo "Run ./Linux/tester_diag.sh start first." >&2
    exit 1
  fi
  if [[ -z "$message" ]]; then
    echo "ERROR: marker message is required." >&2
    usage
    exit 1
  fi

  local session_dir
  session_dir="$(cat "$CURRENT_FILE")"
  if [[ -z "$session_dir" || ! -d "$session_dir" ]]; then
    echo "ERROR: session directory missing: $session_dir" >&2
    exit 1
  fi
  echo "$(date -u +"%Y-%m-%dT%H:%M:%SZ") | $message" >>"$session_dir/tester_markers.log"
  echo "Marker recorded in $session_dir/tester_markers.log"
}

action="${1:-}"
case "$action" in
  start)
    shift
    start_diag "$@"
    ;;
  stop)
    stop_diag
    ;;
  mark)
    shift
    mark_diag "$*"
    ;;
  *)
    usage
    exit 1
    ;;
esac