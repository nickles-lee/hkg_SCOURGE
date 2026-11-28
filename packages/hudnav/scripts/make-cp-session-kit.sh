#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# Build the SESSION-ONLY CarPlay test kit (host / WSL, run as root).
# Nothing here touches the car. The decrypted rootfs is READ-ONLY: we copy
# /app/bin/iAP2Service-2.0 out of it and patch only that COPY with patchelf.
# The car-side script bind-mounts the copy over /app/bin/iAP2Service-2.0 for one
# session; a reboot always restores stock and no /etc/ld.so.preload is used.
# IMPORTANT: the car's /rw_data/hudnav/bin/SHA256SUMS is shared with the AA kit,
# so this builder writes ONLY SHA256SUMS.cp and never SHA256SUMS.
#
# Usage: sh scripts/make-cp-session-kit.sh [output-dir]   (default out/cp-session)
set -eu
cd "$(dirname "$0")/.."

R="${ROOTFS:-}"
PIN=e663d5129177a8bd241cb875e36ec5e507d39a2c8111216acafc272bdc3fa88b
NEEDED=/rw_data/hudnav/bin/cp_rg.so
OUT=${1:-out/cp-session}
LIBSRC=out/cp_rg.so

die() { echo "ERROR: $*" >&2; exit 1; }
[ -n "$R" ] || die "set ROOTFS to your own decrypted stock rootfs directory"

# 1. Pin the stock binary BEFORE copying anything.
[ -f "$R/app/bin/iAP2Service-2.0" ] || die "missing $R/app/bin/iAP2Service-2.0"
got=$(sha256sum "$R/app/bin/iAP2Service-2.0" | cut -d' ' -f1)
[ "$got" = "$PIN" ] || die "stock iAP2Service-2.0 sha256 $got != pin $PIN"
echo "stock iAP2Service-2.0 pin OK: $got"

# 2. The library must exist and stay loadable by iAP2Service (no libdl there).
[ -f "$LIBSRC" ] || die "missing $LIBSRC (build it with: sh scripts/build.sh)"
if readelf --dyn-syms -W "$LIBSRC" 2>/dev/null | grep -q ' dlsym'; then
  die "$LIBSRC references dlsym; iAP2Service has no libdl"
fi
bad=$(readelf -d "$LIBSRC" 2>/dev/null | grep NEEDED | grep -vcE 'libc\.so\.6|ld-linux-x86-64\.so\.2' || true)
[ "$bad" = 0 ] || die "$LIBSRC has unexpected NEEDED entries (see readelf -d)"
echo "library OK: $LIBSRC (no dlsym, only libc NEEDED)"

mkdir -p "$OUT"

# 3. Patch a COPY only. patchelf prepends the new DT_NEEDED entry.
cp "$R/app/bin/iAP2Service-2.0" "$OUT/iAP2Service-2.0.cp"
patchelf --add-needed "$NEEDED" "$OUT/iAP2Service-2.0.cp"
chmod 755 "$OUT/iAP2Service-2.0.cp"

# 4. Fail loudly unless the absolute-path library is the FIRST NEEDED entry.
first=$(readelf -d "$OUT/iAP2Service-2.0.cp" 2>/dev/null | grep -m1 NEEDED || true)
case "$first" in
  *"[$NEEDED]"*) echo "first NEEDED OK: $first" ;;
  *) die "first NEEDED is not $NEEDED: ${first:-<none>}" ;;
esac
count=$(readelf -d "$OUT/iAP2Service-2.0.cp" 2>/dev/null | grep -c -F "[$NEEDED]" || true)
[ "$count" = 1 ] || die "expected exactly one absolute cp_rg.so NEEDED, got $count"

# 5. Ship the gated library built by scripts/build.sh (VM-proven load path).
cp "$LIBSRC" "$OUT/cp_rg.so"
chmod 755 "$OUT/cp_rg.so"

# 6. The car-side driver ships in the kit too. Build must fail on CRLF.
cp scripts/cp-session.sh "$OUT/cp-session.sh"
if grep -q "$(printf '\r')" "$OUT/cp-session.sh"; then
  die "cp-session.sh has CRLF line endings; run: sed -i 's/\\r\$//' scripts/cp-session.sh"
fi
chmod 755 "$OUT/cp-session.sh"

# 7. Checksums are relative to the kit directory so `cd bin && sha256sum -c SHA256SUMS.cp`
#    works when the folder is copied to /rw_data/hudnav/bin on the car.
#    NEVER write SHA256SUMS: that file belongs to the AA kit on the car.
( cd "$OUT" && sha256sum iAP2Service-2.0.cp cp_rg.so > SHA256SUMS.cp )
echo "cp-session kit: $OUT"
cat "$OUT/SHA256SUMS.cp"
echo "wrote SHA256SUMS.cp only (the car's SHA256SUMS is shared with the AA kit)"
echo CP_SESSION_KIT_DONE
