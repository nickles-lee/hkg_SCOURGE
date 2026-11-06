#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 11_deploy_pi.sh — deploy the built artifacts to the Pi and install the
# auto-arm unit. Host/user come from config/paths.local.env (PI_HOST/PI_USER).
#
#   11_deploy_pi.sh [--skip-image] [--start]
#
# The 27 GB usb.img transfer over the Pi's WiFi is ~44 h at 0.17 MB/s — use
# --skip-image after scripts/13_copy_image_to_sd.sh has placed it on the SD
# card directly (docs/04-pi-rig.md).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"
need_fetched

[ -n "$PI_HOST" ] || { echo "set PI_HOST/PI_USER in config/paths.local.env" >&2; exit 1; }

SKIP_IMAGE=0
START=0
for arg in "$@"; do
    case "$arg" in
        --skip-image) SKIP_IMAGE=1 ;;
        --start) START=1 ;;
    esac
done

[ -f "$OUT_DIR/usb.img" ] || { echo "missing $OUT_DIR/usb.img (run 08_build_usb_image.sh)" >&2; exit 1; }
[ -f "$OUT_DIR/stage1.te2" ] || { echo "missing $OUT_DIR/stage1.te2" >&2; exit 1; }
[ -f "$OUT_DIR/regions.json" ] || { echo "missing $OUT_DIR/regions.json" >&2; exit 1; }

echo "== regions.json for the Pi"
"$PYTHON" - "$OUT_DIR/regions.json" "$OUT_DIR/regions.pi.json" <<PY
import json, sys
src, dst = sys.argv[1:3]
r = json.load(open(src))
for reg in r:
    reg["evil"] = "/home/$PI_USER/$PI_DIR/build/stage1.te2"
json.dump(r, open(dst, "w"), indent=2)
print(json.dumps(r, indent=2))
PY

echo "== directories"
ssh "$PI_USER@$PI_HOST" "mkdir -p ~/$PI_DIR/build ~/$PI_DIR/pizero"

echo "== pizero scripts"
rsync -a "$TOCTOU_REPO/pizero/" "$PI_USER@$PI_HOST:~/$PI_DIR/pizero/"

if [ "$SKIP_IMAGE" = 0 ]; then
    echo "== artifacts (27 GB usb.img + envelope + regions)"
    rsync -a --progress "$OUT_DIR/usb.img" "$OUT_DIR/stage1.te2" \
        "$OUT_DIR/regions.pi.json" "$PI_USER@$PI_HOST:~/$PI_DIR/build/"
else
    echo "== regions only (image + envelope already on the SD card)"
    rsync -a --progress "$OUT_DIR/regions.pi.json" \
        "$PI_USER@$PI_HOST:~/$PI_DIR/build/"
fi
ssh "$PI_USER@$PI_HOST" "mv ~/$PI_DIR/build/regions.pi.json ~/$PI_DIR/build/regions.json"

echo "== unit"
scp "$TOCTOU_REPO/pizero/toctou-gadget.service" \
    "$PI_USER@$PI_HOST:~/$PI_DIR/toctou-gadget.service"
if ssh "$PI_USER@$PI_HOST" 'sudo -n true' 2>/dev/null; then
    ssh "$PI_USER@$PI_HOST" "sudo install -m0644 ~/$PI_DIR/toctou-gadget.service /etc/systemd/system/toctou-gadget.service && sudo systemctl daemon-reload"
else
    echo "  (sudo needs a password; install later with:)"
    echo "  ssh $PI_USER@$PI_HOST 'sudo install -m0644 ~/$PI_DIR/toctou-gadget.service /etc/systemd/system/toctou-gadget.service && sudo systemctl daemon-reload'"
fi

echo "== checksums on the Pi"
ssh "$PI_USER@$PI_HOST" "cd ~/$PI_DIR/build && sha256sum usb.img stage1.te2 regions.json"

if [ "$START" = 1 ]; then
    echo "== enabling + starting"
    ssh "$PI_USER@$PI_HOST" 'sudo systemctl enable --now toctou-gadget.service; sudo systemctl status --no-pager toctou-gadget.service | head -20'
fi
