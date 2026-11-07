#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Repeat-update recovery at the media/verification level: verify the update
# stick, corrupt one manifest-covered file, detect the failure, repair it from
# a backup, and verify again. The AppUpgrade engine's own repeat behaviour is
# out of scope (needs provisioned credentials).
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
MEDIA_DEV=
for dev in /dev/sd?; do
    [ -b "$dev" ] || continue
    mount -o rw "$dev" /media/usb 2>/dev/null || continue
    if [ -f /media/usb/IONIQ5_EU.ver ] && [ -f /media/usb/MANIFEST.sha256 ]; then
        MEDIA=/media/usb
        MEDIA_DEV=$dev
        break
    fi
    umount /media/usb 2>/dev/null || true
done
[ -n "$MEDIA" ] || { echo 'HARNESS_BLOCKED repeat-update stick missing'; exit 77; }

cd "$MEDIA"
if ! /bin/busybox sha256sum -c MANIFEST.sha256 >/tmp/repeat-1.log 2>&1; then
    echo 'HARNESS_FAIL repeat-baseline'
    tail -20 /tmp/repeat-1.log
    exit 1
fi
echo 'HARNESS_CHECK repeat-baseline PASS'

cp IONIQ5_EU.ver /tmp/repeat-ver.bak || fail 'repeat-backup'
printf '\nCORRUPT\n' >> IONIQ5_EU.ver
sync
if /bin/busybox sha256sum -c MANIFEST.sha256 >/tmp/repeat-2.log 2>&1; then
    echo 'HARNESS_FAIL repeat-corrupt-detected'
    exit 1
fi
grep -q 'IONIQ5_EU.ver: FAILED' /tmp/repeat-2.log || fail 'repeat-corrupt-marker'
echo 'HARNESS_CHECK repeat-corrupt-detected PASS'

cp /tmp/repeat-ver.bak IONIQ5_EU.ver || fail 'repeat-restore'
sync
if ! /bin/busybox sha256sum -c MANIFEST.sha256 >/tmp/repeat-3.log 2>&1; then
    echo 'HARNESS_FAIL repeat-after-repair'
    tail -20 /tmp/repeat-3.log
    exit 1
fi
echo 'HARNESS_CHECK repeat-after-repair PASS'

# A repeat attempt must leave the media usable: unmount and rediscover it.
cd /
umount "$MEDIA" || fail 'repeat-unmount'
mkdir -p /media/usb2
if [ -n "$MEDIA_DEV" ] && ! mount -o ro "$MEDIA_DEV" /media/usb2 2>/dev/null; then
    # Device node name is not guaranteed; rediscover by content.
    for dev in /dev/sd?; do
        [ -b "$dev" ] || continue
        mount -o ro "$dev" /media/usb2 2>/dev/null || continue
        if [ -f /media/usb2/IONIQ5_EU.ver ] && [ -f /media/usb2/MANIFEST.sha256 ]; then
            break
        fi
        umount /media/usb2 2>/dev/null || true
    done
fi
if [ ! -f /media/usb2/IONIQ5_EU.ver ] || [ ! -f /media/usb2/MANIFEST.sha256 ]; then
    echo 'HARNESS_FAIL repeat-rediscover'
    exit 1
fi
umount /media/usb2 2>/dev/null || true
echo 'HARNESS_CHECK repeat-rediscover PASS'
echo 'HARNESS_CHECK recovery-repeat-update PASS'
