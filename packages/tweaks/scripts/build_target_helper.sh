#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Build tweaks_persist against the vendor rootfs glibc/PCL.
#
# The main daemon is static because the head unit's glibc is 2.25 while most
# build hosts are newer.  This small helper is deliberately dynamically linked
# against the vendor glibc and libpersistence_client_library so the static
# daemon can ask it for the current blue-light setting.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
R=${IONIQ_ROOTFS:-${SCOURGE_ROOTFS:-$(dirname "$0")/../../work/rootfs}}
BUILD="$ROOT/build/persist"
OUT="$ROOT/src/tweaks_persist"
CC=${CC:-gcc}

[ -d "$R" ] || { echo "vendor rootfs not found: $R" >&2; exit 1; }
[ -f "$R/usr/lib/libpersistence_client_library.so.7.1.1" ] || {
    echo "PCL not found in $R" >&2; exit 1;
}

rm -rf "$BUILD"
mkdir -p "$BUILD/lib"
ln -sf "$R/lib/libc-2.25.so" "$BUILD/lib/libc.so.6"
ln -sf "$R/lib/libdl-2.25.so" "$BUILD/lib/libdl.so.2"
ln -sf "$R/lib/libpthread-2.25.so" "$BUILD/lib/libpthread.so.0"
ln -sf "$R/lib/libm-2.25.so" "$BUILD/lib/libm.so.6"
ln -sf "$R/lib/librt-2.25.so" "$BUILD/lib/librt.so.1"
ln -sf "$R/usr/lib/libdbus-1.so.3.8.13" "$BUILD/lib/libdbus-1.so.3"
ln -sf "$R/usr/lib/libdlt.so.2.17.0" "$BUILD/lib/libdlt.so.2"
ln -sf "$R/usr/lib/libz.so.1.2.13" "$BUILD/lib/libz.so.1"
ln -sf "$R/usr/lib/libpersistence_client_library.so.7.1.1" \
       "$BUILD/lib/libpersistence_client_library.so.7"

"$CC" -O2 -Wall -fno-stack-protector -c "$ROOT/src/tweaks_persist.c" \
      -o "$BUILD/tweaks_persist.o"

"$CC" -nostdlib -nostartfiles -o "$OUT" \
    "$BUILD/tweaks_persist.o" \
    "$R/usr/lib/Scrt1.o" "$R/usr/lib/crti.o" "$R/usr/lib/crtn.o" \
    -L"$BUILD/lib" -Wl,-rpath-link,"$BUILD/lib" -Wl,--allow-shlib-undefined \
    -l:libpersistence_client_library.so.7 \
    -l:libdlt.so.2 -l:libz.so.1 -l:libdbus-1.so.3 -l:libc.so.6 \
    "$R/usr/lib/libc_nonshared.a" \
    -Wl,--dynamic-linker=/lib/ld-linux-x86-64.so.2

# Keep the vendor loader permissions out of the binary; strip build paths.
if command -v strip >/dev/null 2>&1; then
    strip --strip-unneeded "$OUT" 2>/dev/null || true
fi

echo "wrote $OUT"
