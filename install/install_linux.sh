#!/usr/bin/env bash
set -euo pipefail

REPO_SLUG="PiercingXX/battlezone-netcode-patch"
# Baked per branch (see the Windows installer for the incident that made
# this explicit): this copy lives on experimental/v4.9, so running it with
# no BZNET_REF installs the branch it came from, not master.
REF="${BZNET_REF:-experimental/v4.9}"
GAME_PATH="${BZNET_GAME_PATH:-}"
ARCHIVE_URL="${BZNET_ARCHIVE_URL:-https://github.com/${REPO_SLUG}/archive/${REF}.tar.gz}"
# BZNET_REF is validated after validate_ref is defined; see below.
ASSUME_YES="${BZNET_ASSUME_YES:-0}"
PACKAGE_MANAGER=""
SUDO_CMD=""

usage() {
    cat <<'EOF'
Usage:
  install_linux.sh [--game-path /path/to/Battlezone 98 Redux] [--ref git-ref]

Environment overrides:
  BZNET_GAME_PATH   Explicit game path
  BZNET_REF         Git ref or branch to install from
  BZNET_ARCHIVE_URL Alternate source archive URL or local archive path
  BZNET_ASSUME_YES  Set to 1 to skip dependency-install confirmation prompts
EOF
}

prompt_yes_no() {
    local prompt_text="$1"
    local response=""

    if [[ "$ASSUME_YES" == "1" ]]; then
        return 0
    fi

    if [[ ! -r /dev/tty ]]; then
        echo "A confirmation prompt is required, but no interactive terminal is available." >&2
        echo "Re-run with BZNET_ASSUME_YES=1 if you want to allow dependency installation non-interactively." >&2
        exit 1
    fi

    printf "%s [Y/n]: " "$prompt_text" > /dev/tty
    IFS= read -r response < /dev/tty || true
    response="${response:-Y}"

    case "$response" in
        Y|y|Yes|YES|yes)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

configure_privilege_escalation() {
    if [[ "$(id -u)" -eq 0 ]]; then
        SUDO_CMD=""
        return
    fi

    if command -v sudo >/dev/null 2>&1; then
        SUDO_CMD="sudo"
        return
    fi

    echo "Dependency installation requires root privileges, but sudo is not available." >&2
    exit 1
}

detect_package_manager() {
    if command -v apt-get >/dev/null 2>&1; then
        PACKAGE_MANAGER="apt"
        return
    fi

    if command -v pacman >/dev/null 2>&1; then
        PACKAGE_MANAGER="pacman"
        return
    fi

    if command -v dnf >/dev/null 2>&1; then
        PACKAGE_MANAGER="dnf"
        return
    fi

    echo "Unsupported Linux distribution: could not find apt-get, pacman, or dnf." >&2
    exit 1
}

# A git ref goes straight into a URL, so it has to look like a git ref.  An
# unvalidated value here is a URL-injection hole in a script people paste from
# a chat message and run with sudo available.
validate_ref() {
    local ref="$1"
    if [[ -z "$ref" ]]; then
        echo "Empty git ref." >&2
        exit 1
    fi
    if [[ ! "$ref" =~ ^[A-Za-z0-9._/-]+$ ]]; then
        echo "Refusing ref '$ref': only letters, digits, dot, underscore," >&2
        echo "slash and dash are allowed in a git ref." >&2
        exit 1
    fi
    case "$ref" in
        -*|*..*|*//*|*/) echo "Refusing malformed git ref '$ref'." >&2; exit 1 ;;
    esac
}

