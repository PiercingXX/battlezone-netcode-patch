#!/usr/bin/env bash
# Cross-build the Proton DSOUND proxy (32-bit Windows DLL) in a rootless podman
# sandbox — no host toolchain, no sudo. This is the T2 cross-build GATE for
# send-path proxy changes: host tests cannot reach the WinSock hook, so "it
# compiles under the real i686-w64-mingw32 toolchain" is the offline check that
# a live BZ_SEND_DAMPEN run is later validated against.
#
#   ./crossbuild.sh            # build build/dsound.dll
#   ./crossbuild.sh clean      # make clean
#
# The toolchain image is built once (Containerfile.mingw) and cached.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "$here/../.." && pwd)"
img="skippy-mingw32:latest"

if ! command -v podman >/dev/null 2>&1; then
    echo "crossbuild: podman not found — this is the sandboxed toolchain path" >&2
    exit 1
fi
if ! podman image exists "$img"; then
    echo "crossbuild: building toolchain image $img (one-time)..."
    podman build -t "$img" -f "$here/Containerfile.mingw" "$here"
fi
# Mount the repo root so the Makefile's ../../shared resolves; build/ lands in
# the proxy dir owned by the invoking user (rootless UID mapping).
exec podman run --rm \
    -v "$repo_root":/work \
    -w /work/Linux/proton_dsound_proxy \
    "$img" make "$@"
