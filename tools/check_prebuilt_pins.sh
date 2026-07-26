#!/usr/bin/env bash
# Fail if a shipped prebuilt binary, its .sha256 sidecar, and the hash pinned
# inside an installer have drifted apart.
#
# Why this exists: on 2026-07-26 a prebuilt refresh updated
# prebuilt/windows/winmm.dll and its sidecar but not the pin at
# install/install_windows.ps1:12.  The installer downloads the DLL from
# raw.githubusercontent.com and compares it against that pin, so every Windows
# install broke on the next pull with "Downloaded winmm.dll hash mismatch".
# Nothing in the build or the test suite could have caught it.
#
# Run this after any prebuilt refresh, before pushing.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
status=0

fail() { echo "FAIL: $*" >&2; status=1; }

# --- prebuilt binaries vs their sidecars ------------------------------------
# The sidecar is written by `sha256sum <name> > <name>.sha256` from inside the
# directory, so it holds a bare filename, not a path.
for dll in prebuilt/linux/dsound.dll prebuilt/windows/winmm.dll; do
    path="$REPO_ROOT/$dll"
    side="$path.sha256"
    if [[ ! -f "$path" ]]; then fail "$dll is missing"; continue; fi
    if [[ ! -f "$side" ]]; then fail "$dll.sha256 is missing"; continue; fi

    actual="$(sha256sum "$path" | cut -d' ' -f1)"
    recorded="$(cut -d' ' -f1 < "$side")"
    if [[ "$actual" != "$recorded" ]]; then
        fail "$dll.sha256 is stale
    binary:   $actual
    sidecar:  $recorded
  Fix: (cd $(dirname "$dll") && sha256sum $(basename "$dll") > $(basename "$dll").sha256)"
    else
        echo "ok: $dll matches its sidecar"
    fi
done

# --- installer pins vs the binaries they will download ----------------------
# install_linux.sh pins nothing today; if it ever grows a pin, add it here.
win_dll="$REPO_ROOT/prebuilt/windows/winmm.dll"
ps1="$REPO_ROOT/install/install_windows.ps1"
if [[ -f "$win_dll" && -f "$ps1" ]]; then
    actual="$(sha256sum "$win_dll" | cut -d' ' -f1)"
    # The fallback literal in: $expectedHash = if (...) { ... } else { "<hash>" }
    pinned="$(grep -oE '"[0-9a-f]{64}"' "$ps1" | tr -d '"' | head -1)"
    if [[ -z "$pinned" ]]; then
        fail "no 64-hex pin found in install/install_windows.ps1 — did the line move?"
    elif [[ "$pinned" != "$actual" ]]; then
        fail "install/install_windows.ps1 pin is stale — Windows installs will break
    prebuilt: $actual
    pinned:   $pinned
  Fix: set \$expectedHash in install/install_windows.ps1 to the prebuilt hash."
    else
        echo "ok: install_windows.ps1 pin matches prebuilt/windows/winmm.dll"
    fi
fi

if [[ $status -ne 0 ]]; then
    echo >&2
    echo "Prebuilt pin check failed. Do not push: the installer would reject" >&2
    echo "the very binary this repo serves." >&2
fi
exit $status
