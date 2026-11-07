#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Managed scenarios. Only executed inside the isolated vendor-rootfs guest.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/app/bin
SCENARIO=${1:?scenario required}
case "${HARNESS_PROFILE:-research}" in research|conformance) ;; *) exit 1 ;; esac

find_update() {
    mkdir -p /media/usb
    for attempt in {1..20}; do
        for dev in /dev/sd? /dev/sd?1; do
            [ -b "$dev" ] || continue
            mounted=0
            while read -r source target fstype options rest; do
                [ "$source" = "$dev" ] && mounted=1
            done </proc/mounts
            [ "$mounted" = 1 ] && continue
            mount "$dev" /media/usb 2>/dev/null || continue
            if [ -f /media/usb/IONIQ5_EU.ver ] && [ -f /media/usb/.lge.upgrade.xml ]; then
                export DEV=$dev MEDIA=/media/usb
                while read -r source target fstype options rest; do
                    [ "$target" = /media/usb ] && echo "HARNESS_USB device=$dev fstype=$fstype"
                done </proc/mounts
                return 0
            fi
            umount /media/usb || return 1
        done
        sleep 1
    done
    echo 'HARNESS_FAIL update media not found'
    return 1
}

case "$SCENARIO" in
    usb-discovery)
        modprobe texfat 2>/dev/null || true
        find_update
        echo 'HARNESS_CHECK usb-discovery PASS'
        ;;
    dbus-ready)
        mkdir -p /rw_data/var/run/dbus /var/run/dbus /run/dbus
        cp /etc/passwd.legacy /tmp/passwd
        ln -sf /etc/shadow.Global /tmp/shadow
        dbus-daemon --system --fork
        export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/var/run/dbus/system_bus_socket
        dbus-daemon --session --fork --print-address >/tmp/harness-session.addr
        export DBUS_SESSION_BUS_ADDRESS="$(cat /tmp/harness-session.addr)"
        dbus-send --system --print-reply --dest=org.freedesktop.DBus / org.freedesktop.DBus.ListNames
        echo 'HARNESS_CHECK dbus-system PASS'
        dbus-send --session --print-reply --dest=org.freedesktop.DBus / org.freedesktop.DBus.ListNames
        echo 'HARNESS_CHECK dbus-session PASS'
        ;;
    dmclient-events|updater-launch)
        if [ "${HARNESS_PROFILE:-research}" != research ]; then
            echo 'HARNESS_BLOCKED unmodified application prerequisites unresolved'
            exit 77
        fi
        modprobe texfat 2>/dev/null || true
        find_update
        export HARNESS_TARGET=$SCENARIO
        exec /bin/bash /media/tools/phase4.sh
        ;;
    *) echo "HARNESS_FAIL unknown scenario $SCENARIO"; exit 1 ;;
esac
