#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# WIFI_PAYLOAD_RESTORE=1
# Removes the WiFi app and restores the QML that was present before install.
# Rename/copy this file to run_stuff.sh on the USB root, then plug it in.
set -u
PATH="${WIFI_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
QML_ROOT="${WIFI_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/wifi"
RW_DIR="${WIFI_RW_DIR:-/rw_data/wifi}"
LOG="$RW_DIR/restore.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${WIFI_SERVICE:-/etc/systemd/system/wifi-daemon.service}"

ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
WIDE="HomeScreen/QHMI_HomeScreenMain_Wide.qml"
ALL="HomeScreen/Screen_HomeScreen_AllMenus.qml"
SETUP="HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
CUSTOM="component/QCustomizedMenuListAllowEmpty.qml"
THEME="component/common/Q_ThemeImage.qml"

mkdir -p "$RW_DIR" 2>/dev/null
: > "$LOG" 2>/dev/null
log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }
result() {
    { echo "WIFI_RESTORE"; echo "time=$(date -u 2>/dev/null)"; echo "usb=$USB_DIR"; echo "$@"; } > "$OUT" 2>&1
    cp -f "$OUT" "$RW_DIR/restore-result.txt" 2>/dev/null
}
fail() { log "FAIL: $*"; result "status=FAIL" "error=$*"; exit 1; }

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
[ -f "$USB_DIR/manifest.md5" ] || fail "manifest.md5 missing"
if ! ( cd "$USB_DIR" && md5sum -c manifest.md5 ) > "$RW_DIR/restore-manifest.log" 2>&1; then
    fail "manifest check failed"
fi

LATEST="$RW_DIR/backup/latest"
[ -d "$LATEST" ] || fail "no backup/latest; cannot restore"

was_ro=0
grep -q ' / .* ro[, ]' /proc/mounts && was_ro=1
if [ "$was_ro" = 1 ]; then
    mount -o remount,rw / 2>/dev/null || fail "cannot remount / rw"
fi

# Stop daemon/SSH and close the firewall.
if [ -x "$RW_DIR/wifi_daemon" ]; then
    "$RW_DIR/wifi_daemon" --mode off >> "$LOG" 2>&1 || true
fi
if command -v systemctl >/dev/null 2>&1; then
    systemctl stop wifi-daemon.service 2>/dev/null || true
    systemctl disable wifi-daemon.service 2>/dev/null || true
fi
for p in /proc/[0-9]*; do
    [ -r "$p/comm" ] || continue
    c=$(cat "$p/comm" 2>/dev/null)
    case "$c" in
        wifi_daemon) kill -TERM "${p#/proc/}" 2>/dev/null || true ;;
        dropbear)
            cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null || true)
            case "$cmd" in
                *"$RW_DIR/bin/dropbear"*) kill -TERM "${p#/proc/}" 2>/dev/null || true ;;
            esac
            ;;
    esac
done
PORT=$(grep '^ssh_port=' "$RW_DIR/config.conf" 2>/dev/null | cut -d= -f2)
[ -n "$PORT" ] || PORT=2222
IPT=""
[ -x "$RW_DIR/bin/iptables" ] && IPT="$RW_DIR/bin/iptables"
[ -z "$IPT" ] && [ -x /usr/sbin/iptables ] && IPT=/usr/sbin/iptables
[ -z "$IPT" ] && [ -x /sbin/iptables ] && IPT=/sbin/iptables
if [ -n "$IPT" ]; then
    for p in "$PORT" 22 2222; do
        case "$IPT" in
            *xtables-multi) "$IPT" iptables -D INPUT -i wlan0 -p tcp --dport "$p" -j ACCEPT 2>/dev/null || true ;;
            *) "$IPT" -D INPUT -i wlan0 -p tcp --dport "$p" -j ACCEPT 2>/dev/null || true ;;
        esac
    done
fi

# Restore QML from the pre-install backup.
for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM" "$THEME"; do
    if [ -f "$LATEST/$f" ]; then
        label=$(getfattr -n security.SMACK64 --only-values "$QML_DIR/$f" 2>/dev/null || true)
        mkdir -p "$QML_DIR/$(dirname "$f")" 2>/dev/null
        cp -f "$LATEST/$f" "$QML_DIR/$f" || fail "restore failed: $f"
        chmod 644 "$QML_DIR/$f" 2>/dev/null || true
        chown 0:0 "$QML_DIR/$f" 2>/dev/null || true
        [ -n "$label" ] && setfattr -n security.SMACK64 -v "$label" "$QML_DIR/$f" 2>/dev/null || true
        log "restored $f"
    fi
done

# Remove new files/assets.
for f in HomeScreen/WifiPage.qml component/WifiTile.qml component/WifiSwitch.qml component/WifiButton.qml; do
    rm -f "$QML_DIR/$f" 2>/dev/null || true
done
rm -rf "$ASSET_DIR" 2>/dev/null || true
rm -f "$SERVICE" 2>/dev/null || true

# Remove the SSH user from the live passwd/shadow and the persistent
# templates that esinit uses on every boot.
remove_car() {
    f="$1"
    [ -f "$f" ] || return 0
    tmp="/tmp/wifi-car-remove.$$"
    grep -v '^car:' "$f" > "$tmp" 2>/dev/null || true
    cat "$tmp" > "$f" 2>/dev/null || true
    rm -f "$tmp"
    case "$f" in
        *shadow*) chmod 600 "$f" 2>/dev/null || true ;;
        *) chmod 644 "$f" 2>/dev/null || true ;;
    esac
}
remove_car /etc/passwd
remove_car /etc/passwd.legacy
remove_car /etc/passwd.secure
remove_car /etc/shadow
remove_car /etc/shadow.Global
if [ -L /home/car ]; then
    rm -f /home/car 2>/dev/null || true
fi

# Keep backups; remove runtime pieces and secrets.
rm -f "$RW_DIR/wifi_daemon" "$RW_DIR/status.json" "$RW_DIR/daemon.log" \
      "$RW_DIR/config.conf" "$RW_DIR/wifi.txt" 2>/dev/null || true
rm -rf "$RW_DIR/bin" "$RW_DIR/ssh" "$RW_DIR/home" 2>/dev/null || true

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload 2>/dev/null || true
fi
sync

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
    if dbus-send --session --type=method_call --dest=com.lge.avn.applauncher /Request \
        com.lge.avn.applauncher.request.launch int32:11 \
        string:AppHomeScreen string:/app/bin/AppHomeScreen >> "$LOG" 2>&1; then
        restart="dbus-ok"
    else
        restart="dbus-launch-failed"
    fi
fi

if [ "$was_ro" = 1 ]; then
    mount -o remount,ro / 2>/dev/null || true
fi

result "status=OK" "restored_from=$LATEST" "restart=$restart" "log=$LOG"
log "=== wifi restore done ==="
exit 0
