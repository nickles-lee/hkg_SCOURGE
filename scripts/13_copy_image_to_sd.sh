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

# Free-space gate: a 27 GB image into a 29 GB card fails hours in, halfway, and
# leaves a truncated usb.img that looks plausible. Refuse before trying.
NEED_KB=$(( ( $(stat -c%s "$IMG") + $(stat -c%s "$ENV") ) / 1024 ))
HAVE_KB="$(sudo df -k --output=avail "$DST" | tail -n1 | tr -d ' ')"
if [ "$HAVE_KB" -lt "$NEED_KB" ]; then
    echo "not enough room on the card:" >&2
    echo "  need $(echo "$NEED_KB" | awk '{printf "%.1f GiB", $1/1048576}')" \
         "($(echo "$NEED_KB" | awk '{printf "%.1f GB", $1/1e6}')), " \
         "$(echo "$HAVE_KB" | awk '{printf "%.1f GiB", $1/1048576}') free" >&2
    echo "  a 64 GB (or larger) card is what this image wants — docs/04." >&2
    echo "  Alternatively serve it from a USB stick: the rig page lists images" >&2
    echo "  under /media and can point serve.img at them." >&2
    exit 1
fi
# Speed gate: a worn card (or one behind a cheap USB 2 bridge that caps commands
# at 120 KiB) copies at a few hundred kB/s. 27 GB then takes a day. Measure 8 MiB
# onto the card and say the number out loud.
SPD_DIR="$(sudo mktemp -d "$DST/.speed.XXXXXX")"
START=$(date +%s%N)
sudo dd if=/dev/zero of="$SPD_DIR/probe" bs=64k count=128 conv=fsync status=none
ELAPSED_MS=$(( ($(date +%s%N) - START) / 1000000 ))
sudo rm -rf "$SPD_DIR"
[ "$ELAPSED_MS" -gt 0 ] || ELAPSED_MS=1
SPEED_MBS=$(awk -v b=8388608 -v ms="$ELAPSED_MS" 'BEGIN { printf "%.1f", (b/1048576) / (ms/1000) }')
HOURS=$(awk -v b="$NEED_KB" -v s="$SPEED_MBS" 'BEGIN { if (s > 0) printf "%.1f", (b/1024) / s / 3600; else print "inf" }')
echo "card write speed: $SPEED_MBS MB/s — this copy will take about ${HOURS} h"
if awk -v s="$SPEED_MBS" 'BEGIN { exit !(s < 2) }'; then
    echo "WARNING: under 2 MB/s. A healthy card in a decent reader is 10-90 MB/s." >&2
    echo "         A 'Max sectors 120 KiB, queue depth 2' bridge or a worn card reads" >&2
    echo "         and writes at this speed; try another reader or another card before" >&2
    echo "         believing the ${HOURS} h estimate. Continuing anyway." >&2
fi

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
