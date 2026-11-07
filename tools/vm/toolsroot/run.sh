#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Probe the two USB sticks, put the update tree on /media/usb and the tools on
# /media/tools, then run phase4.sh.
set +e
modprobe ehci-pci; modprobe usb-storage; modprobe sd_mod
for d in 1 2 3 4 5 6 7 8 9 10 11 12; do [ -b /dev/sda ] && break; sleep 1; done
mkdir -p /media/usb /media/tools
mount /dev/sda /media/usb 2>/dev/null
mount /dev/sdb /media/tools 2>/dev/null
if [ ! -f /media/usb/IONIQ5_EU.ver ] && [ -f /media/tools/IONIQ5_EU.ver ]; then
    umount /media/usb
    umount /media/tools
    mount /dev/sda /media/tools
    mount /dev/sdb /media/usb
fi
# Optional third stick: find the payload image by its marker.
mkdir -p /media/payload
for d in /dev/sdc /dev/sdd /dev/sdb /dev/sda; do
    grep -q " $d " /proc/mounts && continue
    mount "$d" /media/payload 2>/dev/null || continue
    if [ -f /media/payload/PAYLOAD ]; then
        echo "PAYLOAD_MOUNTED $d"
        break
    fi
    umount /media/payload
done
# Optional third stick: per-run payload/scripts.
mkdir -p /media/payload
mount /dev/sdc /media/payload 2>/dev/null && echo "PAYLOAD_MOUNTED /dev/sdc"
echo "RUN_MOUNTS:"
grep -E '/media/usb|/media/tools' /proc/mounts
if [ -x /media/tools/phase4.sh ]; then
    exec /media/tools/phase4.sh
fi
echo "RUN_ERROR: phase4.sh missing" >&2
exit 1
