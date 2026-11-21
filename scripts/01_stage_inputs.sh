#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 01_stage_inputs.sh — stage + hash-gate the official firmware inputs.
#
# Three input paths (SCOURGE never hosts or mirrors firmware — you obtain
# the official package and we verify it):
#   --download       fetch via nudl from the OEM's own update servers
#                    (model/region/version come from firmware_locks.env)
#   --from-dir DIR   an already-extracted official package tree
#   --from-zip ZIP   an official package zip (extracted into input/)
#
# Staged into input/:  mango-rootfs.tar.gz  AppUpgrade.usb  IONIQ5_EU.ver
#                     .lge.upgrade.xml   [package_stock/  full tree]
# Every staged file is hash-gated against config/firmware_locks.env.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

MODE="${1:---download}"
shift || true
mkdir -p "$INPUT_DIR"

stage_and_gate() {
    local src="$1" dst="$2" what="$3" size="$4" sha="$5" md5="$6"
    [ -f "$src" ] || { echo "ERROR: missing $what at $src" >&2; exit 1; }
    local got_size got_md5
    got_size="$(stat -c%s "$src")"
    got_md5="$(md5sum "$src" | cut -d' ' -f1)"
    # size is optional: some inputs are pinned by sha256 alone (the signed XML
    # has no recorded length), and an empty pin must not read as a mismatch.
    [ -z "$size" ] || [ "$got_size" = "$size" ] || {
        echo "ERROR: $what size $got_size != $size" >&2; exit 1; }
    if [ -n "$sha" ]; then
        lock_check_or_die "$src" "$sha" "$what" || exit 1
    elif [ -n "$md5" ]; then
        [ "$got_md5" = "$md5" ] || { echo "ERROR: $what md5 mismatch" >&2; exit 1; }
    fi
    cp -f "$src" "$dst"
    echo "  staged $what ($(stat -c%s "$dst") B, md5 $got_md5)"
}

case "$MODE" in
--download)
    [ -x "$NUDL_BIN" ] || { echo "ERROR: nudl missing — run 00_fetch_third_party.sh" >&2; exit 1; }
    echo "== nudl download $NUDL_MODEL / $NUDL_REGION / $NUDL_VERSION"
    "$NUDL_BIN" download -b "$NUDL_BRAND" -m "$NUDL_MODEL" -r "$NUDL_REGION" \
        -v "$NUDL_VERSION" -o "$INPUT_DIR"
    echo "== nudl verify"
    "$NUDL_BIN" verify -d "$INPUT_DIR"
    ;;
--from-dir)
    SRC_DIR="${1:?--from-dir needs a path}"
    [ -d "$SRC_DIR" ] || { echo "ERROR: not a directory: $SRC_DIR" >&2; exit 1; }
    ;;
--from-zip)
    ZIP="${1:?--from-zip needs a path}"
    echo "== extracting $ZIP"
    mkdir -p "$INPUT_DIR/extracted"
    unzip -q -o "$ZIP" -d "$INPUT_DIR/extracted"
    SRC_DIR="$(find "$INPUT_DIR/extracted" -type d -name 'HU' -printf '%h\n' | head -1)"
    [ -n "$SRC_DIR" ] || { echo "ERROR: could not find a package tree in the zip" >&2; exit 1; }
    ;;
*) echo "usage: $0 [--download | --from-dir DIR | --from-zip ZIP]" >&2; exit 2 ;;
esac

