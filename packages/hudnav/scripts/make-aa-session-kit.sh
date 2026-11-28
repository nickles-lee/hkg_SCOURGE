#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# Build the SESSION-ONLY Android Auto test kit (host / WSL, run as root).
# Nothing here touches the car. The decrypted rootfs is READ-ONLY: we copy
# /app/bin/AppProjection out of it and patch only that COPY with patchelf.
# The car-side script bind-mounts the copy over /app/bin/AppProjection for one
# session; a reboot always restores stock and no /etc/ld.so.preload is used.
#
# Usage: sh scripts/make-aa-session-kit.sh [output-dir]   (default out/aa-session)
set -eu
cd "$(dirname "$0")/.."

R="${ROOTFS:-}"
PIN=726c69ce627e9183771d11bdfac9acf9d92286a9b6c1a0d88bfbd6b5ac5a7d50
NEEDED=/rw_data/hudnav/bin/aa_shim.so
OUT=${1:-out/aa-session}
CC="${CC:-gcc}"

die() { echo "ERROR: $*" >&2; exit 1; }
[ -n "$R" ] || die "set ROOTFS to your own decrypted stock rootfs directory"

# 1. Pin the stock binary BEFORE copying anything.
[ -f "$R/app/bin/AppProjection" ] || die "missing $R/app/bin/AppProjection"
got=$(sha256sum "$R/app/bin/AppProjection" | cut -d' ' -f1)
[ "$got" = "$PIN" ] || die "stock AppProjection sha256 $got != pin $PIN"
echo "stock AppProjection pin OK: $got"

mkdir -p "$OUT"

# 2. Patch a COPY only. patchelf prepends the new DT_NEEDED entry.
cp "$R/app/bin/AppProjection" "$OUT/AppProjection.aa"
patchelf --add-needed "$NEEDED" "$OUT/AppProjection.aa"
chmod 755 "$OUT/AppProjection.aa"

# 3. Fail loudly unless the absolute-path shim is the FIRST NEEDED entry.
first=$(readelf -d "$OUT/AppProjection.aa" 2>/dev/null | grep -m1 NEEDED || true)
case "$first" in
  *"[$NEEDED]"*) echo "first NEEDED OK: $first" ;;
  *) die "first NEEDED is not $NEEDED: ${first:-<none>}" ;;
esac
# Also fail if patchelf produced an entry that points anywhere else first.
count=$(readelf -d "$OUT/AppProjection.aa" 2>/dev/null | grep -c '\[/rw_data/hudnav/bin/aa_shim.so\]' || true)
[ "$count" = 1 ] || die "expected exactly one absolute aa_shim.so NEEDED, got $count"

# 4. The existing gated shim from the car kit (T11/T11b proven artefact).
[ -f out/car-kit/aa_shim.so ] || die "missing out/car-kit/aa_shim.so (build it with make-car-kit.sh)"
cp out/car-kit/aa_shim.so "$OUT/aa_shim.so"
chmod 755 "$OUT/aa_shim.so"

# 5. STEP 2 DECISION: the 260319 rootfs already ships chsmack -e/--exec
#    (security.SMACK64EXEC) and setfattr/getfattr/attr, so no extra static
#    labelling binary is needed. Build one only if the source is ever supplied.
EXTRA=""
if [ -f src/hudnav_setlabel.c ]; then
  $CC -static -O2 -o "$OUT/hudnav_setlabel" src/hudnav_setlabel.c
  file "$OUT/hudnav_setlabel" | grep -q 'statically linked' || die "hudnav_setlabel is not statically linked"
  EXTRA="hudnav_setlabel"
  echo "built static labelling helper: $OUT/hudnav_setlabel"
else
  echo "no src/hudnav_setlabel.c: rootfs chsmack/setfattr supply labelling (FACT, step 1)"
fi

# 6. The car-side driver ships in the kit too. Build must fail on CRLF.
cp scripts/aa-session.sh "$OUT/aa-session.sh"
for s in aa-session.sh; do
  if grep -q "$(printf '\r')" "$OUT/$s"; then
    die "$s has CRLF line endings; run: sed -i 's/\\r\$//' scripts/$s"
  fi
done
chmod 755 "$OUT/aa-session.sh"

# 7. Checksums are relative to the kit directory so `cd bin && sha256sum -c` works
#    when the folder is copied to /rw_data/hudnav/bin on the car.
( cd "$OUT" && sha256sum AppProjection.aa aa_shim.so $EXTRA > SHA256SUMS )
echo "aa-session kit: $OUT"
cat "$OUT/SHA256SUMS"
echo "AA_SESSION_KIT_DONE"
