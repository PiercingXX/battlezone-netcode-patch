#!/usr/bin/env bash
# bz_wrap.sh — wrap the game launch, bundle the session, upload it to Discord.
#
# WHY THIS IS A WRAPPER AND NOT PART OF THE DLL
#
# The opt-in *is* the mechanism. Steam launch options can wrap the game
# command, so this lives outside the game process entirely:
#
#   WINEDLLOVERRIDES=dsound=n,b ~/.local/share/bz-netcode/bz_wrap.sh %command% -nointro
#
# No wrapper in the launch options, nothing ever uploads. That beats uploading
# from the proxy on every axis: it runs after the process exits, with native
# tooling, so there is no HTTPS from a 32-bit mingw DLL and no work under the
# loader lock at DLL_PROCESS_DETACH — and it fires **even when the game
# crashes**, which is precisely when the bundle matters and precisely when
# testers forget.
#
# It also closes the two ways sessions currently die:
#
#   * BZLogger.txt is overwritten by the next launch. This snapshots it
#     *before* launching, so game 1's log survives game 2 starting. That is
#     exactly how PiercingXX's game-2 log was lost on 2026-07-26.
#   * Crash bundles never get sent because the tester restarts first. This
#     bundles on exit whatever the exit was.
#
# THE WEBHOOK URL IS NEVER COMMITTED. Discord participates in GitHub secret
# scanning: a webhook URL pushed to a public repo is auto-revoked. Run
# `bz_wrap.sh --setup` once; it writes ~/.config/bz-netcode/upload.conf with
# mode 600.
#
# PRIVACY: a bundle contains every peer's public IP. The destination must be a
# private channel. See docs/TESTING.md.
set -uo pipefail

# State (conf, outbox, work) lives next to this script whenever a sibling
# upload.conf exists; the XDG paths below are the fallback for a host install.
# The sibling wins because sandboxed Steam cannot see the host's dot-dirs —
# Flatpak remaps XDG_DATA_HOME and snapd's home interface excludes hidden
# files outright — so the installer mirrors wrapper + conf into the sandbox
# and this rule makes each copy self-contained. It also means running the
# sandbox copy from a host shell (say, --retry with the host's curl after an
# upload parked) hits that copy's own conf and outbox, not the host's.
SELF_DIR="$(cd -- "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")" && pwd)"
if [[ -f "$SELF_DIR/upload.conf" ]]; then
    CONF_DIR="$SELF_DIR"
    OUTBOX="$SELF_DIR/outbox"
    WORK="$SELF_DIR/work"
else
    CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/bz-netcode"
    OUTBOX="${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode/outbox"
    WORK="${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode/work"
fi
CONF_FILE="$CONF_DIR/upload.conf"

# One definition, used by meta.txt, --status and the installer's staleness
# check.  It was previously inlined in the meta.txt heredoc only, which is why
# a tester running V4.91-harvest against a V4.92-arms repo went unnoticed until
# somebody read a bundle's meta.txt after the fact (2026-08-12).
WRAPPER_VERSION="V5.3-shipped-20260816"

# Discord's webhook attachment cap is ~10 MB for an unboosted server. Stay
# under it with room for the multipart envelope.
MAX_PART_BYTES=$((9 * 1024 * 1024))

# Where the wrapper's own account of itself goes.
#
# log() used to write to stderr and nowhere else.  Under a sandboxed Steam that
# stderr is unreachable, so on 2026-08-12 the question "why did his logs take
# five minutes?" could not be answered from anything the tester was able to
# hand over — the bundle does not carry it and no file existed.  The answer
# turned out to be ordinary (the snap runtime has no curl or python3, so the
# wrapper parked and a 10-minute timer drained it), but nothing on that
# machine could say so.  Now it can.
LOG_FILE="$CONF_DIR/bz_wrap.log"
LOG_MAX_BYTES=$((256 * 1024))

