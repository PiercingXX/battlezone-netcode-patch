#!/usr/bin/env bash
set -euo pipefail

LOG_FILE="BZLogger.txt"
EXE_FILE="battlezone98redux.exe"
EXPECTED_SRC="${1:-}"
EXPECTED_RECV="2097152"
EXPECTED_SEND="524288"
RUNTIME_MODE="${VERIFY_RUNTIME_ONLY:-0}"

if [[ ! -f "$LOG_FILE" ]]; then
  echo "Missing $LOG_FILE"
  exit 1
fi

if [[ ! -f "$EXE_FILE" ]]; then
  echo "Missing $EXE_FILE"
  exit 1
fi

start_line=$(grep -n "Starting BattleZone 98 Redux" "$LOG_FILE" | tail -n 1 | cut -d: -f1)
if [[ -z "$start_line" ]]; then
  echo "No startup marker found in $LOG_FILE"
  exit 1
fi

session_log=$(mktemp)
trap 'rm -f "$session_log"' EXIT
tail -n "+$start_line" "$LOG_FILE" > "$session_log"

echo "Latest startup marker:"
grep -n "Starting BattleZone 98 Redux" "$LOG_FILE" | tail -n 1 || true

echo "Latest loaded net.ini source:"
grep -n "MOD FOUND net.ini" "$session_log" | tail -n 1 || true

echo "Latest socket buffer line:"
grep -n "BZRNet P2P Socket Opened With" "$session_log" | tail -n 1 || true

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

# Validate the two patched immediates directly in the executable.
if [[ "$RUNTIME_MODE" == "1" ]]; then
  echo "Executable patch bytes: (runtime mode, skipped)"
  exe_ok=1
else
  send_hex=$(xxd -p -s 0x52d96a -l 4 "$EXE_FILE" | tr -d '\n')
  recv_hex=$(xxd -p -s 0x52db5e -l 4 "$EXE_FILE" | tr -d '\n')

  if [[ "$send_hex" == "00000800" && "$recv_hex" == "00002000" ]]; then
    exe_ok=1
  else
    exe_ok=0
  fi

  echo "Executable patch bytes:"
  echo "- send @0x52d96a: $send_hex (expected 00000800)"
  echo "- recv @0x52db5e: $recv_hex (expected 00002000)"
fi

if [[ "$src_ok" -eq 1 && "$buf_ok" -eq 1 ]]; then
  echo "VERIFY RESULT: PASS"
  exit 0
fi

echo "VERIFY RESULT: FAIL"
if [[ "$src_ok" -ne 1 ]]; then
  echo "- net.ini source mismatch"
fi
if [[ "$buf_ok" -ne 1 ]]; then
  echo "- socket buffer line mismatch"
fi
if [[ "$RUNTIME_MODE" == "1" && "$buf_ok" -ne 1 ]]; then
  echo "- runtime patch was likely applied after socket init; run runtime patch before launch, then start game"
elif [[ "$exe_ok" -eq 1 && "$buf_ok" -ne 1 ]]; then
  echo "- executable is patched; launch the game once after patching to generate a fresh log session"
fi
if [[ "$exe_ok" -ne 1 ]]; then
  echo "- executable patch bytes not detected"
fi
exit 2