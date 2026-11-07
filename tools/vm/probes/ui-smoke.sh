#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Mode C UI smoke: Weston (DRM/pixman) + a visible Wayland probe + a real Qt
# Quick file evaluated by qmlscene. Runs only inside the vendor rootfs guest.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/app/bin
export XDG_RUNTIME_DIR=/var/run/user/root
export LD_LIBRARY_PATH=/app/lib:/usr/lib

mkdir -p /rw_data/var/run/user/root /rw_data/var/log /rw_data/var/run/dbus
chmod 700 /rw_data/var/run/user/root
cp /etc/passwd.legacy /tmp/passwd 2>/dev/null || true
ln -sf /etc/shadow.Global /tmp/shadow 2>/dev/null || true

printf '[core]\nshell=fullscreen-shell.so\n' >/tmp/weston.ini
/usr/bin/weston --backend=drm-backend.so --use-pixman --tty=1 \
    --config=/tmp/weston.ini --socket=wayland-0 --idle-time=0 --debug \
    </dev/tty1 >/tmp/weston.log 2>&1 &
sleep 6
if ! pidof weston >/dev/null 2>&1; then
    echo 'HARNESS_FAIL weston did not start'
    tail -40 /tmp/weston.log 2>&1
    exit 1
fi
echo 'HARNESS_CHECK weston-alive PASS'

export WAYLAND_DISPLAY=wayland-0
setsid /usr/bin/weston-simple-shm </dev/null >/tmp/ui-probe.log 2>&1 &
sleep 4
if ! pidof weston-simple-shm >/dev/null 2>&1; then
    echo 'HARNESS_FAIL visible Wayland probe exited'
    tail -40 /tmp/ui-probe.log 2>&1
    exit 1
fi
echo 'HARNESS_CHECK wayland-probe PASS'

cat >/tmp/ui-smoke.qml <<'EOF'
import QtQuick 2.4
Rectangle {
    width: 640
    height: 360
    color: "#204080"
    Component.onCompleted: console.log("UI_SMOKE_QML_READY")
}
EOF
( QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
    /usr/bin/qt5/qmlscene --quit /tmp/ui-smoke.qml >/tmp/ui-smoke.qml.log 2>&1 || true ) 2>/dev/null
echo 'QML_SCENE_RC=handled'
if grep -q 'UI_SMOKE_QML_READY' /tmp/ui-smoke.qml.log; then
    echo 'HARNESS_CHECK qml-ready PASS'
else
    echo 'HARNESS_FAIL qml-ready'
    tail -60 /tmp/ui-smoke.qml.log 2>&1
    exit 1
fi

echo 'HARNESS_CHECK ui-smoke PASS'
