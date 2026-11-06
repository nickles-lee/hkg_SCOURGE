#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 15_car_preflight.sh — read-only preflight run by the HU failsafe loader
# from the update drive. The loader sets USB_DIR to the directory containing
# this script. Writes the report to /rw_data and copies it back to the drive
# when writable. It does not modify the system.
#
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128.
USB_DIR="${USB_DIR:-$(dirname "$0")}"
TMP=/rw_data/scourge_preflight.txt
mkdir -p /rw_data 2>/dev/null || true

{
    echo "=== SCOURGE preflight ==="
    date -u 2>/dev/null
    uname -a 2>/dev/null
    echo "--- cmdline ---"
    cat /proc/cmdline 2>/dev/null
    echo "--- mounts ---"
    mount 2>/dev/null | head -40
    echo "--- df ---"
    df -h 2>/dev/null | head -30
    echo "--- updater (engine: four stubs expected) ---"
    F=/app/bin/AppUpgrade
    if [ -f "$F" ]; then
        stat -c '%s %n' "$F" 2>/dev/null
        md5sum "$F" 2>/dev/null
        for pair in 2086500:FUN_002f5a00 2086852:FUN_002f5b60 \
                    2086916:FUN_002f5ba0 2087076:FUN_002f5c40; do
            off=${pair%%:*}; name=${pair##*:}
            printf '%s bytes at %s: ' "$name" "$off"
            dd if="$F" bs=1 skip="$off" count=3 2>/dev/null | od -An -tx1 | tr -d ' \n'
            echo
        done
    else
        echo "missing $F"
    fi
    echo "--- button gate (AppScomoDM: two stubs expected) ---"
    S=/app/bin/AppScomoDM
    if [ -f "$S" ]; then
        stat -c '%s %n' "$S" 2>/dev/null
        md5sum "$S" 2>/dev/null
        for pair in 930864:FUN_001e3430 930928:FUN_001e3470; do
            off=${pair%%:*}; name=${pair##*:}
            printf '%s bytes at %s: ' "$name" "$off"
            dd if="$S" bs=1 skip="$off" count=3 2>/dev/null | od -An -tx1 | tr -d ' \n'
            echo
        done
    else
        echo "missing $S"
    fi
    echo "--- failsafe ---"
    ls -la /etc/initial_root/ 2>/dev/null
    systemctl status initial-root-failsafe --no-pager 2>/dev/null | head -20
    echo "--- initial_root log (tail) ---"
    tail -50 /rw_data/initial_root.log 2>/dev/null
    echo "=== end ==="
} > "$TMP" 2>&1

cp "$TMP" "$USB_DIR/scourge_preflight.txt" 2>/dev/null || true
cp /rw_data/initial_root.log "$USB_DIR/initial_root.log" 2>/dev/null || true
exit 0