log() {
    printf '[bz_wrap] %s\n' "$*" >&2
    # Best effort, always: a wrapper that dies because it could not write its
    # own log would be worse than one that logs nothing.  CONF_DIR may not
    # exist yet on the very first run.
    {
        # Only reach for mkdir when the directory is genuinely absent. In the
        # sandbox layout CONF_DIR is the wrapper's own directory and always
        # exists, and the environments where logging matters most are exactly
        # the toolless ones — needing a binary to record that no binaries are
        # available is how this stays undiagnosable.
        [[ -d "$CONF_DIR" ]] || mkdir -p "$CONF_DIR" 2>/dev/null || return 0
        # Single-file rotation, so an unattended machine cannot fill its disk
        # with retry chatter. Best-effort for the same reason as above: if wc
        # or mv is absent the log keeps growing, which beats not writing it.
        if [[ -f "$LOG_FILE" ]]; then
            local sz
            sz="$(wc -c <"$LOG_FILE" 2>/dev/null || echo 0)"
            if [[ "$sz" =~ ^[0-9]+$ ]] && (( sz > LOG_MAX_BYTES )); then
                mv -f "$LOG_FILE" "$LOG_FILE.1" 2>/dev/null || true
            fi
        fi
        printf '%s [%s] %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo '?')" \
               "$$" "$*" >>"$LOG_FILE" 2>/dev/null || true
    } 2>/dev/null || true
}

usage() {
    cat <<'EOF'
Usage:
  bz_wrap.sh --setup                 configure the webhook and player name
  bz_wrap.sh --retry                 send anything parked in the outbox and exit
  bz_wrap.sh --status                show configuration and outbox state
  bz_wrap.sh %command% [args...]     launch the game, then bundle and upload

Steam launch options (Linux):
  native / Flatpak:
    WINEDLLOVERRIDES=dsound=n,b "${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode/bz_wrap.sh" %command% -nointro
  Snap:
    WINEDLLOVERRIDES=dsound=n,b "$SNAP_USER_COMMON/.local/share/bz-netcode/bz_wrap.sh" %command% -nointro

Steam evaluates launch options through a shell. Flatpak remaps XDG_DATA_HOME
into its sandbox, so the first line finds the wrapper at
~/.local/share/bz-netcode/ on a native install and at
~/.var/app/com.valvesoftware.Steam/data/bz-netcode/ under Flatpak. Snap
remaps HOME itself and its sandbox cannot read the host's dot-dirs at all —
with the XDG line the exec target does not exist and the game never launches
— so its line uses $SNAP_USER_COMMON, which snapd always sets inside the
sandbox, and the wrapper must live at
~/snap/steam/common/.local/share/bz-netcode/ (the installer puts it there).

Everything before %command% is an environment variable; everything after it is
an argument to the game. The wrapper itself goes immediately before %command%.
EOF
}

# ── Configuration ────────────────────────────────────────────────────────────

load_conf() {
    BZ_WEBHOOK=""
    BZ_PLAYER=""
    BZ_INCLUDE_PROTON=0
    # Menu-only uploads default ON: a skipped bundle and a broken uploader look
    # identical from the Discord channel, and testers reported the uploader as
    # broken when it had just decided their session was uninteresting. The
    # bundles are a few kB. BZ_UPLOAD_MENU=0 in upload.conf restores the skip.
    BZ_UPLOAD_MENU=1
    if [[ -f "$CONF_FILE" ]]; then
        # shellcheck disable=SC1090
        source "$CONF_FILE"
    fi
    # An env override exists for unattended runs, but the file is the norm.
    BZ_WEBHOOK="${BZ_UPLOAD_WEBHOOK:-$BZ_WEBHOOK}"
}

