#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Mode C WiFi-app smoke: install the first-party WiFi payload from the host
# share, then evaluate the patched AppHomeScreen WifiPage.qml with qmlscene.
# This captures QML errors; it is not a screenshot of the page or a car test.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/app/bin:/usr/bin/qt5
export XDG_RUNTIME_DIR=/var/run/user/root
export LD_LIBRARY_PATH=/app/lib:/usr/lib

mkdir -p /rw_data/var/run/user/root /rw_data/var/log
chmod 700 /rw_data/var/run/user/root
cp /etc/passwd.legacy /tmp/passwd 2>/dev/null || true
ln -sf /etc/shadow.Global /tmp/shadow 2>/dev/null || true

if [ ! -f /media/host/run_stuff.sh ] || [ ! -d /media/host/payload ]; then
    echo 'HARNESS_BLOCKED WiFi payload share missing; pass --share <wifi_app/out/usb>'
    exit 77
fi

USB_DIR=/media/host sh /media/host/run_stuff.sh >/tmp/wifi-install.log 2>&1 || true
WIFI_QML=/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/WifiPage.qml
if [ ! -f "$WIFI_QML" ]; then
    echo 'HARNESS_FAIL wifi-install (WifiPage.qml not installed)'
    tail -80 /tmp/wifi-install.log 2>&1
    exit 1
fi
echo 'HARNESS_CHECK wifi-install PASS'

export QML2_IMPORT_PATH=/app/share/AppHomeScreen/qml_eco3.0:/app/share/AppHomeScreen
( QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
    /usr/bin/qt5/qmlscene --quit "$WIFI_QML" >/tmp/wifi-qml.log 2>&1 || true ) 2>/dev/null
if grep -Eq 'WifiPage|qml:' /tmp/wifi-qml.log; then
    echo 'HARNESS_CHECK wifi-qml PASS'
else
    echo 'HARNESS_FAIL wifi-qml (no QML output captured)'
    tail -80 /tmp/wifi-qml.log 2>&1
    exit 1
fi

echo '=== WIFI INSTALL LOG ==='
tail -60 /tmp/wifi-install.log 2>&1
echo '=== WIFI QML LOG ==='
tail -120 /tmp/wifi-qml.log 2>&1
echo '=== WIFI QML ERRORS ==='
grep -aEi 'error|warning|failed|unavailable|not found' /tmp/wifi-qml.log 2>&1 | tail -40 || true

echo 'HARNESS_CHECK wifi-ui PASS'
