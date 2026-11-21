#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 11_deploy_pi.sh — deploy the built artifacts to the Pi and install the
# auto-arm unit. Host/user come from config/paths.local.env (PI_HOST/PI_USER).
#
#   11_deploy_pi.sh [--skip-image] [--start] [--image PATH-ON-PI] [--envelope PATH-ON-PI]
#
# The 27 GB usb.img transfer over the Pi's WiFi is ~44 h at 0.17 MB/s, so the
# image never travels over the network. Either scripts/13_copy_image_to_sd.sh
# has put it on the SD card (then --skip-image), or it lives on a dedicated
# exFAT stick formatted and filled from the build host and is passed with
# --image (docs/04-pi-rig.md). The 879 MB envelope can ride along on that same
# stick (--envelope), so nothing bulky crosses the WiFi; regions and the unit are
# still deployed here.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"
need_fetched

[ -n "$PI_HOST" ] || { echo "set PI_HOST/PI_USER in config/paths.local.env" >&2; exit 1; }

# Rig credentials are optional: with RIG_PASSWORD set in config/rig.local.env the
# sudo steps below run unattended (`sudo -n` cannot prompt over this pipe).
RIG_PASSWORD="${RIG_PASSWORD:-}"
if [ -z "$RIG_PASSWORD" ] && [ -f "$SCOURGE_ROOT/config/rig.local.env" ]; then
    # Source it: the file quotes its values, so a sed one-liner hands sudo a
    # password with literal quotes around it and the auth fails silently.
    # shellcheck disable=SC1091
    . "$SCOURGE_ROOT/config/rig.local.env"
fi

rig_sudo() {
    if [ -n "$RIG_PASSWORD" ]; then
        ssh "$PI_USER@$PI_HOST" "sudo -S -p '' $1" <<<"$RIG_PASSWORD"
    else
        ssh "$PI_USER@$PI_HOST" "sudo $1"
    fi
}

SKIP_IMAGE=0
START=0
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-image) SKIP_IMAGE=1; shift ;;
        --start) START=1; shift ;;
        --image) IMG_PI="${2:?--image needs a path on the Pi}"; shift 2 ;;
        --envelope) ENV_PI="${2:?--envelope needs a path on the Pi}"; shift 2 ;;
        *) echo "usage: $0 [--skip-image] [--start] [--image PATH-ON-PI]" >&2; exit 2 ;;
    esac
