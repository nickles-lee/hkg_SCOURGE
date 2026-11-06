#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 13_copy_image_to_sd.sh — copy the 27 GB usb.img to the Pi's SD card directly
# (card reader on this host), because the Pi's WiFi is far too slow.
#
#   1. power off the Pi
#   2. remove the SD card, insert it into this host
#   3. run this script
#   4. reinsert the card, boot the Pi
#
# The Pi rootfs is found by LABEL (PI_ROOTFS_LABEL, default rootfs) or
# PARTUUID (PI_ROOTFS_PARTUUID) from config/paths.local.env.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

IMG="$OUT_DIR/usb.img"
ENV="$OUT_DIR/stage1.te2"

[ -f "$IMG" ] || { echo "missing $IMG" >&2; exit 1; }
[ -f "$ENV" ] || { echo "missing $ENV" >&2; exit 1; }

IMG_SHA="$(sha256sum "$IMG" | cut -d' ' -f1)"
ENV_SHA="$(sha256sum "$ENV" | cut -d' ' -f1)"

DEV=""
[ -n "$PI_ROOTFS_PARTUUID" ] && DEV="$(readlink -f "/dev/disk/by-partuuid/$PI_ROOTFS_PARTUUID" 2>/dev/null || true)"
[ -z "$DEV" ] && DEV="$(readlink -f "/dev/disk/by-label/$PI_ROOTFS_LABEL" 2>/dev/null || true)"
[ -n "$DEV" ] || {
    echo "No Pi rootfs found. Insert the Pi SD card (label '$PI_ROOTFS_LABEL') and retry," >&2
    echo "or set PI_ROOTFS_LABEL / PI_ROOTFS_PARTUUID in config/paths.local.env." >&2
    lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,MOUNTPOINT
    exit 1
}

echo "device: $DEV"
MNT="$(findmnt -no TARGET "$DEV" 2>/dev/null || true)"
if [ -z "$MNT" ]; then
    MNT="$(mktemp -d)"
    sudo mount "$DEV" "$MNT"
    trap 'sudo umount "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true' EXIT
fi
echo "mount:  $MNT"

DST="$MNT/home/$PI_USER/$PI_DIR/build"
sudo mkdir -p "$DST"
for pair in "$IMG:usb.img:$IMG_SHA" "$ENV:stage1.te2:$ENV_SHA"; do
    src="${pair%%:*}"; rest="${pair#*:}"; name="${rest%%:*}"; want="${rest#*:}"
    echo "copying $(stat -c%s "$src") bytes -> $DST/$name"
    sudo cp --reflink=never "$src" "$DST/$name"
done
sudo sync
for pair in "$IMG:usb.img:$IMG_SHA" "$ENV:stage1.te2:$ENV_SHA"; do
    src="${pair%%:*}"; rest="${pair#*:}"; name="${rest%%:*}"; want="${rest#*:}"
    got="$(sudo sha256sum "$DST/$name" | cut -d' ' -f1)"
    echo "$name sha256: $got"
    [ "$got" = "$want" ] || { echo "MISMATCH for $name (want $want)" >&2; exit 1; }
done
echo "OK — reinsert the card in the Pi, boot, then run 11_deploy_pi.sh --skip-image"
