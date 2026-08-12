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

# --- installers must not hardcode a hash ------------------------------------
# install_windows.ps1 reads prebuilt/windows/winmm.dll.sha256 at run time
# instead of carrying a literal, so a prebuilt refresh cannot strand people
# running a cached copy of the script.  Reintroducing a literal would bring
# back the 2026-07-26 breakage, so flag any that appears.  BZNET_WINMM_SHA256
# remains available to callers who want strict pinning.
for script in install/install_windows.ps1 install/install_linux.sh; do
    path="$REPO_ROOT/$script"
    [[ -f "$path" ]] || continue
    # Ignore comment lines: the rationale text mentions hashes by name.
    # -i: an uppercase literal is exactly as broken and used to slip through.
    literal="$(grep -vE '^\s*(#)' "$path" | grep -oiE '[0-9a-f]{64}' | head -1 || true)"
    if [[ -n "$literal" ]]; then
        fail "$script hardcodes a SHA256 ($literal)
  A refresh of the prebuilt will strand anyone running a cached copy of this
  script. Read the .sha256 sidecar at run time instead."
    else
        echo "ok: $script carries no hardcoded hash"
    fi
done

if [[ $status -ne 0 ]]; then
    echo >&2
    echo "Prebuilt pin check failed. Do not push: the installer would reject" >&2
    echo "the very binary this repo serves." >&2
fi
exit $status
