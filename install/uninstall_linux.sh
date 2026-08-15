#!/usr/bin/env bash
# Remove the Battlezone netcode patch from a Linux (Proton) install.
#
# There was no uninstaller for either platform until V4.9, which is awkward for
# a script that asks for sudo and writes to /etc. Everything the installer
# touches is removed here, and nothing else:
#
#   <game>/dsound.dll                                  the proxy
#   <game>/packaged_mods/9990001/net.ini               the tuning mod
#   /etc/sysctl.d/99-battlezone-netcode.conf           the UDP buffer limits
#   bz_wrap.sh + upload.conf                           the uploader (test crew),
#                                                      host copy and any Snap /
#                                                      Flatpak sandbox mirrors
#
# Logs and captures the game or the proxy produced are left alone: they are
# research data, and deleting someone's session logs to uninstall a DLL would
# be a rude surprise. --purge-logs removes them if you actually want that.
#
# The Steam launch options are yours to clear; a script cannot edit them.
set -euo pipefail

GAME_PATH=""
PURGE_LOGS=0
ASSUME_YES=0

usage() {
    cat <<'EOF'
Usage:
  uninstall_linux.sh [--game-path /path/to/Battlezone 98 Redux] [--purge-logs] [--yes]

Options:
  --game-path   Game folder. Auto-detected from the usual Steam roots if omitted.
  --purge-logs  Also delete dsound_proxy.log, bz_buffer_log.* and BZLogger.txt
                from the game folder. Off by default: those are research data.
  --yes         Do not prompt.

Removes:
  <game>/dsound.dll
  <game>/packaged_mods/9990001/net.ini   (and the directory, if left empty)
  /etc/sysctl.d/99-battlezone-netcode.conf   (needs sudo)
  bz_wrap.sh + upload.conf (host, Snap and Flatpak copies; a non-empty
  outbox of unsent session bundles is kept)
  bz-netcode-retry.timer/.service (the user timer that drains a Snap outbox)

Afterwards, clear the Steam launch options by hand:
  WINEDLLOVERRIDES=dsound=n,b %command% -nointro
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --game-path)
            [[ $# -ge 2 ]] || { echo "Missing value for --game-path" >&2; exit 1; }
            GAME_PATH="$2"; shift 2 ;;
        --purge-logs) PURGE_LOGS=1; shift ;;
        --yes|-y)     ASSUME_YES=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; usage; exit 1 ;;
    esac
done

prompt_yes_no() {
    [[ "$ASSUME_YES" == "1" ]] && return 0
    local answer
    read -r -p "$1 [y/N] " answer || return 1
    [[ "$answer" == "y" || "$answer" == "Y" ]]
}

GAME_PATHS=()
detect_game_paths() {
    # Keep this list a superset of install_linux.sh's detect_game_paths: the
    # uninstaller must find every install the installer can create.  Every
    # match is cleaned, mirroring the installer's patch-all-installs rule
    # (2026-08-15: first-match-wins left a Flatpak copy stale).
    local candidates=(
        "$HOME/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
        "$HOME/.steam/steam/steamapps/common/Battlezone 98 Redux"
        "$HOME/.steam/root/steamapps/common/Battlezone 98 Redux"
        "$HOME/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
        "$HOME/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux"
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Battlezone 98 Redux"
    )
    local c seen
    for c in "${candidates[@]}"; do
        if [[ -f "$c/battlezone98redux.exe" ]]; then
            # ~/.steam/steam is usually a symlink to ~/.local/share/Steam;
            # resolve before deduplicating or the same install is listed twice.
            local real; real="$(readlink -f "$c")"
            for seen in "${GAME_PATHS[@]}"; do
                [[ "$(readlink -f "$seen")" == "$real" ]] && continue 2
            done
            GAME_PATHS+=("$c")
        fi
    done
}

if [[ -n "$GAME_PATH" ]]; then
    GAME_PATHS=("$GAME_PATH")
else
    detect_game_paths
fi

