#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# build_dropbear.sh — build the static-musl dropbearmulti for the WiFi package.
#
# TOOLCHAIN POLICY (docs/07-packages.md): the head unit runs kernel 4.1.27
# with glibc 2.25, so a dynamically-linked binary built on a modern host
# dies on-car on symbol versions. A FULLY STATIC musl build has zero dynamic
# dependencies and runs on the vendor kernel — this is the validated recipe
# (upstream TOCTOU repo docs/07 §3 background; this script is first-party).
#
# Build environments, in order of preference:
#   1. Pinned Alpine container (docker or podman, auto-detected) — most
#      reproducible; no host contamination.
#   2. Host musl-gcc (Debian/Ubuntu musl-tools) via MUSL_GCC=<path>.
#   3. --use-existing <path> — accept an already-built static dropbearmulti
#      (it still passes the static-link gate below).
#
# Usage: build_dropbear.sh [--use-existing /path/to/dropbearmulti]
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")     # packages/wifi
OUT="$ROOT/bin"
DROPBEAR_VERSION="DROPBEAR_2024.86"

existing=""
[ "${1:-}" = "--use-existing" ] && existing="${2:?--use-existing needs a path}"

mkdir -p "$OUT" "$ROOT/build/dropbear"

check_static() {
    # The gate: must be an x86-64 statically linked ELF.
    file "$1" | grep -q "ELF 64-bit LSB executable, x86-64" || {
        echo "ERROR: $1 is not an x86-64 ELF" >&2; exit 1; }
    if file "$1" | grep -q "dynamically linked"; then
        echo "ERROR: $1 is DYNAMICALLY linked — it will not run on the head" >&2
        echo "  unit's glibc 2.25. Build it static against musl (see the" >&2
        echo "  toolchain policy in docs/07-packages.md)." >&2
        exit 1
    fi
    echo "static-link gate OK: $1"
}

if [ -n "$existing" ]; then
    check_static "$existing"
    cp -f "$existing" "$OUT/dropbearmulti"
else
    RT="${CONTAINER_RUNTIME:-}"
    [ -z "$RT" ] && command -v docker >/dev/null 2>&1 && RT=docker
    [ -z "$RT" ] && command -v podman >/dev/null 2>&1 && RT=podman

    if [ -n "$RT" ]; then
        echo "== building in a pinned Alpine container ($RT)"
        "$RT" run --rm -v "$ROOT/build/dropbear:/src" alpine:3.20 sh -exc '
            apk add --no-cache build-base git
            cd /src
            [ -d dropbear ] || git clone --depth 1 --branch '"$DROPBEAR_VERSION"' https://github.com/mkj/dropbear
            cd dropbear
            ./configure CC=musl-gcc --disable-zlib --disable-syslog --disable-harden LDFLAGS=-static
            make PROGRAMS="dropbear dbclient dropbearkey scp" MULTI=1 STATIC=1
            file dropbearmulti
        '
        BIN="$ROOT/build/dropbear/dropbear/dropbearmulti"
    elif command -v musl-gcc >/dev/null 2>&1; then
        echo "== building with host musl-gcc"
        cd "$ROOT/build/dropbear"
        [ -d dropbear ] || git clone --depth 1 --branch "$DROPBEAR_VERSION" https://github.com/mkj/dropbear
        cd dropbear
        ./configure CC=musl-gcc --disable-zlib --disable-syslog --disable-harden LDFLAGS=-static
        make PROGRAMS="dropbear dbclient dropbearkey scp" MULTI=1 STATIC=1
        BIN="$PWD/dropbearmulti"
    else
        echo "ERROR: no container runtime and no musl-gcc on the host." >&2
        echo "  Install docker or podman (recommended), or musl-tools," >&2
        echo "  or pass --use-existing /path/to/dropbearmulti." >&2
        exit 1
    fi

    check_static "$BIN"
    cp -f "$BIN" "$OUT/dropbearmulti"
fi

sha256sum "$OUT/dropbearmulti"
echo "== smoke test (host-executable: x86-64 static, our own build)"
"$OUT/dropbearmulti" 2>&1 | head -2 || true
echo "wrote $OUT/dropbearmulti — record its sha256 in the package's"
echo "MANIFEST when building the USB tree (scripts/build_payload.sh)."
