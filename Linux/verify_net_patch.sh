#!/usr/bin/env bash
set -euo pipefail

LOG_FILE="BZLogger.txt"
PROXY_LOG_FILE="dsound_proxy.log"
EXPECTED_SRC="${1:-}"
EXPECTED_RECV="2097152"
EXPECTED_SEND="524288"
PROXY_MODE="${VERIFY_PROXY_READBACK:-1}"

if [[ ! -f "$LOG_FILE" ]]; then
  echo "Missing $LOG_FILE"
  exit 1
fi

start_line=$(grep -n "Starting BattleZone 98 Redux" "$LOG_FILE" | tail -n 1 | cut -d: -f1)
if [[ -z "$start_line" ]]; then
  echo "No startup marker found in $LOG_FILE"
  exit 1
fi

session_log=$(mktemp)
proxy_session_log=$(mktemp)
trap 'rm -f "$session_log" "$proxy_session_log"' EXIT
tail -n "+$start_line" "$LOG_FILE" > "$session_log"

if [[ -f "$PROXY_LOG_FILE" ]]; then
  proxy_start_line=$(grep -n "DllMain: DLL_PROCESS_ATTACH" "$PROXY_LOG_FILE" | tail -n 1 | cut -d: -f1 || true)
  if [[ -n "${proxy_start_line:-}" ]]; then
    tail -n "+$proxy_start_line" "$PROXY_LOG_FILE" > "$proxy_session_log"
  else
    cat "$PROXY_LOG_FILE" > "$proxy_session_log"
  fi
fi

echo "Latest startup marker:"
grep -n "Starting BattleZone 98 Redux" "$LOG_FILE" | tail -n 1 || true

echo "Latest loaded net.ini source:"
grep -n "MOD FOUND net.ini" "$session_log" | tail -n 1 || true

echo "Latest socket buffer line:"
grep -n "BZRNet P2P Socket Opened With" "$session_log" | tail -n 1 || true

echo "Latest proxy effective readback line:"
if [[ -f "$PROXY_LOG_FILE" ]]; then
  grep -n "effective readback" "$proxy_session_log" | tail -n 1 || true
else
  echo "(no $PROXY_LOG_FILE found)"
fi

if [[ -n "$EXPECTED_SRC" ]]; then
  echo "Expected net.ini source should contain:"
  echo "$EXPECTED_SRC"
  if grep -Fq "$EXPECTED_SRC" "$session_log"; then
    src_ok=1
  else
    src_ok=0
  fi
else
  echo "Expected net.ini source: (skipped)"
  src_ok=1
fi

if grep -q "BZRNet P2P Socket Opened With $EXPECTED_RECV received buffer, $EXPECTED_SEND send buffer" "$session_log"; then
  buf_ok=1
else
  buf_ok=0
fi

if [[ "$PROXY_MODE" == "1" ]]; then
  if [[ -f "$PROXY_LOG_FILE" ]] && grep -Eq "effective readback SO_SNDBUF=$EXPECTED_SEND .*SO_RCVBUF=$EXPECTED_RECV" "$proxy_session_log"; then
    proxy_ok=1
  else
    proxy_ok=0
  fi
else
  proxy_ok=0
fi

if [[ "$src_ok" -eq 1 && ( "$buf_ok" -eq 1 || "$proxy_ok" -eq 1 ) ]]; then
  echo "VERIFY RESULT: PASS"
  if [[ "$buf_ok" -ne 1 && "$proxy_ok" -eq 1 ]]; then
    echo "- passed via proxy readback verification mode"
  fi
  exit 0
fi

echo "VERIFY RESULT: FAIL"
if [[ "$src_ok" -ne 1 ]]; then
  echo "- net.ini source mismatch"
fi
if [[ "$buf_ok" -ne 1 ]]; then
  echo "- socket buffer line mismatch"
fi
if [[ "$PROXY_MODE" == "1" && "$proxy_ok" -ne 1 ]]; then
  echo "- proxy readback mismatch"
fi
if [[ "$PROXY_MODE" == "1" && "$buf_ok" -ne 1 ]]; then
  echo "- BZLogger startup text is not authoritative for this Proton hook path"
  echo "- proxy readback is the effective-value source of truth"
fi
exit 2