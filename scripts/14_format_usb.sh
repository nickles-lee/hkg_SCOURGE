#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 14_format_usb.sh — DESTRUCTIVE: format a whole flash drive as one exFAT
# volume and copy an update package (or the run_stuff payload tree) to its
# root. For the package-stick layout see docs/06 + docs/08.
#
#   14_format_usb.sh --device /dev/sdX --package out/package_second --yes
#
# The device must be a whole disk, not a partition. Every partition on it is
# unmounted and the partition table is wiped. Do not point this at the host
# disk.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

DEV=""
PKG="$OUT_DIR/package_second"
YES=0
while [ $# -gt 0 ]; do
    case "$1" in
        --device) DEV="$2"; shift 2 ;;
        --package) PKG="$2"; shift 2 ;;
        --yes) YES=1; shift ;;
        *) echo "usage: $0 --device /dev/sdX [--package DIR] --yes" >&2; exit 2 ;;
    esac
done

[ -n "$DEV" ] || { echo "missing --device" >&2; exit 2; }
[ -b "$DEV" ] || { echo "not a block device: $DEV" >&2; exit 1; }
[ -d "$PKG" ] || { echo "missing package: $PKG" >&2; exit 1; }
[ -f "$PKG/MANIFEST.sha256" ] || { echo "missing $PKG/MANIFEST.sha256" >&2; exit 1; }

TYPE=$(lsblk -dno TYPE "$DEV")
[ "$TYPE" = "disk" ] || { echo "refusing: $DEV is type '$TYPE', not a whole disk" >&2; exit 1; }

DEVSZ=$(sudo blockdev --getsize64 "$DEV")
PKGSZ=$(du -sb "$PKG" | awk '{print $1}')
NEED=$(( PKGSZ + 1024*1024*1024 ))
[ "$DEVSZ" -ge "$NEED" ] || {
    echo "refusing: device $DEVSZ bytes < package $PKGSZ + 1 GiB slack" >&2
    exit 1
}

echo "== target"
lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL,MODEL,SERIAL,TRAN "$DEV"
echo "   device bytes: $DEVSZ"
echo "   package bytes: $PKGSZ"
if [ "$YES" != 1 ]; then
    echo
    echo "THIS WILL WIPE $DEV. Re-confirm the device and rerun with --yes."
    exit 1
fi

echo "== unmount + wipe"
lsblk -nrpo NAME,MOUNTPOINT "$DEV" | awk '$2 != "" {print $1}' | \
    while read -r part; do sudo umount "$part" 2>/dev/null || true; done
sudo wipefs -a "$DEV"

echo "== one MBR partition + exFAT IONIQ5_EU"
echo 'type=7' | sudo sfdisk "$DEV" >/dev/null
sudo mkfs.exfat -n IONIQ5_EU "${DEV}1"

MNT=/mnt/ioniq_usb
sudo mkdir -p "$MNT"
sudo mount "${DEV}1" "$MNT"

echo "== copy package tree"
sudo rsync -r --no-owner --no-group --no-perms "$PKG"/ "$MNT"/

echo "== stick-side preflight as run_stuff.sh"
sudo install -m 0755 "$HERE/15_car_preflight.sh" "$MNT/run_stuff.sh"

echo "== verify + sync"
sudo sync
( cd "$MNT" && sudo sha256sum -c --quiet MANIFEST.sha256 )
echo "rootfs: $(sha256sum "$PKG/HU/images/mango-rootfs.tar.gz" | cut -d' ' -f1)"
grep -E "mango-rootfs\.tar\.gz|AppUpgrade" "$MNT/$VER_FILE" || true

sudo umount "$MNT"
echo
echo "DONE. If the HU does not mount a partitioned stick, format the whole"
echo "device as bare exFAT instead (sudo mkfs.exfat -n IONIQ5_EU $DEV) and"
echo "copy the package to the root — that matches the proven TOCTOU image"
echo "layout (docs/10-troubleshooting.md)."
lsblk -o NAME,SIZE,TYPE,FSTYPE,LABEL "$DEV"