if [[ ${#GAME_PATHS[@]} -eq 0 ]]; then
    echo "Could not find the game. Pass --game-path." >&2
    exit 1
fi

for gp in "${GAME_PATHS[@]}"; do
    if [[ ! -f "$gp/battlezone98redux.exe" ]]; then
        echo "Game executable not found in: $gp" >&2
        exit 1
    fi
done

echo "Game folder(s):"
printf '  %s\n' "${GAME_PATHS[@]}"
echo

removed=0

remove_file() {
    local f="$1"
    if [[ -e "$f" ]]; then
        rm -f "$f"
        echo "  removed $f"
        removed=$((removed + 1))
    else
        echo "  (absent) $f"
    fi
}

echo "Removing patch files:"
for GAME_PATH in "${GAME_PATHS[@]}"; do
    # Other mods ship a dsound.dll too (DSOAL, for one). Only delete a DLL that
    # carries this patch's own marker string; anything else is not ours to remove.
    if [[ -f "$GAME_PATH/dsound.dll" ]] && ! grep -aq 'BZ_GOV_START' "$GAME_PATH/dsound.dll"; then
        echo "  KEEPING $GAME_PATH/dsound.dll: it does not look like this patch's"
        echo "  proxy (no BZ_GOV_START marker) — another mod's DLL? Remove it by"
        echo "  hand if you are certain."
    else
        remove_file "$GAME_PATH/dsound.dll"
    fi
    remove_file "$GAME_PATH/packaged_mods/9990001/net.ini"
    # Only if the installer's own directory is now empty; never recursive.
    if [[ -d "$GAME_PATH/packaged_mods/9990001" ]]; then
        rmdir "$GAME_PATH/packaged_mods/9990001" 2>/dev/null \
            && echo "  removed empty $GAME_PATH/packaged_mods/9990001" || true
    fi
done

# The uploader, wherever the installer may have put it: the host XDG dirs
# plus the Snap and Flatpak sandbox mirrors. A non-empty outbox holds session
# bundles that never uploaded — research data, same rule as the game logs.
uploader_dirs=(
    "${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode"
    "${XDG_CONFIG_HOME:-$HOME/.config}/bz-netcode"
    "$HOME/snap/steam/common/.local/share/bz-netcode"
    "$HOME/.var/app/com.valvesoftware.Steam/data/bz-netcode"
)
for d in "${uploader_dirs[@]}"; do
    [[ -e "$d/bz_wrap.sh" || -e "$d/upload.conf" ]] || continue
    [[ -e "$d/bz_wrap.sh" ]] && remove_file "$d/bz_wrap.sh"
    [[ -e "$d/upload.conf" ]] && remove_file "$d/upload.conf"
    rm -rf "$d/work"
    rmdir "$d/outbox" 2>/dev/null || true
    if [[ -d "$d/outbox" ]]; then
        echo "  keeping $d/outbox: it holds unsent session bundles"
    fi
    rmdir "$d" 2>/dev/null && echo "  removed empty $d" || true
done

# The host-side timer that drains a Snap install's outbox.
unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
if [[ -f "$unit_dir/bz-netcode-retry.timer" || -f "$unit_dir/bz-netcode-retry.service" ]]; then
    systemctl --user disable --now bz-netcode-retry.timer 2>/dev/null || true
    remove_file "$unit_dir/bz-netcode-retry.timer"
    remove_file "$unit_dir/bz-netcode-retry.service"
    systemctl --user daemon-reload 2>/dev/null || true
fi

if [[ "$PURGE_LOGS" == "1" ]] && \
   prompt_yes_no "Delete session logs and captures? They are research data and cannot be recovered."; then
    echo
    echo "Removing logs and captures (--purge-logs):"
    for GAME_PATH in "${GAME_PATHS[@]}"; do
        remove_file "$GAME_PATH/dsound_proxy.log"
        remove_file "$GAME_PATH/winmm_proxy.log"
        remove_file "$GAME_PATH/bz_buffer_log.bin"
        remove_file "$GAME_PATH/bz_buffer_log.meta.txt"
        remove_file "$GAME_PATH/BZLogger.txt"
    done
else
    echo
    echo "Leaving logs and captures in place (pass --purge-logs to delete them)."
fi

sysctl_file="/etc/sysctl.d/99-battlezone-netcode.conf"
if [[ -f "$sysctl_file" ]]; then
    echo
    echo "The installer raised the kernel UDP buffer limits in:"
    echo "  $sysctl_file"
    echo "Contents:"
    sed 's/^/    /' "$sysctl_file"
    echo
    echo "Nothing else on the system depends on these; they are a ceiling, not"
    echo "a default, so leaving them costs nothing either."
    if prompt_yes_no "Remove $sysctl_file?"; then
        sudo_cmd=""
        if [[ "$(id -u)" != "0" ]]; then
            if command -v sudo >/dev/null 2>&1; then
                sudo_cmd="sudo"
            else
                echo "  need root to remove it and sudo is not available; skipping." >&2
            fi
        fi
        if [[ "$(id -u)" == "0" || -n "$sudo_cmd" ]]; then
            # An if, not &&: under set -e a refused sudo would abort the
            # script before the summary and launch-option instructions.
            if $sudo_cmd rm -f "$sysctl_file"; then
                echo "  removed $sysctl_file"
                removed=$((removed + 1))
                echo "  (the raised limits stay live until the next reboot)"
            else
                echo "  could not remove it (sudo failed?); left in place." >&2
            fi
        fi
    else
        echo "  kept."
    fi
fi

echo
if [[ "$removed" == "0" ]]; then
    echo "Nothing to remove — the patch does not appear to be installed here."
else
    echo "Uninstall complete ($removed item(s) removed)."
fi
echo
echo "One thing this script cannot do: clear your Steam launch options."
echo "Open Steam > Battlezone 98 Redux > Properties > Launch Options and"
echo "delete the WINEDLLOVERRIDES=dsound=n,b line."