do_setup() {
    # Under `curl | bash` stdin holds the piped script itself; prompts must
    # talk to the terminal or read would eat installer text as answers.  A
    # permission check on /dev/tty is not enough: without a controlling
    # terminal the node is "readable" but opening it fails.
    if [[ ! -t 0 ]]; then
        if { : < /dev/tty; } 2>/dev/null; then
            exec < /dev/tty
        else
            log "no interactive terminal; run --setup from a terminal"
            return 1
        fi
    fi
    mkdir -p "$CONF_DIR"
    load_conf

    cat <<'EOF'

bz_wrap setup
-------------
The webhook URL is pinned in the private Discord channel the bundles land in.
It is a weak secret: anyone holding it can post to that one channel, nothing
more. It is never written to the repo, and it must not be pasted anywhere
public - Discord auto-revokes webhook URLs that appear on GitHub.

EOF
    local url player proton
    read -r -p "Discord webhook URL${BZ_WEBHOOK:+ [keep existing]}: " url
    [[ -z "$url" ]] && url="$BZ_WEBHOOK"
    if [[ -z "$url" ]]; then
        log "no webhook URL given; nothing saved"
        return 1
    fi
    if [[ "$url" != https://discord.com/api/webhooks/* \
       && "$url" != https://discordapp.com/api/webhooks/* ]]; then
        log "that does not look like a Discord webhook URL"
        log "expected https://discord.com/api/webhooks/<id>/<token>"
        return 1
    fi

    read -r -p "Your player name (empty = your in-game name, read from BZLogger at upload time)${BZ_PLAYER:+ [$BZ_PLAYER]}: " player
    [[ -z "$player" ]] && player="$BZ_PLAYER"

    read -r -p "Include Proton logs? They are large. [y/N] " proton
    if [[ "$proton" == "y" || "$proton" == "Y" ]]; then proton=1; else proton=0; fi

    umask 077
    cat >"$CONF_FILE" <<EOF
# Written by bz_wrap.sh --setup. Do not commit this file.
BZ_WEBHOOK='$url'
BZ_PLAYER='$player'
BZ_INCLUDE_PROTON=$proton
EOF
    chmod 600 "$CONF_FILE"
    log "saved $CONF_FILE (mode 600)"

    echo
    echo "Now set your Steam launch options for Battlezone 98 Redux to:"
    echo
    # The standardized line: identical for native and Flatpak, because Steam
    # evaluates launch options through a shell and Flatpak remaps
    # XDG_DATA_HOME into its sandbox. The Snap copy needs its own line —
    # snap remaps HOME and blocks the host's dot-dirs, so only
    # $SNAP_USER_COMMON resolves reliably inside that sandbox. Fall back to
    # this script's real path only when it is installed somewhere
    # non-standard.
    local self_real
    self_real="$(readlink -f "$0")"
    if [[ "$self_real" == "$HOME/snap/steam/common/"* ]]; then
        echo '  WINEDLLOVERRIDES=dsound=n,b "$SNAP_USER_COMMON/.local/share/bz-netcode/bz_wrap.sh" %command% -nointro'
    elif [[ "$self_real" == "$(readlink -f "${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode/bz_wrap.sh" 2>/dev/null || true)" ]]; then
        echo '  WINEDLLOVERRIDES=dsound=n,b "${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode/bz_wrap.sh" %command% -nointro'
    else
        echo "  WINEDLLOVERRIDES=dsound=n,b $self_real %command% -nointro"
    fi
    echo
    echo "Order matters. Everything before %command% is an environment variable;"
    echo "everything after it is an argument handed to the game."
    echo
    echo "What gets sent: the proxy log, BZLogger (before and after), multi.ini,"
    echo "and a meta file. Bundles contain every peer's public IP, which is why"
    echo "the destination is a private channel."
}

do_status() {
    load_conf
    # First line, because "which version is this tester on?" is the question
    # that has to be answerable before any of the others mean anything.
    echo "wrapper     : $WRAPPER_VERSION"
    echo "             $(readlink -f -- "${BASH_SOURCE[0]}")"
    echo "config file : $CONF_FILE $( [[ -f "$CONF_FILE" ]] && echo '(present)' || echo '(MISSING - run --setup)')"
    echo "webhook     : $( [[ -n "$BZ_WEBHOOK" ]] && echo 'configured' || echo 'NOT SET')"
    echo "player      : ${BZ_PLAYER:-<auto: in-game name, read from BZLogger at upload time>}"
    echo "proton logs : $( [[ "${BZ_INCLUDE_PROTON:-0}" == "1" ]] && echo included || echo skipped)"
    echo "outbox      : $OUTBOX"
    if [[ -d "$OUTBOX" ]]; then
        local n
        n="$(find "$OUTBOX" -maxdepth 1 -type f \( -name '*.tar.xz*' -o -name '*.tar.gz*' \) ! -name '*.msg' 2>/dev/null | wc -l)"
        echo "              $n bundle(s) waiting"
    else
        echo "              empty"
    fi
    echo "uploader    : $(command -v curl >/dev/null 2>&1 && echo 'curl' \
                          || { command -v python3 >/dev/null 2>&1 && echo 'python3 (stdlib)'; } \
                          || echo 'NONE here - bundles park for the host-side drain')"
    echo "log         : $LOG_FILE $( [[ -f "$LOG_FILE" ]] && echo '(present)' || echo '(nothing logged yet)')"
    if [[ -f "$LOG_FILE" ]]; then
        echo "last lines  :"
        tail -n 5 "$LOG_FILE" 2>/dev/null | sed 's/^/              /'
    fi
}

# ── Helpers ──────────────────────────────────────────────────────────────────

# Steam runs launch options inside its runtime environment: LD_PRELOAD
# injects gameoverlayrenderer.so into every child and LD_LIBRARY_PATH points
# at Steam's own libraries, which breaks host binaries — observed 2026-07-28
# in the Flatpak, where the post-game curl failed and the bundle parked while
# the identical retry outside that environment sailed through.  The game must
# keep Steam's environment; the wrapper's helpers must not.
clean_env() { env -u LD_PRELOAD -u LD_LIBRARY_PATH -u LD_AUDIT "$@"; }

resolve_hostname() {
    local h=""
    command -v hostnamectl >/dev/null 2>&1 && h="$(hostnamectl --static 2>/dev/null || true)"
    [[ -z "$h" ]] && command -v hostname >/dev/null 2>&1 && h="$(hostname 2>/dev/null || true)"
    [[ -z "$h" && -r /etc/hostname ]] && h="$(tr -d '\n' </etc/hostname 2>/dev/null || true)"
    [[ -z "$h" ]] && h="${HOSTNAME:-unknown-host}"
    printf '%s' "$h" | tr -c 'A-Za-z0-9._-' '-'
}

# The name the bundle carries must be the name the game itself uses — the
# one every peer's BZLogger prints in its Adding Player lines, so bundles
# and cross-log analysis agree on who is who. The session's own log states
# it outright:
#   Authenticated to BZRNet As S<steamid>:<name>
# and unlike the Adding Player lines this one only ever names the LOCAL
# player. Field-tested the hard way: the Steam persona in loginusers.vdf is
# a login-time snapshot that matched nobody's in-game name.
game_player_name() {
    local game_dir="$1"
    [[ -n "$game_dir" && -f "$game_dir/BZLogger.txt" ]] || return 1
    sed -n 's/.*Authenticated to BZRNet As S[0-9]*://p' "$game_dir/BZLogger.txt" | tail -1
}

# Fallback only, for a session that died before authenticating: the Steam
# persona of the "MostRecent" "1" block in Steam's config/loginusers.vdf.
# The Steam root is wherever steamapps/ hangs off, which works unchanged for
# native, Flatpak and Snap installs.
steam_player_name() {
    local game_dir="$1"
    [[ -n "$game_dir" ]] || return 1
    local vdf="${game_dir%/steamapps/*}/config/loginusers.vdf"
    [[ -f "$vdf" ]] || return 1
    # Single-account files often carry no MostRecent flag at all; fall back
    # to the last account in the file.
    awk -F'"' '
        tolower($2) == "personaname" && $4 != "" { name = $4 }
        tolower($2) == "mostrecent" && $4 == "1" && name != "" { print name; found = 1; exit }
        END { if (!found && name != "") print name }
    ' "$vdf"
}

# What would catch a crash dump on this machine, if anything. Report-only:
# the 2026-08-03 Windows map-load crash produced no dump and nobody knew none
# could exist until the bundle was already the only evidence.
crash_capture_status() {
    local pattern
    pattern="$(cat /proc/sys/kernel/core_pattern 2>/dev/null || true)"
    case "$pattern" in
        *systemd-coredump*) echo "systemd-coredump" ;;
        \|*)                echo "piped:${pattern%% *}" ;;
        "")                 echo "unknown" ;;
        *)
            if [[ "$(ulimit -c 2>/dev/null || echo 0)" == "0" ]]; then
                echo "NONE (core_pattern=$pattern but ulimit -c is 0)"
            else
                echo "file:$pattern"
            fi
            ;;
    esac
}