done
PI_ROOT="/home/$PI_USER/$PI_DIR"
# Where the gadget reads the 27 GB image. The SD card is often too small for it
# (docs/04), in which case the image sits on a dedicated exFAT stick and this
# path points there instead.
IMG_PI="${IMG_PI:-$PI_ROOT/build/usb.img}"
#: The patched envelope the gadget serves from pass #2 on. Where the image sits,
#: so can this — 839 MB over the WiFi is 1.5 h. An image on a stick defaults to
#: the envelope beside it; a build-dir default there would point the swap at a
#: file that is not on that media at all.
case "$IMG_PI" in
    "$PI_ROOT"/build/*) ENV_PI_DEFAULT="$PI_ROOT/build/stage1.te2" ;;
    *) ENV_PI_DEFAULT="${IMG_PI%/*}/stage1.te2" ;;
esac
ENV_PI="${ENV_PI:-$ENV_PI_DEFAULT}"

[ -f "$OUT_DIR/usb.img" ] || { echo "missing $OUT_DIR/usb.img (run 08_build_usb_image.sh)" >&2; exit 1; }
[ -f "$OUT_DIR/stage1.te2" ] || { echo "missing $OUT_DIR/stage1.te2" >&2; exit 1; }
[ -f "$OUT_DIR/regions.json" ] || { echo "missing $OUT_DIR/regions.json" >&2; exit 1; }

echo "== regions.json for the Pi"
"$PYTHON" - "$OUT_DIR/regions.json" "$OUT_DIR/regions.pi.json" <<PY
import json, sys
src, dst = sys.argv[1:3]
r = json.load(open(src))
for reg in r:
    reg["evil"] = "$ENV_PI"
json.dump(r, open(dst, "w"), indent=2)
print(json.dumps(r, indent=2))
PY

echo "== directories"
ssh "$PI_USER@$PI_HOST" "mkdir -p ~/$PI_DIR/build ~/$PI_DIR/pizero"

echo "== pizero scripts"
rsync -a "$TOCTOU_REPO/pizero/" "$PI_USER@$PI_HOST:~/$PI_DIR/pizero/"

# An envelope outside the rig's build dir lives on the stick: do not upload 839 MB
# to a path the gadget is not going to read.
UPLOAD_ENVELOPE=1
[ "${ENV_PI%/*}" = "$PI_ROOT/build" ] || UPLOAD_ENVELOPE=0
if [ "$SKIP_IMAGE" = 0 ] && [ "$UPLOAD_ENVELOPE" = 1 ]; then
    echo "== artifacts (27 GB usb.img + envelope + regions)"
    rsync -a --progress "$OUT_DIR/usb.img" "$OUT_DIR/stage1.te2" \
        "$OUT_DIR/regions.pi.json" "$PI_USER@$PI_HOST:~/$PI_DIR/build/"
elif [ "$UPLOAD_ENVELOPE" = 1 ]; then
    echo "== envelope + regions (image already on the SD card)"
    rsync -a --progress "$OUT_DIR/stage1.te2" "$OUT_DIR/regions.pi.json" \
        "$PI_USER@$PI_HOST:~/$PI_DIR/build/"
else
    echo "== regions only (image and envelope are already on the rig's media)"
    rsync -a --progress "$OUT_DIR/regions.pi.json" \
        "$PI_USER@$PI_HOST:~/$PI_DIR/build/"
fi
ssh "$PI_USER@$PI_HOST" "mv ~/$PI_DIR/build/regions.pi.json ~/$PI_DIR/build/regions.json"

# The served image is a symlink, not a hardcoded path: that is what the page's
# Serve button moves. The gadget is pointed at the pointer, so a swap plus a
# re-arm is all it takes to change what the head unit sees.
echo "== serve pointer -> $IMG_PI"
ssh "$PI_USER@$PI_HOST" "ln -sfn '$IMG_PI' ~/$PI_DIR/build/serve.img && ls -l ~/$PI_DIR/build/serve.img"

echo "== unit (paths rewritten for this rig)"
mkdir -p "$WORK_DIR"
echo "   image: $IMG_PI"
"$PYTHON" - "$TOCTOU_REPO/pizero/toctou-gadget.service" "$WORK_DIR/toctou-gadget.service" \
    "$PI_ROOT" <<'PY'
import re
import sys

src, dst, root = sys.argv[1:4]
# The upstream unit carries its author's home directory; rewrite whatever prefix
# it has instead of quoting a personal path here (our own leak gate flags those).
text = re.sub(r"/home/[^/\s]+/toctou-usb-update", root, open(src).read())
lines = []
for line in text.splitlines():
    if line.startswith("ConditionPathExists="):
        # A dangling serve.img means "no image armed": let the unit be skipped
        # cleanly instead of failing on every boot.
        lines.append(line)
        line = "ConditionPathExists=%s/build/serve.img" % root
    if line.startswith("ExecStart="):
        # The upstream unit relies on 03_deploy_gadget.sh's own defaults, which
        # point at <pizero>/build/. Pass the real locations explicitly instead,
        # and point at serve.img so the page's Serve button owns the choice.
        line = ("ExecStart=%s/pizero/03_deploy_gadget.sh "
                "%s/build/serve.img %s/build/regions.json" % (root, root, root))
    lines.append(line)
open(dst, "w").write("\n".join(lines) + "\n")
print(dst)
PY
scp "$WORK_DIR/toctou-gadget.service" \
    "$PI_USER@$PI_HOST:~/$PI_DIR/toctou-gadget.service"
if rig_sudo "true" 2>/dev/null; then
    scp "$WORK_DIR/toctou-gadget.service" "$PI_USER@$PI_HOST:/tmp/toctou-gadget.service"
    # One sudo per command on purpose: sudo covers only the first command of a
    # "a && b" string, and the second one then fails on interactive auth.
    rig_sudo "install -m0644 /tmp/toctou-gadget.service /etc/systemd/system/toctou-gadget.service"
    rig_sudo "systemctl daemon-reload"
    # Deliberately not enabled: on a Pi 4/5 powered through USB-C, the gadget
    # binding the UDC can cost the board its power (docs/04, Other boards).
    # Arming is a manual step — systemctl start, or TOCTOU mode on the page.
    echo "  unit installed and left disabled — arm it with 'systemctl start toctou-gadget'"
else
    echo "  (sudo needs a password; install later with:)"
    echo "  ssh $PI_USER@$PI_HOST 'sudo install -m0644 ~/$PI_DIR/toctou-gadget.service /etc/systemd/system/toctou-gadget.service && sudo systemctl daemon-reload'"
fi

echo "== checksums on the Pi"
# The image and the envelope may live on a stick that is not plugged in yet, so a
# missing path is a note, not a failure.
ssh "$PI_USER@$PI_HOST" "for f in '$IMG_PI' '$ENV_PI' '$PI_ROOT/build/regions.json'; do
    if [ -e \"\$f\" ]; then sha256sum \"\$f\"; else echo \"\$f: not present (plug the stick, then re-run)\"; fi
done"

#: The recovery switch: an empty region list makes the gadget serve the image
#: verbatim, which is a genuine stock reinstall. Kept next to the swap config so
#: going back is one cp, not a rebuild.
echo "== recovery config (empty region list)"
ssh "$PI_USER@$PI_HOST" "printf '[]\n' > '$PI_ROOT/build/regions-stock.json'"

if [ "$START" = 1 ]; then
    echo "== enabling + starting"
    rig_sudo "systemctl enable --now toctou-gadget.service"
    ssh "$PI_USER@$PI_HOST" "systemctl --no-pager status toctou-gadget.service | head -20"
fi
