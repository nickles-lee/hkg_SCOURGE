#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# BATTERY_PAYLOAD_RESTORE_REV=1
# Restore the QML and remove the Battery app.
#
# The loader MD5-gates run_stuff.sh, so copy this file over run_stuff.sh on the
# stick (or rename it) to run it.
set -u
PATH="${BATTERY_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
QML_ROOT="${BATTERY_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/battery"
RW_DIR="${BATTERY_RW_DIR:-/rw_data/battery}"
LOG="$RW_DIR/restore.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${BATTERY_SERVICE:-/etc/systemd/system/battery-daemon.service}"
ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"

mkdir -p "$RW_DIR" 2>/dev/null
: > "$LOG" 2>/dev/null

log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }

result() {
    {
        echo "BATTERY_PAYLOAD_RESTORE"
        echo "time=$(date -u 2>/dev/null)"
        echo "usb=$USB_DIR"
        echo "uid=$(id -u 2>/dev/null)"
        echo "$@"
    } > "$OUT" 2>&1
    cp -f "$OUT" "$RW_DIR/result.txt" 2>/dev/null
}

fail() {
    log "FAIL: $*"
    result "status=FAIL" "error=$*"
    exit 1
}

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
log "=== battery restore start ==="

# Remount rw first: disabling the unit removes a symlink under /etc.
was_ro=0
grep -q ' / .* ro[, ]' /proc/mounts && was_ro=1
if [ "$was_ro" = 1 ]; then
    mount -o remount,rw / 2>/dev/null || fail "cannot remount / rw"
fi

# Stop and disable the daemon.
if command -v systemctl >/dev/null 2>&1; then
    systemctl disable --now battery-daemon.service >> "$LOG" 2>&1 || true
fi
for p in /proc/[0-9]*; do
    [ -r "$p/comm" ] || continue
    if [ "$(cat "$p/comm" 2>/dev/null)" = "battery_daemon" ]; then
        kill -TERM "${p#/proc/}" 2>/dev/null || true
    fi
done
rm -f "$SERVICE" 2>/dev/null || true
command -v systemctl >/dev/null 2>&1 && systemctl daemon-reload >> "$LOG" 2>&1 || true

# Restore the Advanced QML from the pre-install backup, else the 260128 ref.
restored=""
if [ -f "$RW_DIR/backup/latest/$ADV" ]; then
    cp -f "$RW_DIR/backup/latest/$ADV" "$QML_DIR/$ADV" || fail "restore failed"
    restored="backup-latest"
elif [ -f "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/$ADV" ]; then
    cp -f "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/$ADV" "$QML_DIR/$ADV" || fail "restore failed"
    restored="usb-backup-current"
elif [ -f "$USB_DIR/backup/AppHomeScreen/qml_eco3.0/$ADV" ]; then
    cp -f "$USB_DIR/backup/AppHomeScreen/qml_eco3.0/$ADV" "$QML_DIR/$ADV" || fail "restore failed"
    restored="usb-original"
else
    fail "no QML backup found"
fi
chmod 644 "$QML_DIR/$ADV" 2>/dev/null || true
chown root:root "$QML_DIR/$ADV" 2>/dev/null || true

rm -f "$QML_DIR/HomeScreen/BatteryPage.qml" 2>/dev/null || true
rm -f "$QML_DIR/component/BatterySwitch.qml" 2>/dev/null || true
rm -f "$QML_DIR/component/BatteryTile.qml" 2>/dev/null || true
rm -rf "$ASSET_DIR" 2>/dev/null || true
rm -f "$RW_DIR/battery_daemon" 2>/dev/null || true
rm -rf "$RW_DIR/bin" 2>/dev/null || true
rm -f "$RW_DIR/smack_expected" 2>/dev/null || true

rm -rf /root/.cache/qtshadercache 2>/dev/null || true
rm -rf /root/.cache/AppHomeScreen 2>/dev/null || true
rm -rf /tmp/qmlcache 2>/dev/null || true
rm -rf /tmp/qt-qml-cache 2>/dev/null || true

sync
if [ "$was_ro" = 1 ]; then
    mount -o remount,ro / 2>/dev/null || true
fi

# Restart AppHomeScreen.
restart="not-attempted"
launcher_pid=""
for p in /proc/[0-9]*; do
    [ -r "$p/comm" ] || continue
    if [ "$(cat "$p/comm" 2>/dev/null)" = "AppLauncher" ]; then
        launcher_pid="${p#/proc/}"
        break
    fi
done
if [ -n "$launcher_pid" ] && [ -r "/proc/$launcher_pid/environ" ]; then
    for var in DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR WAYLAND_DISPLAY QT_QPA_PLATFORM; do
        val=$(tr '\0' '\n' < "/proc/$launcher_pid/environ" 2>/dev/null | grep "^$var=" | head -n 1 | cut -d= -f2-)
        [ -n "$val" ] && export "$var=$val"
    done
fi
if command -v dbus-send >/dev/null 2>&1; then
    dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
        com.lge.avn.applauncher.request.terminate int32:11 >> "$LOG" 2>&1 || true
    sleep 2
    restart="dbus-launch-failed"
    for i in 1 2 3; do
        if dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
            com.lge.avn.applauncher.request.launch int32:11 \
            string:AppHomeScreen string:/app/bin/AppHomeScreen >> "$LOG" 2>&1; then
            restart="dbus-ok"
            break
        fi
        sleep 2
    done
fi

log "restored=$restored restart=$restart"
result "status=OK" "restored=$restored" "restart=$restart" "log=$LOG"
exit 0