# The game directory is wherever the executable Steam handed us lives.
game_dir_from_command() {
    local arg
    for arg in "$@"; do
        case "$arg" in
            */battlezone98redux.exe|*/Battlezone98Redux.exe|*battlezone98redux.exe)
                dirname "$arg"; return 0 ;;
        esac
    done
    # Proton command lines bury the game path mid-list, after Steam's own
    # helpers (reaper, steam-launch-wrapper, proton).  Any argument containing
    # steamapps/common/<Game>/ names the install even when the exe pattern
    # missed — observed 2026-07-28, when the fallback below bundled Steam's
    # ubuntu12_32 runtime dir instead of the game.
    # Steam's own helpers (SteamLinuxRuntime, Proton) live under
    # steamapps/common too, so prefer the candidate that holds the game exe
    # and fall back to the first that exists at all.
    local first_dir=""
    for arg in "$@"; do
        case "$arg" in
            */steamapps/common/*/*)
                local rest="${arg#*/steamapps/common/}"
                local dir="${arg%%/steamapps/common/*}/steamapps/common/${rest%%/*}"
                [[ -d "$dir" ]] || continue
                if [[ -f "$dir/battlezone98redux.exe" || -f "$dir/BZLogger.txt" ]]; then
                    printf '%s' "$dir"; return 0
                fi
                [[ -z "$first_dir" ]] && first_dir="$dir"
                ;;
        esac
    done
    if [[ -n "$first_dir" ]]; then
        printf '%s' "$first_dir"
        return 0
    fi
    # Last resort: the first argument that is an existing file — useful for a
    # hand-run wrapper, wrong for Steam launches (it finds Steam's reaper).
    for arg in "$@"; do
        if [[ -f "$arg" ]]; then dirname "$arg"; return 0; fi
    done
    return 1
}