# The Windows installer verifies the SHA256 of what it downloads against a
# sidecar; this side downloaded a source tarball with no integrity check at
# all.  It builds from that tarball, so a corrupted or substituted archive is
# executed, not merely unpacked.
#
# The sidecar lives beside the archive on the same host, so this is an
# integrity check and not a trust anchor -- it catches a truncated download or
# a mangled proxy, not a compromised github.com.  Say so rather than implying
# more.  BZNET_ARCHIVE_SHA256 pins it properly for anyone who wants that.
verify_archive() {
    local file="$1"
    local expected="${BZNET_ARCHIVE_SHA256:-}"

    if ! command -v sha256sum >/dev/null 2>&1; then
        if [[ -n "$expected" ]]; then
            echo "BZNET_ARCHIVE_SHA256 is set but sha256sum is not available;" >&2
            echo "refusing to build unverified when an explicit pin was asked for." >&2
            exit 1
        fi
        echo "sha256sum not found; skipping the archive hash check." >&2
        expected=""
    else
        local actual
        actual="$(sha256sum "$file" | awk '{print $1}')"
        echo "Source archive SHA256: $actual"
    fi

    if [[ -n "$expected" ]]; then
        expected="$(printf '%s' "$expected" | tr 'A-Z' 'a-z')"
        if [[ "$actual" != "$expected" ]]; then
            echo "Archive SHA256 mismatch." >&2
            echo "  expected: $expected" >&2
            echo "  actual:   $actual" >&2
            echo "Refusing to build from it." >&2
            exit 1
        fi
        echo "Archive matches BZNET_ARCHIVE_SHA256."
        return 0
    fi

    # No pin given: at least prove the archive is a well-formed tarball before
    # we extract and build from it.  A truncated download fails here instead of
    # halfway through a compile with a baffling error.
    if ! tar -tzf "$file" >/dev/null 2>&1; then
        echo "Downloaded archive is not a readable gzip tarball -- the download" >&2
        echo "was truncated or intercepted. Refusing to build from it." >&2
        exit 1
    fi
    echo "(Set BZNET_ARCHIVE_SHA256 to pin this exactly.)"
}

# BZNET_REF arrives from the environment and lands in a URL just the same.
validate_ref "$REF"

download_to() {
    local source_path="$1"
    local out_file="$2"

    if [[ -f "$source_path" ]]; then
        command cp -f "$source_path" "$out_file"
        return
    fi

    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$source_path" -o "$out_file"
        return
    fi

    if command -v wget >/dev/null 2>&1; then
        wget -qO "$out_file" "$source_path"
        return
    fi

    echo "Missing curl or wget in PATH." >&2
    exit 2
}

install_packages() {
    local packages=()
    local explanation=""

    detect_package_manager
    configure_privilege_escalation

    case "$PACKAGE_MANAGER" in
        apt)
            packages=(curl wget tar make gcc-mingw-w64-i686 g++-mingw-w64-i686 binutils-mingw-w64-i686)
            explanation="The installer needs a download tool, tar, make, and the 32-bit MinGW cross-compiler so it can build dsound.dll locally from source for Proton."
            ;;
        pacman)
            packages=(curl wget tar make mingw-w64-gcc)
            explanation="The installer needs a download tool, tar, make, and the MinGW cross-compiler package so it can build dsound.dll locally from source for Proton."
            ;;
        dnf)
            packages=(curl wget tar make mingw32-gcc mingw32-gcc-c++)
            explanation="The installer needs a download tool, tar, make, and the MinGW cross-compiler packages so it can build dsound.dll locally from source for Proton."
            ;;
    esac

    echo "$explanation"
    echo "Packages to install: ${packages[*]}"

    if ! prompt_yes_no "Proceed with automatic dependency installation?"; then
        echo "Dependency installation cancelled." >&2
        exit 1
    fi

    case "$PACKAGE_MANAGER" in
        apt)
            $SUDO_CMD apt-get update
            $SUDO_CMD apt-get install -y "${packages[@]}"
            ;;
        pacman)
            $SUDO_CMD pacman -Sy --needed --noconfirm "${packages[@]}"
            ;;
        dnf)
            $SUDO_CMD dnf install -y "${packages[@]}"
            ;;
    esac
}

ensure_build_dependencies() {
    local missing=0

    command -v tar >/dev/null 2>&1 || missing=1
    command -v make >/dev/null 2>&1 || missing=1
    command -v i686-w64-mingw32-g++ >/dev/null 2>&1 || missing=1

    if command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1; then
        :
    else
        missing=1
    fi

    if [[ "$missing" -eq 0 ]]; then
        return
    fi

    install_packages
}

