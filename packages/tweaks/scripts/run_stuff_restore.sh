#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TWEAKS_PAYLOAD_RESTORE=1
# Removes the Tweaks app and restores the QML/wrandr present before install.
# Rename/copy this file to run_stuff.sh on the USB root, then plug it in.
set -u
PATH="${TWEAKS_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
QML_ROOT="${TWEAKS_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/tweaks"
STANDBY_ROOT="${TWEAKS_STANDBY_ROOT:-/app/share/AppStandbyClock}"
OFFLINE_STATUS="${TWEAKS_OFFLINE_STATUS:-/update/offline_status}"
RW_DIR="${TWEAKS_RW_DIR:-/rw_data/tweaks}"
LOG="$RW_DIR/restore.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${TWEAKS_SERVICE:-/etc/systemd/system/tweaks-daemon.service}"

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
    { echo "TWEAKS_RESTORE"; echo "time=$(date -u 2>/dev/null)"; echo "usb=$USB_DIR"; echo "$@"; } > "$OUT" 2>&1
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

# Stop daemon.
if command -v systemctl >/dev/null 2>&1; then
    systemctl stop tweaks-daemon.service 2>/dev/null || true
    systemctl disable tweaks-daemon.service 2>/dev/null || true
fi
for p in /proc/[0-9]*; do
    [ -r "$p/comm" ] || continue
    [ "$(cat "$p/comm" 2>/dev/null)" = "tweaks_daemon" ] && kill -TERM "${p#/proc/}" 2>/dev/null || true
done

# Restore the vendor wrandr.
WRANDR="${TWEAKS_WRANDR:-/usr/bin/wrandr}"
WRANDR_REAL="${TWEAKS_WRANDR_REAL:-/usr/bin/wrandr.real}"
if [ -f "$WRANDR_REAL" ]; then
    WRANDR_LABEL=$(getfattr -n security.SMACK64 --only-values "$WRANDR" 2>/dev/null || true)
    cp -f "$WRANDR_REAL" "$WRANDR" 2>/dev/null || fail "cannot restore wrandr"
    chmod 755 "$WRANDR" 2>/dev/null || true
    chown 0:0 "$WRANDR" 2>/dev/null || true
    [ -n "$WRANDR_LABEL" ] && setfattr -n security.SMACK64 -v "$WRANDR_LABEL" "$WRANDR" 2>/dev/null || true
    rm -f "$WRANDR_REAL" 2>/dev/null || true
    log "restored wrandr from wrandr.real"
elif [ -f "$RW_DIR/backup/wrandr.orig" ]; then
    cp -f "$RW_DIR/backup/wrandr.orig" "$WRANDR" 2>/dev/null || fail "cannot restore wrandr backup"
    chmod 755 "$WRANDR" 2>/dev/null || true
    log "restored wrandr from backup"
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

# Restore AppStandbyClock QML from the same pre-install backup.
for base in qml_eco3.0 new_qml qml_genesis; do
    for f in main.qml Screen_AppStandbyClock_Confirm.qml Screen_AppStandbyClock_Confirm_Profile.qml; do
        src="$LATEST/AppStandbyClock/$base/$f"
        [ -f "$src" ] || continue
        dst="$STANDBY_ROOT/$base/$f"
        label=$(getfattr -n security.SMACK64 --only-values "$dst" 2>/dev/null || true)
        mkdir -p "$STANDBY_ROOT/$base" 2>/dev/null
        cp -f "$src" "$dst" || fail "restore failed: AppStandbyClock/$base/$f"
        chmod 644 "$dst" 2>/dev/null || true
        chown 0:0 "$dst" 2>/dev/null || true
        [ -n "$label" ] && setfattr -n security.SMACK64 -v "$label" "$dst" 2>/dev/null || true
        log "restored AppStandbyClock/$base/$f"
    done
done

# Remove app files.
for f in HomeScreen/TweaksPage.qml component/TweaksTile.qml component/TweaksSwitch.qml \
         component/TweaksButton.qml component/TweaksZonePicker.qml; do
    rm -f "$QML_DIR/$f" 2>/dev/null || true
done
rm -rf "$ASSET_DIR" 2>/dev/null || true
rm -f "$SERVICE" 2>/dev/null || true

# Clear caches.
rm -rf /root/.cache/qtshadercache 2>/dev/null || true
rm -rf /root/.cache/AppHomeScreen 2>/dev/null || true
rm -rf /tmp/qmlcache 2>/dev/null || true
rm -rf /tmp/qt-qml-cache 2>/dev/null || true

# Remove runtime pieces, keep backups.  failures.log is deliberately kept
# for post-mortem after an uninstall.
rm -f "$RW_DIR/tweaks_daemon" "$RW_DIR/tweaks_persist" \
      "$RW_DIR/status.json" "$RW_DIR/daemon.log" "$RW_DIR/cluster_gamma" \
      "$RW_DIR/last_gamma" "$RW_DIR/config.conf" \
      "$RW_DIR/disclaimer_auto" "$RW_DIR/disclaimer_timeout_ms" \
      "$RW_DIR/zones.txt" "$RW_DIR/tzdata_version" "$RW_DIR/ota_call.out" \
      "$RW_DIR/dst_write_failed" "$RW_DIR/pcl_write_app" 2>/dev/null || true
rm -rf "$RW_DIR/zoneinfo" "$RW_DIR/zoneinfo.new" 2>/dev/null || true

# Release the vendor OTA offline flag only if Tweaks set it.
if [ -f "$RW_DIR/ota_block_owned" ] && [ -f "$OFFLINE_STATUS" ] && \
   [ "$(cat "$OFFLINE_STATUS" 2>/dev/null)" = "1" ]; then
    printf '0' > "$OFFLINE_STATUS" 2>/dev/null || true
    if command -v dbus-send >/dev/null 2>&1 && [ -f /tmp/session_appmgr ]; then
        bus=$(cat /tmp/session_appmgr 2>/dev/null)
        if [ -n "$bus" ]; then
            DBUS_SESSION_BUS_ADDRESS="$bus" dbus-send --session --print-reply \
                --reply-timeout=8000 --dest=com.lge.hkmc.avn.upgrade.fota \
                /fotaInterface \
                com.lge.hkmc.avn.upgrade.fota.fotaInterface.setOFFlineMode \
                int32:0 >> "$LOG" 2>&1 || \
            dbus-send --system --print-reply --reply-timeout=8000 \
                --dest=com.lge.hkmc.avn.upgrade.fota /fotaInterface \
                com.lge.hkmc.avn.upgrade.fota.fotaInterface.setOFFlineMode \
                int32:0 >> "$LOG" 2>&1 || true
        fi
    fi
    log "released OTA offline mode"
fi
if [ ! -f "$OFFLINE_STATUS" ] || [ "$(cat "$OFFLINE_STATUS" 2>/dev/null)" != "1" ]; then
    rm -f "$RW_DIR/ota_block_owned" 2>/dev/null || true
fi

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
log "=== tweaks restore done ==="
exit 0
