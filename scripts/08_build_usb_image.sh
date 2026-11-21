#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 08_build_usb_image.sh — build the 27 GB vanilla usb.img for the TOCTOU rig
# + emit regions.json pointing at out/stage1.te2.
#
# The image is 100% genuine firmware; only the rootfs byte region is served
# dynamically by the Pi (docs/01, docs/04). Needs sudo/loop.
#
# The assembly runs here instead of through upstream's
# pizero/02_build_usb_image.sh: that script copies the package with `cp -a` /
# `rsync -a`, which abort on a kernel exFAT mount because ownership and mode
# cannot be stored there (every chown returns EPERM, root included), and the
# failed copy takes `set -e` down with it. The sequence below is the same one —
# image size PKGSZ + PKGSZ/7 + 64 MiB, mkfs.exfat, the rootfs written first so
# it lands in one contiguous extent, then the rest — with the unstorable
# attributes dropped and timestamps kept.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"
need_venv

STOCK="$STOCK_PACKAGE"
EVIL="$OUT_DIR/stage1.te2"
IMG="$OUT_DIR/usb.img"
MANGO_REL="HU/images/mango-rootfs.tar.gz"
LABEL="IONIQ5_EU"

[ -d "$STOCK" ] || { echo "missing $STOCK (run 01_stage_inputs.sh)" >&2; exit 1; }
[ -f "$EVIL" ] || { echo "missing $EVIL (run 04_repack_rootfs.sh)" >&2; exit 1; }
[ -f "$STOCK/$MANGO_REL" ] || { echo "missing $STOCK/$MANGO_REL" >&2; exit 1; }

PKGSZ="$(du -sb "$STOCK" | awk '{print $1}')"
IMGSZ=$(( PKGSZ + PKGSZ/7 + 64*1024*1024 ))
echo "== building $IMG ($IMGSZ B, exFAT label $LABEL; needs sudo/loop)"
rm -f "$IMG"
truncate -s "$IMGSZ" "$IMG"
mkfs.exfat -n "$LABEL" "$IMG" >/dev/null

MP="$(mktemp -d)"
cleanup() {
    sudo umount "$MP" 2>/dev/null || true
    rmdir "$MP" 2>/dev/null || true
}
trap cleanup EXIT
sudo mount -o loop "$IMG" "$MP"

echo "== writing $MANGO_REL first (one contiguous extent = the swap region)"
sudo mkdir -p "$MP/$(dirname "$MANGO_REL")"
sudo cp --reflink=never --preserve=timestamps --no-preserve=ownership,mode \
    "$STOCK/$MANGO_REL" "$MP/$MANGO_REL"
sync

echo "== copying the rest of the package"
( cd "$STOCK" && sudo rsync -rlDt --exclude="$MANGO_REL" ./ "$MP"/ )
sync

# FIEMAP is unimplemented for exFAT, so filefrag has to fall back to FIBMAP,
# which needs root; the physical offset comes back in 512-byte units.
FRAG="$(sudo filefrag -b512 -e "$MP/$MANGO_REL")"
echo "$FRAG" | sed -n '1,4p'
EXTENTS="$(echo "$FRAG" | grep -c '^[[:space:]]*[0-9]*:')"
PHYS512="$(echo "$FRAG" | awk -F: '/^[[:space:]]*0:/{gsub(/[^0-9.]/,"",$3); split($3,a,"."); print a[1]; exit}')"
LEN="$(stat -c%s "$MP/$MANGO_REL")"
cleanup; trap - EXIT

[ -n "$PHYS512" ] || { echo "ERROR: no first extent for $MANGO_REL in filefrag output" >&2; exit 1; }
[ "$LEN" = "$GENUINE_ENVELOPE_SIZE" ] || {
    echo "ERROR: $MANGO_REL is $LEN B, expected $GENUINE_ENVELOPE_SIZE" >&2; exit 1; }
[ "$EXTENTS" -eq 1 ] || {
    echo "ERROR: $MANGO_REL occupies $EXTENTS extents; the plugin swaps one contiguous region." >&2
    echo "  Rebuild on a fresh image, or extend regions.json with every extent." >&2
    exit 1; }
OFF=$(( PHYS512 * 512 ))

echo "== regions.json (switch_after_pass=2, benign_then_evil)"
rm -f "$OUT_DIR/regions.json"
"$PYTHON" - "$OFF" "$LEN" "$EVIL" "$OUT_DIR/regions.json" <<'PY'
import json, sys
off, length, evil, dst = sys.argv[1:5]
regions = [{
    "off": int(off),
    "len": int(length),
    "evil": evil,
    "switch_after_pass": 2,
    "mode": "benign_then_evil",
}]
with open(dst, "w") as fh:
    json.dump(regions, fh, indent=2)
    fh.write("\n")
print(json.dumps(regions, indent=2))
PY

echo "== verify the region holds the genuine rootfs (off=$OFF len=$LEN)"
cmp -n "$LEN" \
    <(dd if="$IMG" iflag=skip_bytes,count_bytes bs=1M skip="$OFF" count="$LEN" 2>/dev/null) \
    "$STOCK/$MANGO_REL" && echo "extent OK"

# The artifacts are created as root when this runs under sudo; hand them back to
# the invoking user so the later steps (13_copy_image_to_sd.sh, deploys) can read
# and replace them without another escalation.
if [ -n "${SUDO_UID:-}" ] && [ -n "${SUDO_GID:-}" ]; then
    chown "$SUDO_UID:$SUDO_GID" "$IMG" "$OUT_DIR/regions.json"
fi

echo "== hashes (compare against docs/03-stage1-build.md)"
sha256sum "$IMG" "$OUT_DIR/regions.json"
