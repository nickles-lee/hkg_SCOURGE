#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 08_build_usb_image.sh — build the 27 GB vanilla usb.img for the TOCTOU rig
# + emit regions.json pointing at out/stage1.te2.
#
# The image is 100% genuine firmware; only the rootfs byte region is served
# dynamically by the Pi (docs/01, docs/04). Needs sudo/loop.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"
need_venv
need_fetched

STOCK="$STOCK_PACKAGE"
EVIL="$OUT_DIR/stage1.te2"
IMG="$OUT_DIR/usb.img"
PIZERO="$TOCTOU_REPO/pizero"

[ -d "$STOCK" ] || { echo "missing $STOCK (run 01_stage_inputs.sh)" >&2; exit 1; }
[ -f "$EVIL" ] || { echo "missing $EVIL (run 04_repack_rootfs.sh)" >&2; exit 1; }

echo "== building $IMG (needs sudo/loop; ~27 GB)"
sudo "$PIZERO/02_build_usb_image.sh" "$STOCK" "$EVIL" "$IMG" exfat

echo "== regions.json (switch_after_pass=2, benign_then_evil)"
rm -f "$OUT_DIR/regions.json"
"$PYTHON" - "$PIZERO/build/regions.json" "$EVIL" "$OUT_DIR/regions.json" <<'PY'
import json, sys
src, evil, dst = sys.argv[1:4]
r = json.load(open(src))
for reg in r:
    reg["evil"] = evil
    reg["switch_after_pass"] = 2
    reg["mode"] = "benign_then_evil"
json.dump(r, open(dst, "w"), indent=2)
print(json.dumps(r, indent=2))
PY

echo "== verify the region maps to the genuine rootfs"
OFF="$($PYTHON -c 'import json,sys; r=json.load(open(sys.argv[1]))[0]; print(r["off"])' "$OUT_DIR/regions.json")"
LEN="$($PYTHON -c 'import json,sys; r=json.load(open(sys.argv[1]))[0]; print(r["len"])' "$OUT_DIR/regions.json")"
echo "off=$OFF len=$LEN"
cmp <(dd if="$IMG" bs=1 skip="$OFF" count=16 2>/dev/null) \
    <(dd if="$STOCK/HU/images/mango-rootfs.tar.gz" bs=1 count=16 2>/dev/null) \
    && echo "extent OK"

echo "== hashes (compare against docs/03-stage1-build.md)"
sha256sum "$IMG" "$OUT_DIR/regions.json"
