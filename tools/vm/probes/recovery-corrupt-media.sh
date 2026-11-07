#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Corrupt an update stick on its qcow2 overlay and assert the media integrity
# gate fails cleanly. This is a manifest-level media test, not the AppUpgrade
# signature/credential path.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

fail() {
    echo "HARNESS_FAIL $1"
    exit 1
}

modprobe ehci-pci 2>/dev/null || true
modprobe usb-storage 2>/dev/null || true
modprobe sd_mod 2>/dev/null || true
sleep 2
mkdir -p /media/usb
MEDIA=
for dev in /dev/sd?; do
    [ -b "$dev" ] || continue
    mount -o rw "$dev" /media/usb 2>/dev/null || continue
    if [ -f /media/usb/IONIQ5_EU.ver ] && [ -f /media/usb/MANIFEST.sha256 ]; then
        MEDIA=/media/usb
        break
    fi
    umount /media/usb 2>/dev/null || true
done
[ -n "$MEDIA" ] || { echo 'HARNESS_BLOCKED corrupt-media update stick missing'; exit 77; }
echo 'HARNESS_CHECK media-discovery PASS'

cd "$MEDIA"
if ! /bin/busybox sha256sum -c MANIFEST.sha256 >/tmp/media-baseline.log 2>&1; then
    echo 'HARNESS_FAIL manifest-baseline'
    tail -20 /tmp/media-baseline.log
    exit 1
fi
echo 'HARNESS_CHECK manifest-baseline PASS'

# Deliberate overlay-only corruption: the backing image is read-only.
printf '\nCORRUPT\n' >> IONIQ5_EU.ver
sync
if /bin/busybox sha256sum -c MANIFEST.sha256 >/tmp/media-corrupt.log 2>&1; then
    echo 'HARNESS_FAIL corrupt-undetected'
    exit 1
fi
if ! grep -q 'IONIQ5_EU.ver: FAILED' /tmp/media-corrupt.log; then
    echo 'HARNESS_FAIL corrupt-marker-missing'
    tail -20 /tmp/media-corrupt.log
    exit 1
fi
if grep -qiE 'segmentation fault|kernel panic|BUG:' /tmp/media-corrupt.log; then
    echo 'HARNESS_FAIL corrupt-crash'
    tail -20 /tmp/media-corrupt.log
    exit 1
fi
echo 'HARNESS_CHECK corrupt-detected PASS'
echo '=== CORRUPT MANIFEST OUTPUT ==='
tail -20 /tmp/media-corrupt.log
echo 'HARNESS_CHECK recovery-corrupt-media PASS'
