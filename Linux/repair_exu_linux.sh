#!/usr/bin/env bash
set -euo pipefail

GAME="Battlezone 98 Redux"
APPID="301650"
MODID="3406347034"
GAME_PATH=""
REQUIRED="0"
QUIET="0"

usage() {
    cat <<'EOF'
Usage:
  repair_exu_linux.sh [--game-path /path/to/Battlezone 98 Redux] [--modid 3406347034] [--required] [--quiet]

Behavior:
  - By default this is best-effort and exits success if the EXU workshop mod is missing.
  - With --required, missing EXU workshop files are treated as an error.
EOF
}

log() {
    if [[ "$QUIET" != "1" ]]; then
        echo "$@"
    fi
}

warn() {
    echo "[repair_exu_linux] $*" >&2
}

die() {
    warn "$1"
    exit "${2:-1}"
}

STEAM_ROOTS=(
    "$HOME/.local/share/Steam"
    "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
    "$HOME/.var/app/com.valvesoftware.Steam/data/Steam"
    "$HOME/snap/steam/common/.local/share/Steam"
    "$HOME/snap/steam/current/.local/share/Steam"
    "$HOME/snap/steam/common/.steam/steam"
    "$HOME/snap/steam/current/.steam/steam"
    "$HOME/snap/steam/common/.steam/root"
    "$HOME/snap/steam/current/.steam/root"
    "$HOME/snap/steam/common/.steam/debian-installation"
    "$HOME/snap/steam/current/.steam/debian-installation"
)

find_game_dir() {
    local root
    for root in "${STEAM_ROOTS[@]}"; do
        if [[ -d "$root/steamapps/common/$GAME" ]]; then
            printf '%s\n' "$root/steamapps/common/$GAME"
            return 0
        fi
    done

    find "$HOME/.local/share/Steam" "$HOME/.var/app/com.valvesoftware.Steam" "$HOME/snap/steam" \
        -type d -path "*/steamapps/common/$GAME" 2>/dev/null | head -n 1
}

find_workshop_mod_dir() {
    local root
    for root in "${STEAM_ROOTS[@]}"; do
        if [[ -d "$root/steamapps/workshop/content/$APPID/$MODID" ]]; then
            printf '%s\n' "$root/steamapps/workshop/content/$APPID/$MODID"
            return 0
        fi
    done

    find "$HOME/.local/share/Steam" "$HOME/.var/app/com.valvesoftware.Steam" "$HOME/snap/steam" \
        -type d -path "*/workshop/content/$APPID/$MODID" 2>/dev/null | head -n 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --game-path)
            if [[ $# -lt 2 ]]; then
                die "Missing value for --game-path" 2
            fi
            GAME_PATH="$2"
            shift 2
            ;;
        --modid)
            if [[ $# -lt 2 ]]; then
                die "Missing value for --modid" 2
            fi
            MODID="$2"
            shift 2
            ;;
        --required)
            REQUIRED="1"
            shift
            ;;
        --quiet)
            QUIET="1"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "Unknown argument: $1" 2
            ;;
    esac
done

if [[ -z "$GAME_PATH" ]]; then
    GAME_PATH="$(find_game_dir || true)"
fi

if [[ -z "$GAME_PATH" || ! -f "$GAME_PATH/battlezone98redux.exe" ]]; then
    die "Could not find Battlezone 98 Redux game path." 10
fi

WORKSHOP_MODDIR="$(find_workshop_mod_dir || true)"
if [[ -z "$WORKSHOP_MODDIR" ]]; then
    if [[ "$REQUIRED" == "1" ]]; then
        die "Could not find workshop mod '$MODID'. Subscribe to EXU in Steam Workshop, launch Steam once, then rerun." 11
    fi
    log "EXU workshop mod ($MODID) not found; skipping EXU repair."
    exit 0
fi

if [[ ! -f "$WORKSHOP_MODDIR/exu.dll" ]]; then
    if [[ "$REQUIRED" == "1" ]]; then
        die "'$WORKSHOP_MODDIR/exu.dll' not found. Workshop download may be incomplete." 12
    fi
    log "Workshop mod found but exu.dll is missing; skipping EXU repair."
    exit 0
fi

PACKAGED_DIR="$GAME_PATH/packaged_mods"
PACKAGED_MOD_PATH="$PACKAGED_DIR/$MODID"
mkdir -p "$PACKAGED_DIR"

if [[ -L "$PACKAGED_MOD_PATH" ]]; then
    rm -f "$PACKAGED_MOD_PATH"
fi
mkdir -p "$PACKAGED_MOD_PATH"
cp -af "$WORKSHOP_MODDIR/." "$PACKAGED_MOD_PATH/"

MODDIR="$GAME_PATH/mods/$MODID"
mkdir -p "$MODDIR"
cp -f "$WORKSHOP_MODDIR/exu.dll" "$MODDIR/exu.dll"
if [[ -f "$WORKSHOP_MODDIR/exu.lua" ]]; then
    cp -f "$WORKSHOP_MODDIR/exu.lua" "$MODDIR/exu.lua"
else
    rm -f "$MODDIR/exu.lua"
fi

log "EXU repair complete for mod $MODID:"
log "  packaged_mods/$MODID refreshed from Workshop"
log "  mods/$MODID/exu.dll copied"
if [[ -f "$MODDIR/exu.lua" ]]; then
    log "  mods/$MODID/exu.lua copied"
else
    log "  mods/$MODID/exu.lua removed (not provided by Workshop)"
fi