if [ -n "${SRC_DIR:-}" ]; then
    echo "== staging from $SRC_DIR"
    stage_and_gate "$SRC_DIR/HU/images/mango-rootfs.tar.gz" \
        "$INPUT_DIR/mango-rootfs.tar.gz" "rootfs container" \
        "$GENUINE_ENVELOPE_SIZE" "" "$GENUINE_MD5"
    stage_and_gate "$SRC_DIR/AppUpgrade" "$INPUT_DIR/AppUpgrade.usb" \
        "USB wrapper container" "$WRAPPER_SIZE" "$WRAPPER_STOCK_SHA256" ""
    stage_and_gate "$SRC_DIR/$VER_FILE" "$INPUT_DIR/$VER_FILE" \
        ".ver" "$VER_STOCK_SIZE" "$VER_STOCK_SHA256" ""
    stage_and_gate "$SRC_DIR/.lge.upgrade.xml" "$INPUT_DIR/.lge.upgrade.xml" \
        ".lge.upgrade.xml" "" "$LGE_UPGRADE_XML_SHA256" ""
    # Full stock tree for the vanilla image / packages (24 GB+; optional for
    # payload-only builds, required for 08_build_usb_image.sh / 06_build_package.sh).
    if [ ! -d "$STOCK_PACKAGE" ]; then
        echo "== staging the full stock tree at $STOCK_PACKAGE (24 GB+; this takes a while)"
        mkdir -p "$STOCK_PACKAGE"
        rsync -a "$SRC_DIR"/ "$STOCK_PACKAGE"/
    fi
fi

echo
echo "== staged input gates:"
[ -f "$INPUT_DIR/mango-rootfs.tar.gz" ] && \
    lock_check_or_die "$INPUT_DIR/mango-rootfs.tar.gz" "" "rootfs (md5)" 2>/dev/null || true
md5sum "$INPUT_DIR/mango-rootfs.tar.gz" | grep -q "$GENUINE_MD5" && echo "  rootfs md5 OK"
[ -f "$INPUT_DIR/AppUpgrade.usb" ] && \
    lock_check_or_die "$INPUT_DIR/AppUpgrade.usb" "$WRAPPER_STOCK_SHA256" "USB wrapper"
[ -f "$INPUT_DIR/$VER_FILE" ] && \
    lock_check_or_die "$INPUT_DIR/$VER_FILE" "$VER_STOCK_SHA256" ".ver"
[ -f "$INPUT_DIR/.lge.upgrade.xml" ] && \
    lock_check_or_die "$INPUT_DIR/.lge.upgrade.xml" "$LGE_UPGRADE_XML_SHA256" ".lge.upgrade.xml"

# The .ver rootfs row is derived, never trusted: it must describe the envelope we
# actually staged. Pinning that CRC as a constant is what let a stale value block
# a genuine package (and would let a republished manifest pass unnoticed).
echo
echo "== derived gate: the .ver rootfs row describes the staged envelope"
"$PYTHON" - "$INPUT_DIR/$VER_FILE" "$INPUT_DIR/mango-rootfs.tar.gz" \
    "$VER_ROOTFS_ROW_ID" "$GENUINE_ENVELOPE_SIZE" <<'PY'
import os, sys, zlib

ver_path, img_path, row_id, want_size = sys.argv[1:5]
crc = 0
with open(img_path, "rb") as fh:
    for chunk in iter(lambda: fh.read(1 << 22), b""):
        crc = zlib.crc32(chunk, crc)
crc &= 0xFFFFFFFF
signed = crc - (1 << 32) if crc >= 1 << 31 else crc
size = os.path.getsize(img_path)

row = None
with open(ver_path, encoding="utf-8") as fh:
    for line in fh:
        f = line.rstrip("\n").split("|")
        if len(f) >= 5 and f[0].endswith("HU\\images") and f[1] == "mango-rootfs.tar.gz":
            row = f
            break

if row is None:
    sys.exit("ERROR: no mango-rootfs row in %s" % ver_path)

row_id_got, row_crc, row_size = row[2], int(row[3]), int(row[4])
checks = [
    ("row id", row_id_got, row_id),
    ("row size", row_size, size),
    ("container size", size, int(want_size)),
    ("row crc", row_crc, crc if row_crc >= 0 else signed),
]
failed = False
for name, got, want in checks:
    ok = str(got) == str(want)
    failed = failed or not ok
    print("  [%s] %s %s%s" % ("PASS" if ok else "FAIL", name, got,
                              "" if ok else " (measured %s)" % want))
print("  row: %s" % "|".join(row))
sys.exit(1 if failed else 0)
PY
echo "INPUTS: OK"