# The proxy asks for SO_RCVBUF=4MB / SO_SNDBUF=512KB, but the Linux kernel
# silently clamps setsockopt to net.core.rmem_max / net.core.wmem_max
# (~208KB by default on most distros).  Without raising these limits the
# patch's buffer enlargement is mostly fictional under Proton.
apply_socket_buffer_sysctls() {
    local target_rmem=4194304
    local target_wmem=524288
    local sysctl_file="/etc/sysctl.d/99-battlezone-netcode.conf"
    local current_rmem current_wmem

    current_rmem="$(sysctl -n net.core.rmem_max 2>/dev/null || echo 0)"
    current_wmem="$(sysctl -n net.core.wmem_max 2>/dev/null || echo 0)"

    if [[ "$current_rmem" -ge "$target_rmem" && "$current_wmem" -ge "$target_wmem" ]]; then
        echo "Kernel UDP buffer limits already sufficient (rmem_max=$current_rmem wmem_max=$current_wmem)."
        return 0
    fi

    echo
    echo "Kernel UDP buffer limits are below the patch targets:"
    echo "  net.core.rmem_max=$current_rmem (need >= $target_rmem)"
    echo "  net.core.wmem_max=$current_wmem (need >= $target_wmem)"
    echo "Without this, the kernel silently clamps the enlarged socket buffers."

    # This step is optional tuning: never abort the install over it.
    local manual_hint
    manual_hint=$(cat <<EOF
To apply manually later, run:
  sudo sysctl -w net.core.rmem_max=$target_rmem net.core.wmem_max=$target_wmem
  printf 'net.core.rmem_max=$target_rmem\\nnet.core.wmem_max=$target_wmem\\n' | sudo tee $sysctl_file
EOF
)

    # A permission check on /dev/tty is not enough: without a controlling
    # terminal (CI, setsid) the node is "readable" but opening it fails.
    if [[ "$ASSUME_YES" != "1" ]] && ! { : < /dev/tty; } 2>/dev/null; then
        echo "No interactive terminal available; skipping kernel limit change."
        echo "$manual_hint"
        return 0
    fi

    if ! prompt_yes_no "Raise kernel UDP buffer limits now (persisted in $sysctl_file)?"; then
        echo "Skipped."
        echo "$manual_hint"
        return 0
    fi

    local sudo_cmd=""
    if [[ "$(id -u)" -ne 0 ]]; then
        if command -v sudo >/dev/null 2>&1; then
            sudo_cmd="sudo"
        else
            echo "Warning: sudo not available; skipping kernel limit change." >&2
            echo "$manual_hint"
            return 0
        fi
    fi

    if ! printf 'net.core.rmem_max=%s\nnet.core.wmem_max=%s\n' "$target_rmem" "$target_wmem" \
            | $sudo_cmd tee "$sysctl_file" >/dev/null; then
        echo "Warning: could not write $sysctl_file; continuing without it." >&2
        echo "$manual_hint"
        return 0
    fi
    if ! $sudo_cmd sysctl -w "net.core.rmem_max=$target_rmem" "net.core.wmem_max=$target_wmem"; then
        echo "Warning: sysctl apply failed; limits will take effect after reboot via $sysctl_file." >&2
    fi
}

detect_game_path() {
    local candidates=()

    candidates+=("$HOME/.local/share/Steam/steamapps/common/Battlezone 98 Redux")
    candidates+=("$HOME/snap/steam/common/.local/share/Steam/steamapps/common/Battlezone 98 Redux")
    candidates+=("$HOME/.var/app/com.valvesoftware.Steam/data/Steam/steamapps/common/Battlezone 98 Redux")

    for candidate in "${candidates[@]}"; do
        if [[ -f "$candidate/battlezone98redux.exe" ]]; then
            GAME_PATH="$candidate"
            return
        fi
    done
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --game-path)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --game-path" >&2
                exit 1
            fi
            GAME_PATH="$2"
            shift 2
            ;;
        --ref)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --ref" >&2
                exit 1
            fi
            REF="$2"
            validate_ref "$REF"
            ARCHIVE_URL="https://github.com/${REPO_SLUG}/archive/${REF}.tar.gz"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ -z "$GAME_PATH" ]]; then
    detect_game_path
fi

if [[ -z "$GAME_PATH" ]]; then
    echo "Could not find Battlezone 98 Redux automatically." >&2
    echo "Run again with: --game-path '/path/to/Battlezone 98 Redux'" >&2
    exit 1
fi

if [[ ! -f "$GAME_PATH/battlezone98redux.exe" ]]; then
    echo "Game executable not found in: $GAME_PATH" >&2
    exit 1
fi

ensure_build_dependencies

temp_dir="$(mktemp -d)"
archive_file="$temp_dir/source.tar.gz"
trap 'rm -rf "$temp_dir"' EXIT

echo "Downloading source archive from $ARCHIVE_URL"
download_to "$ARCHIVE_URL" "$archive_file"
verify_archive "$archive_file"

echo "Extracting source archive"
tar -xzf "$archive_file" -C "$temp_dir"

source_root="$(find "$temp_dir" -mindepth 1 -maxdepth 1 -type d | head -n1)"
if [[ -z "$source_root" ]]; then
    echo "Failed to locate extracted source directory." >&2
    exit 1
fi

echo "Building Linux Proton proxy from source"
(
    cd "$source_root/Linux/proton_dsound_proxy"
    make clean
    make
)

