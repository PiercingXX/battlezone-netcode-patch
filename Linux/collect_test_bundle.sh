#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_GAME_ROOT="$HOME/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
GAME_ROOT="${1:-$DEFAULT_GAME_ROOT}"

if [[ ! -d "$GAME_ROOT" ]]; then
  echo "ERROR: game folder not found: $GAME_ROOT" >&2
  echo "Usage: $0 /path/to/Battlezone\ 98\ Redux" >&2
  exit 1
fi

utc_stamp="$(date -u +"%Y%m%dT%H%M%SZ")"
local_stamp="$(date +"%Y-%m-%d %H:%M:%S %Z")"
host_name="$(hostname 2>/dev/null || echo unknown-host)"

out_dir="$REPO_ROOT/test_bundles"
bundle_dir="$out_dir/linux_${host_name}_${utc_stamp}"
mkdir -p "$bundle_dir"

echo "=== Battlezone Linux test bundle ==="
echo "Game folder: $GAME_ROOT"
echo "Bundle dir : $bundle_dir"
echo

read -r -p "Tester name (optional): " tester_name || true
read -r -p "Role (host/client, optional): " tester_role || true
read -r -p "Match type (1v1/2v2/etc, optional): " match_type || true
read -r -p "Map name (optional): " map_name || true
read -r -p "Notes (optional): " test_notes || true

# Run verifier and capture full output into the bundle.
verify_log="$bundle_dir/verify_output.txt"
{
  echo "# verify command"
  echo "VERIFY_PROXY_READBACK=1 '$SCRIPT_DIR/verify_net_patch.sh'"
  echo
  VERIFY_PROXY_READBACK=1 "$SCRIPT_DIR/verify_net_patch.sh"
} >"$verify_log" 2>&1 || true

# Collect expected logs if present.
copy_if_exists() {
  local src="$1"
  local dst_name="$2"
  if [[ -f "$src" ]]; then
    cp -f "$src" "$bundle_dir/$dst_name"
  fi
}

copy_if_exists "$GAME_ROOT/BZLogger.txt" "BZLogger.txt"
copy_if_exists "$GAME_ROOT/dsound_proxy.log" "dsound_proxy.log"
copy_if_exists "$GAME_ROOT/winmm_proxy.log" "winmm_proxy.log"
copy_if_exists "$GAME_ROOT/multi.ini" "multi.ini"

# Collect environment snapshot for later analysis.
{
  echo "timestamp_local=$local_stamp"
  echo "timestamp_utc=$utc_stamp"
  echo "tester_name=$tester_name"
  echo "tester_role=$tester_role"
  echo "match_type=$match_type"
  echo "map_name=$map_name"
  echo "notes=$test_notes"
  echo "host_name=$host_name"
  echo "user_name=${USER:-unknown}"
  echo "game_root=$GAME_ROOT"
  echo "repo_root=$REPO_ROOT"
  echo "kernel=$(uname -srmo 2>/dev/null || true)"
  echo "desktop_session=${XDG_CURRENT_DESKTOP:-unknown}"
  echo "steam_path=$(command -v steam || echo not-found)"
  echo "proton_log_env=${PROTON_LOG:-unset}"
} >"$bundle_dir/session_info.txt"

# Archive bundle.
archive_path="$out_dir/$(basename "$bundle_dir").tar.gz"
tar -czf "$archive_path" -C "$out_dir" "$(basename "$bundle_dir")"

echo
if grep -q "VERIFY RESULT: PASS" "$verify_log"; then
  echo "Verifier result: PASS"
else
  echo "Verifier result: FAIL or inconclusive (check verify_output.txt)"
fi

echo "Bundle created: $archive_path"
echo "Send this file back to the test coordinator."
