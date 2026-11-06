#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Read-only diagnostic collector for the Battery app.
set -u
PATH=/bin:/sbin:/usr/bin:/usr/sbin
USB_DIR="${USB_DIR:-/media/usb}"
OUT="$USB_DIR/battery_diag.txt"
RW_DIR="/rw_data/battery"

{
    echo "BATTERY_DIAG"
    echo "time=$(date -u 2>/dev/null)"
    echo "usb=$USB_DIR"
    echo "uid=$(id -u 2>/dev/null)"
    echo
    echo "== can interfaces =="
    ip -details link show 2>/dev/null | sed -n '/can/,+3p'
    echo
    echo "== can0 sniff (3 s) =="
    "$RW_DIR/battery_daemon" --iface can0 --sniff 3 2>&1 | head -40
    echo
    echo "== daemon process =="
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        if [ "$(cat "$p/comm" 2>/dev/null)" = "battery_daemon" ]; then
            echo "pid=${p#/proc/}"
            tr '\0' ' ' < "$p/cmdline" 2>/dev/null
            echo
        fi
    done
    echo
    echo "== service =="
    systemctl status battery-daemon.service --no-pager 2>&1 | head -20
    echo
    echo "== daemon version / freshness =="
    grep -o '"ver":"[^"]*"' "$RW_DIR/status.json" 2>/dev/null
    if [ -x "$RW_DIR/battery_daemon" ]; then
        echo "installed md5: $(md5sum "$RW_DIR/battery_daemon" 2>/dev/null | cut -d' ' -f1)"
    fi
    if [ -f "$USB_DIR/payload/bin/battery_daemon" ]; then
        echo "payload  md5: $(md5sum "$USB_DIR/payload/bin/battery_daemon" 2>/dev/null | cut -d' ' -f1)"
    fi
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        if [ "$(cat "$p/comm" 2>/dev/null)" = "battery_daemon" ]; then
            echo "running  md5: $(md5sum "$p/exe" 2>/dev/null | cut -d' ' -f1) pid=${p#/proc/}"
        fi
    done
    echo
    echo "== D-Bus system bus reachability =="
    dbus-send --system --print-reply --dest=org.freedesktop.DBus \
        /org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>&1 | head -40
    echo
    echo "== status mirror =="
    cat "$RW_DIR/status.json" 2>/dev/null
    echo
    echo "== daemon log tail =="
    tail -30 "$RW_DIR/daemon.log" 2>/dev/null
    echo
    echo "== install log tail =="
    tail -30 "$RW_DIR/install.log" 2>/dev/null
    echo
    echo "== charge door dependencies =="
    ls -l /usr/bin/dbus-send 2>/dev/null
    getfattr -n security.SMACK64EXEC --only-values "$RW_DIR/battery_daemon" 2>/dev/null
    echo
    echo "== QML hashes =="
    md5sum /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml \
           /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/BatteryPage.qml \
           /app/share/AppHomeScreen/qml_eco3.0/component/BatterySwitch.qml \
           /app/share/AppHomeScreen/qml_eco3.0/component/BatteryTile.qml 2>/dev/null
    echo
    echo "== markers =="
    grep -l 'HELLO_PAYLOAD_260128' /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml 2>/dev/null
    grep -l 'BATTERY_PAYLOAD_260128' /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml 2>/dev/null
    echo
    echo "== install result =="
    cat "$RW_DIR/result.txt" 2>/dev/null
} > "$OUT" 2>&1

echo "wrote $OUT"
exit 0