built_dll="$source_root/Linux/proton_dsound_proxy/build/dsound.dll"
if [[ ! -f "$built_dll" ]]; then
    echo "Build completed, but dsound.dll was not produced." >&2
    exit 1
fi

dest_path="$GAME_PATH/dsound.dll"
if [[ -f "$dest_path" ]]; then
    echo "Deleting existing $dest_path before install"
    rm -f "$dest_path"
fi

echo "Installing patch to $dest_path"
command install -m 0644 "$built_dll" "$dest_path"
rm -f "$GAME_PATH/dsound_proxy.log"

# net.ini send-governor tuning.  The game only loads net.ini through the
# mod system - a copy in the game folder root is silently ignored - so it
# is installed as a local packaged mod.
net_ini_src="$source_root/net-ini/net.ini"
net_ini_dst="$GAME_PATH/packaged_mods/9990001/net.ini"
if [[ -f "$net_ini_src" ]]; then
    mkdir -p "$(dirname "$net_ini_dst")"
    command install -m 0644 "$net_ini_src" "$net_ini_dst"
    echo "Installed net.ini tuning mod to $net_ini_dst"

    # Workshop mods ship their own net.ini and win over the local file, and
    # DISABLING the mod in the in-game manager is not enough - it still loads.
    workshop_net_ini="$(find "$GAME_PATH/../../workshop/content/301650" -mindepth 2 -maxdepth 2 -name net.ini 2>/dev/null | head -n1)"
    if [[ -n "$workshop_net_ini" ]]; then
        echo "WARNING: a Workshop mod also provides net.ini and will override the local file:" >&2
        echo "  $workshop_net_ini" >&2
        echo "Unsubscribe from that mod (disabling it in-game is NOT enough) if you plan to host." >&2
    fi
fi

apply_socket_buffer_sysctls

exu_repair_script="$source_root/Linux/repair_exu_linux.sh"
if [[ -x "$exu_repair_script" ]]; then
    echo "Applying Linux EXU compatibility repair (best effort)"
    if ! "$exu_repair_script" --game-path "$GAME_PATH"; then
        echo "Warning: EXU compatibility repair failed; continuing with netcode patch install." >&2
    fi
fi