# A BZLogger that never wrote "Exiting Game With Return Code" ended abruptly.
# This is the same rule tools/analyze_drops.py uses.
detect_crash() {
    local logfile="$1"
    [[ -f "$logfile" ]] || return 1
    if tail -c 200000 "$logfile" 2>/dev/null | grep -q 'Exiting Game With Return Code'; then
        return 1
    fi
    return 0
}

session_summary() {
    local logfile="$1"
    local map="unknown"
    if [[ -f "$logfile" ]]; then
        map="$(grep -o 'Launching Network Game .*, Map [^,]*' "$logfile" 2>/dev/null \
               | tail -1 | sed 's/.*Map //' || true)"
        [[ -z "$map" ]] && map="unknown"
    fi
    printf '%s' "$map"
}

# ── Upload ───────────────────────────────────────────────────────────────────

# Pure bash on purpose: the message is generated by this script, so only the
# characters it can actually contain need escaping. This used to shell out to
# python3 for json.dumps, which silently added python3 to the upload's
# requirements — see post_file for why every dependency here matters.
json_escape() {
    local s="$1"
    s=${s//\\/\\\\}
    s=${s//\"/\\\"}
    s=${s//$'\n'/\\n}
    s=${s//$'\t'/\\t}
    printf '%s' "$s"
}

# Discord needs multipart/form-data. curl is the natural tool but is NOT a
# given: the Steam snap's runtime ships neither curl nor python3, and stock
# Ubuntu desktop ships python3 but not curl. So: curl if present, else a
# python3 stdlib uploader (covers a stock Ubuntu host shell, where --retry
# drains bundles the snap sandbox had to park), else park.
post_file() {
    local file="$1" message="$2"
    load_conf
    if [[ -z "$BZ_WEBHOOK" ]]; then
        return 1
    fi
    local payload
    payload="{\"content\": \"$(json_escape "$message")\"}"
    # Which uploader ran, and how long it took, are the two facts every upload
    # question so far has turned on.  Record them before and after, so a
    # timeout leaves the "starting" line behind even when nothing returns.
    local t0 rc
    t0="$(date +%s 2>/dev/null || echo 0)"
    local sz
    sz="$(wc -c <"$file" 2>/dev/null || echo '?')"
    if command -v curl >/dev/null 2>&1; then
        log "upload: curl, $(basename "$file") ($sz bytes)"
        # --fail so a 4xx is an error rather than a silently discarded body.
        clean_env curl --fail --silent --show-error --max-time 300 \
             -F "payload_json=$payload" \
             -F "file1=@$file" \
             "$BZ_WEBHOOK" >/dev/null
        rc=$?
        log "upload: curl finished rc=$rc after $(( $(date +%s 2>/dev/null || echo 0) - t0 ))s"
        return $rc
    fi
    if command -v python3 >/dev/null 2>&1; then
        log "upload: python3 stdlib, $(basename "$file") ($sz bytes)"
        BZW_URL="$BZ_WEBHOOK" BZW_FILE="$file" BZW_PAYLOAD="$payload" \
        clean_env python3 - <<'PYEOF'
import os, sys, uuid, urllib.request

url = os.environ['BZW_URL']
path = os.environ['BZW_FILE']
payload = os.environ['BZW_PAYLOAD']
boundary = uuid.uuid4().hex
with open(path, 'rb') as fh:
    data = fh.read()
body = b''.join([
    b'--' + boundary.encode() + b'\r\n',
    b'Content-Disposition: form-data; name="payload_json"\r\n\r\n',
    payload.encode() + b'\r\n',
    b'--' + boundary.encode() + b'\r\n',
    ('Content-Disposition: form-data; name="file1"; filename="%s"\r\n'
     % os.path.basename(path)).encode(),
    b'Content-Type: application/octet-stream\r\n\r\n',
    data + b'\r\n',
    b'--' + boundary.encode() + b'--\r\n',
])
req = urllib.request.Request(
    url, data=body, method='POST',
    headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
try:
    urllib.request.urlopen(req, timeout=300).read()
except Exception as e:
    print('[bz_wrap] python3 upload failed: %s' % e, file=sys.stderr)
    sys.exit(1)
PYEOF
        rc=$?
        log "upload: python3 finished rc=$rc after $(( $(date +%s 2>/dev/null || echo 0) - t0 ))s"
        return $rc
    fi
    # The Steam snap's runtime is exactly this case, every single time. It is
    # not a failure and must not read like one: the bundle parks and the
    # host-side drain unit sends it. See install_linux.sh.
    log "no curl and no python3 in this environment (the Steam snap runtime has"
    log "neither) — parking the bundle for the host-side drain to send"
    return 1
}

# Park a bundle for the next wrapped launch rather than losing it.
park() {
    local file="$1" message="$2"
    mkdir -p "$OUTBOX"
    mv -f "$file" "$OUTBOX/" 2>/dev/null || cp -f "$file" "$OUTBOX/"
    printf '%s' "$message" >"$OUTBOX/$(basename "$file").msg"
    log "upload failed; parked in $OUTBOX (retried on the next wrapped launch)"
}

flush_outbox() {
    [[ -d "$OUTBOX" ]] || return 0
    local f msg
    for f in "$OUTBOX"/*.tar.xz "$OUTBOX"/*.tar.gz "$OUTBOX"/*.tar.xz.part* "$OUTBOX"/*.tar.gz.part*; do
        [[ -e "$f" ]] || continue
        [[ "$f" == *.msg ]] && continue
        msg="(retry) $(basename "$f")"
        [[ -f "$f.msg" ]] && msg="$(cat "$f.msg")"
        if post_file "$f" "$msg"; then
            log "uploaded parked bundle $(basename "$f")"
            rm -f "$f" "$f.msg"
        else
            log "parked bundle still not uploadable; leaving it"
            return 0
        fi
    done
}

# ── Bundle ───────────────────────────────────────────────────────────────────

collect_and_upload() {
    local game_dir="$1" snapshot="$2" start_utc="$3" exit_code="$4"

    load_conf
    local host player stamp bundle
    host="$(resolve_hostname)"
    # An explicit BZ_PLAYER still wins; otherwise the in-game name from this
    # session's own log, then the Steam persona, and the OS account name
    # only as a last resort. Sanitized the same way as the hostname because
    # it lands in the bundle filename.
    player="$BZ_PLAYER"
    [[ -z "$player" ]] && player="$(game_player_name "$game_dir" || true)"
    [[ -z "$player" ]] && player="$(steam_player_name "$game_dir" || true)"
    [[ -z "$player" ]] && player="$(id -un)"
    player="$(printf '%s' "$player" | tr -c 'A-Za-z0-9._-' '-')"
    stamp="$(date -u +%Y%m%dT%H%M%SZ)"
    bundle="$WORK/bz_${player}_${stamp}"
    mkdir -p "$bundle"

    # Meta first, so even a bundle that collects nothing else is interpretable.
    {
        echo "player=$player"
        echo "hostname=$host"
        echo "start_utc=$start_utc"
        echo "overrides=${overrides# }"
        echo "crash_capture=$(crash_capture_status)"
        echo "end_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "end_local=$(date +%Y-%m-%dT%H:%M:%S%z)"
        echo "utc_offset_seconds=$(date +%z | awk '{s=substr($0,1,1); h=substr($0,2,2); m=substr($0,4,2); v=(h*3600+m*60); print (s=="-"? -v : v)}')"
        echo "game_exit_code=$exit_code"
        echo "game_dir=$game_dir"
        echo "command=${GAME_CMDLINE:-}"
        echo "platform=linux-proton"
        echo "wrapper_version=$WRAPPER_VERSION"
    } >"$bundle/meta.txt"

    # The pre-launch snapshot is the whole point: this is game N-1's log,
    # which game N's launch has already overwritten in the game folder.
    if [[ -f "$snapshot" ]]; then
        cp -f "$snapshot" "$bundle/BZLogger.prelaunch.txt"
    fi

    local f
    for f in BZLogger.txt dsound_proxy.log winmm_proxy.log multi.ini; do
        [[ -f "$game_dir/$f" ]] && cp -f "$game_dir/$f" "$bundle/$f"
    done
    # Capture files persist in the game dir after the capture that made them,
    # so with logging off a bundle would ship YESTERDAY'S ring looking current
    # (happened 2026-08-03). Only take them if written during this session.
    for f in bz_buffer_log.bin bz_buffer_log.meta.txt capture_verify.txt; do
        [[ -f "$game_dir/$f" ]] || continue
        if [[ -n "${start_epoch:-}" ]] && \
           (( $(stat -c %Y "$game_dir/$f" 2>/dev/null || echo 0) < start_epoch )); then
            log "skipping stale $f (predates this session)"
            continue
        fi
        cp -f "$game_dir/$f" "$bundle/$f"
    done

    if [[ "${BZ_INCLUDE_PROTON:-0}" == "1" ]]; then
        for f in "$HOME"/steam-*.log; do
            [[ -e "$f" ]] && cp -f "$f" "$bundle/" 2>/dev/null || true
        done
    fi

    # Headline for the Discord message.
    local crash_flag="" map duration
    if detect_crash "$bundle/BZLogger.txt"; then
        crash_flag=" **CRASH** (no \`Exiting Game With Return Code\`)"
    fi
    map="$(session_summary "$bundle/BZLogger.txt")"
    duration="$(( $(date -u +%s) - $(date -u -d "$start_utc" +%s 2>/dev/null || date -u +%s) ))"

    # Menu-only sessions (no network game launched, no crash) teach nothing
    # about the netcode, but they do prove the uploader works — so they are
    # sent unless the user opted out with BZ_UPLOAD_MENU=0. A crash without a
    # map line always goes: dying before the map loads is exactly what needs
    # evidence.
    if [[ "$map" == "unknown" && -z "$crash_flag" && "${BZ_UPLOAD_MENU:-1}" == "0" ]]; then
        log "no network game this session and no crash; BZ_UPLOAD_MENU=0 is set, skipping upload"
        rm -rf "$bundle"
        return 0
    fi

    local message
    message="$(printf '%s' "\
**$player** on \`$host\` — map \`$map\`, $((duration / 60)) min, exit $exit_code$crash_flag")"

    local tarball="$WORK/$(basename "$bundle").tar.xz"
    # xz because BZLogger runs to tens of megabytes and compresses far under
    # the webhook cap.
    if command -v xz >/dev/null 2>&1; then
        clean_env tar -cJf "$tarball" -C "$WORK" "$(basename "$bundle")"
    else
        tarball="${tarball%.xz}.gz"
        clean_env tar -czf "$tarball" -C "$WORK" "$(basename "$bundle")"
    fi
    rm -rf "$bundle"

    local size
    size="$(stat -c %s "$tarball" 2>/dev/null || echo 0)"
    log "bundle $(basename "$tarball") is $((size / 1024)) kB"

    if [[ "$size" -le "$MAX_PART_BYTES" ]]; then
        if post_file "$tarball" "$message"; then
            log "uploaded"
            rm -f "$tarball"
        else
            park "$tarball" "$message"
        fi
        return 0
    fi

    # Oversized: split. Each part uploads separately; the receiver reassembles
    # with `cat part* > bundle.tar.xz`.
    log "bundle exceeds the webhook cap; splitting"
    split -b "$MAX_PART_BYTES" -d -a 2 "$tarball" "$tarball.part"
    local part total
    total="$(find "$WORK" -maxdepth 1 -name "$(basename "$tarball").part*" | wc -l)"
    local i=0
    for part in "$tarball".part*; do
        i=$((i + 1))
        if post_file "$part" "$message — part $i of $total (\`cat *.part* > bundle.tar.xz\`)"; then
            rm -f "$part"
        else
            park "$part" "$message — part $i of $total"
        fi
    done
    rm -f "$tarball"
}

# ── Main ─────────────────────────────────────────────────────────────────────

case "${1:-}" in
    --setup)  do_setup; exit $? ;;
    --status) do_status; exit 0 ;;
    --retry)  flush_outbox; exit 0 ;;
    -h|--help|"") usage; exit 0 ;;
esac

mkdir -p "$WORK"

# Recorded into meta.txt so a wrong game_dir guess can be diagnosed from the
# bundle itself.
GAME_CMDLINE="$*"

game_dir="$(game_dir_from_command "$@" || true)"
if [[ -z "$game_dir" ]]; then
    log "could not work out the game directory from the command line"
    log "launching anyway; no bundle will be collected"
fi

# Pre-launch: snapshot BZLogger before the game truncates it.
snapshot=""
start_utc="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
start_epoch="$(date +%s)"

# Per-session BZ_* overrides, for switching A/B arms without editing the Steam
# launch line: one KEY=VALUE per line in $CONF_DIR/bznet.env (BZ_ keys only —
# this file is a tuning knob, not a general environment editor). The host
# switching BZ_GOV_START between 16000/40000/80000 is the use this exists for.
# Every override is logged and recorded in meta.txt so the bundle says which
# arm it was.
overrides=""
if [[ -f "$CONF_DIR/bznet.env" ]]; then
    while IFS= read -r _line; do
        case "$_line" in
            BZ_[A-Za-z0-9_]*=*)
                _k="${_line%%=*}"
                _v="${_line#*=}"
                _v="${_v%\'}"; _v="${_v#\'}"; _v="${_v%\"}"; _v="${_v#\"}"
                export "${_k}=${_v}"
                overrides="$overrides $_k=$_v"
                log "session override: $_k=$_v (from bznet.env)"
                ;;
        esac
    done <"$CONF_DIR/bznet.env"
fi
if [[ -n "$game_dir" && -f "$game_dir/BZLogger.txt" ]]; then
    snapshot="$WORK/BZLogger.prelaunch.$$"
    cp -f "$game_dir/BZLogger.txt" "$snapshot" 2>/dev/null || snapshot=""
    [[ -n "$snapshot" ]] && log "snapshotted the previous session's BZLogger"
fi

# Anything parked from a previous run goes now, while there is a network and
# before the game saturates it.
flush_outbox

# Run the game. Never let a wrapper failure stop someone playing.
"$@"
rc=$?
log "game exited with $rc"

if [[ -n "$game_dir" ]]; then
    collect_and_upload "$game_dir" "$snapshot" "$start_utc" "$rc" || \
        log "bundle collection failed; the game session itself was unaffected"
fi
[[ -n "$snapshot" ]] && rm -f "$snapshot"

exit "$rc"