# ── Automatic log upload (test crew) ─────────────────────────────────────────
# Zero prompts by design: the tester's whole job is to run one command and
# paste one launch line.  The webhook rides in on BZNET_WEBHOOK, which is
# baked into the install command pinned in the private channel - so the
# credential lives in that channel, never in this public repo.  No
# BZNET_WEBHOOK (i.e. a normal player) means no uploader and no questions.
wrapper_ready=0
wrapper_dir="${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode"
conf_dir="${XDG_CONFIG_HOME:-$HOME/.config}/bz-netcode"
if [[ -n "${BZNET_WEBHOOK:-}" && -f "$source_root/upload/bz_wrap.sh" ]]; then
    if [[ "$BZNET_WEBHOOK" != https://discord.com/api/webhooks/* \
       && "$BZNET_WEBHOOK" != https://discordapp.com/api/webhooks/* ]]; then
        echo "Warning: BZNET_WEBHOOK is not a Discord webhook URL; skipping upload setup." >&2
    else
        mkdir -p "$wrapper_dir" "$conf_dir"
        command cp -f "$source_root/upload/bz_wrap.sh" "$wrapper_dir/bz_wrap.sh"
        chmod +x "$wrapper_dir/bz_wrap.sh"
        (
            umask 077
            cat >"$conf_dir/upload.conf" <<EOF
# Written by install_linux.sh. Do not commit this file.
BZ_WEBHOOK='$BZNET_WEBHOOK'
BZ_PLAYER='${BZNET_PLAYER:-}'
BZ_INCLUDE_PROTON=0
EOF
        )
        chmod 600 "$conf_dir/upload.conf"

        # Sandboxed Steam cannot read the host copy above: Flatpak remaps
        # XDG_DATA_HOME into ~/.var/app, and snapd's home interface excludes
        # dot-dirs outright — which is why the wrapper line used to abort the
        # launch on Snap: the exec target simply did not exist inside the
        # sandbox. Mirror the wrapper into every sandbox that has a Steam,
        # with the conf as a sibling: bz_wrap.sh prefers a sibling
        # upload.conf, so each copy is self-contained.
        for sandbox_dir in \
            "$HOME/snap/steam/common/.local/share/bz-netcode" \
            "$HOME/.var/app/com.valvesoftware.Steam/data/bz-netcode"; do
            [[ -d "${sandbox_dir%/bz-netcode}/Steam" ]] || continue
            mkdir -p "$sandbox_dir"
            command cp -f "$source_root/upload/bz_wrap.sh" "$sandbox_dir/bz_wrap.sh"
            chmod +x "$sandbox_dir/bz_wrap.sh"
            command cp -f "$conf_dir/upload.conf" "$sandbox_dir/upload.conf"
            chmod 600 "$sandbox_dir/upload.conf"
            echo "Mirrored the uploader into $sandbox_dir (sandboxed Steam)."

            # The Steam snap's runtime has neither curl nor python3, so the
            # post-exit upload can only ever park bundles — and the next
            # wrapped launch runs inside the same toolless sandbox, so
            # nothing in there will ever send them. A host-side user timer
            # drains the outbox with host tools instead.
            if [[ "$sandbox_dir" == "$HOME/snap/steam/"* ]] \
               && command -v systemctl >/dev/null 2>&1; then
                unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
                mkdir -p "$unit_dir"
                cat >"$unit_dir/bz-netcode-retry.service" <<UNIT
[Unit]
Description=Send parked Battlezone netcode session bundles

[Service]
Type=oneshot
ExecStart="$sandbox_dir/bz_wrap.sh" --retry
UNIT
                cat >"$unit_dir/bz-netcode-retry.timer" <<UNIT
[Unit]
Description=Drain the Battlezone netcode outbox the snap sandbox cannot send from

[Timer]
OnBootSec=2min
OnUnitActiveSec=10min

[Install]
WantedBy=timers.target
UNIT
                if systemctl --user daemon-reload 2>/dev/null \
                   && systemctl --user enable --now bz-netcode-retry.timer 2>/dev/null; then
                    echo "Enabled bz-netcode-retry.timer: Snap-parked bundles are sent every 10 minutes."
                else
                    echo "Could not enable the retry timer; send parked bundles by hand with:"
                    echo "  \"$sandbox_dir/bz_wrap.sh\" --retry"
                fi
            fi
        done

        # Empty BZ_PLAYER = the wrapper reads the in-game player name from
        # BZLogger at upload time; the OS account name is not who
        # anyone is on Steam.
        echo "Automatic log upload configured for '${BZNET_PLAYER:-your in-game name (read at upload time)}'."
        wrapper_ready=1
    fi
fi

if [[ "$wrapper_ready" == "1" ]]; then
    if [[ "$GAME_PATH" == "$HOME/snap/steam/"* ]]; then
        # Snap remaps HOME and blocks the host's dot-dirs, so the XDG line
        # can never resolve there; $SNAP_USER_COMMON is guaranteed by snapd
        # inside the sandbox and points at ~/snap/steam/common.
        launch_line='WINEDLLOVERRIDES=dsound=n,b "$SNAP_USER_COMMON/.local/share/bz-netcode/bz_wrap.sh" %command% -nointro'
    else
        launch_line='WINEDLLOVERRIDES=dsound=n,b "${XDG_DATA_HOME:-$HOME/.local/share}/bz-netcode/bz_wrap.sh" %command% -nointro'
    fi
else
    launch_line='WINEDLLOVERRIDES=dsound=n,b %command% -nointro'
fi

# State both outcomes on the final lines, where the eye actually lands — a
# skipped or failed uploader setup higher up scrolls away (field-proven on
# Windows, 2026-08-02).
if [[ "$wrapper_ready" == "1" ]]; then
    uploader_status="Log uploader: OK"
elif [[ -n "${BZNET_WEBHOOK:-}" ]]; then
    uploader_status="LOG UPLOADER: NOT INSTALLED - scroll up for the reason"
else
    uploader_status="Log uploader: not requested (no BZNET_WEBHOOK - correct for normal players)"
fi

cat <<EOF

Install complete.
Patch DLL: OK    $uploader_status

Steam launch options still need to be set once on Linux:
$launch_line

(That's all you need: bigger socket buffers, DSCP priority marking, the [Net]
tuning poke, the governor cold-start fix and the host auto-kick relax are on by
default; BZ_AUTOKICK_RELAX=0 restores stock kicking. The inbound reorder buffer
is OFF - it has been since V4.8, and V4.9 found the structural reason: this
protocol's sequence number counts messages, not datagrams, so there is no
per-datagram key to reorder by. BZ_SEND_DUP=1 exists but is deprecated - live A/B testing
showed outbound duplication does not help this game and degrades busy
uplinks by doubling packet rate. Leave it off.)

Installed to:
$dest_path
EOF