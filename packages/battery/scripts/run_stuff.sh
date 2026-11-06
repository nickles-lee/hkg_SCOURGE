#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# BATTERY_PAYLOAD_REV=1
# Battery preconditioning app for Ioniq 5 Gen5W (260128).
#
# Put this file, payload/, backup/, backup_current/ and manifest.md5 at the
# ROOT of a FAT/exFAT USB stick. The initial-root failsafe runs it as root
# within ~8 s of a content change.
#
# Installs:
#   /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml
#   /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/BatteryPage.qml
#   /app/share/AppHomeScreen/qml_eco3.0/component/BatterySwitch.qml
#   /app/share/AppHomeScreen/qml_eco3.0/component/BatteryTile.qml
#   /app/share/AppHomeScreen/battery/{battery_icon_n,battery_icon_d}.png
#   /rw_data/battery/battery_daemon
#   /etc/systemd/system/battery-daemon.service
#
# It backs up the current Advanced QML to /rw_data/battery/backup/<timestamp>/
# and to the USB backup_current/. It does not flash firmware and does not touch
# the updater, bootloader, or partitions.
set -u
PATH="${BATTERY_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
BACKUP="$USB_DIR/backup"
QML_ROOT="${BATTERY_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/battery"
RW_DIR="${BATTERY_RW_DIR:-/rw_data/battery}"
LOG="$RW_DIR/install.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${BATTERY_SERVICE:-/etc/systemd/system/battery-daemon.service}"

ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
NEW_FILES="HomeScreen/BatteryPage.qml
component/BatterySwitch.qml
component/BatteryTile.qml"

mkdir -p "$RW_DIR" 2>/dev/null
mkdir -p "$RW_DIR/backup" 2>/dev/null
: > "$LOG" 2>/dev/null

log() {
    echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null
}

result() {
    {
        echo "BATTERY_PAYLOAD"
        echo "time=$(date -u 2>/dev/null)"
        echo "usb=$USB_DIR"
        echo "qml_dir=$QML_DIR"
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

# Copy a QML file, retrying after unlinking the destination. A reinstall can
# meet an old file that was created by a previous payload with a different
# label/inode; the unlink makes the new copy inherit the directory label and
# also avoids text-file-busy style failures if the UI has the old file open.
copy_qml_file() {
    _src=$1
    _dst=$2
    _what=$3
    if cp -f "$_src" "$_dst" 2>>"$LOG"; then
        return 0
    fi
    log "WARNING: copy failed: $_what; retrying after removing destination"
    if grep -q ' / .* ro[, ]' /proc/mounts; then
        log "root is ro again; remounting rw before retry"
        mount -o remount,rw / 2>>"$LOG" || true
    fi
    rm -f "$_dst" 2>>"$LOG" || true
    if cp -f "$_src" "$_dst" 2>>"$LOG"; then
        log "copy retry OK: $_what"
        return 0
    fi
    log "ERROR: copy retry failed: $_what"
    return 1
}

mount -o remount,rw "$USB_DIR" 2>/dev/null || true

log "=== battery payload start usb=$USB_DIR ==="

[ -d "$PAYLOAD" ] || fail "payload directory missing: $PAYLOAD"
[ -d "$BACKUP" ] || fail "backup directory missing: $BACKUP"
[ -f "$USB_DIR/manifest.md5" ] || fail "manifest.md5 missing"
[ -d "$QML_DIR" ] || fail "expected QML directory missing: $QML_DIR (not qml_eco3.0?)"

if ! ( cd "$USB_DIR" && md5sum -c manifest.md5 ) > "$RW_DIR/manifest-check.log" 2>&1; then
    log "manifest check failed"
    fail "manifest check failed (see /rw_data/battery/manifest-check.log)"
fi
log "manifest OK"

# Pick the Advanced variant that matches what is on the car.
cur="$QML_DIR/$ADV"
ref="$BACKUP/AppHomeScreen/qml_eco3.0/$ADV"
[ -f "$cur" ] || fail "target file missing: $cur"
cur_md5=$(md5sum "$cur" | cut -d' ' -f1)
ref_md5=$(md5sum "$ref" | cut -d' ' -f1)

variant=plain
already=0
hello_mismatch=0
if grep -q 'BATTERY_PAYLOAD_260128' "$cur" 2>/dev/null; then
    already=1
    if grep -q 'HELLO_PAYLOAD_260128' "$cur" 2>/dev/null && \
       grep -q 'WIFI_PAYLOAD_260128' "$cur" 2>/dev/null; then
        variant=hello_wifi
    elif grep -q 'WIFI_PAYLOAD_260128' "$cur" 2>/dev/null; then
        variant=wifi
    elif grep -q 'HELLO_PAYLOAD_260128' "$cur" 2>/dev/null; then
        variant=hello
    fi
elif grep -q 'WIFI_PAYLOAD_260128' "$cur" 2>/dev/null; then
    if grep -q 'HELLO_PAYLOAD_260128' "$cur" 2>/dev/null; then
        variant=hello_wifi
    else
        variant=wifi
    fi
elif grep -q 'HELLO_PAYLOAD_260128' "$cur" 2>/dev/null; then
    variant=hello
elif [ "$cur_md5" = "$ref_md5" ]; then
    variant=plain
else
    fail "unknown QML version for $ADV (current=$cur_md5 ref=$ref_md5)"
fi
# The Hello base check only means something on a FIRST install over a
# Hello-patched file.  On reinstall the current file is battery+hello patched
# and can never match the pre-battery base, so the warning would fire on
# every reinstall.
if [ "$variant" = "hello" ] && [ "$already" = 0 ]; then
    hello_base_md5=$(cat "$PAYLOAD/hello_base.md5" 2>/dev/null | cut -d' ' -f1)
    if [ -n "$hello_base_md5" ] && [ "$cur_md5" != "$hello_base_md5" ]; then
        hello_mismatch=1
        log "WARNING: Hello QML differs from the known artifact; installing anyway"
    fi
fi
log "variant=$variant already_patched=$already hello_mismatch=$hello_mismatch"

# Back up the file we are about to replace (original or Hello-patched).
STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null)
[ -n "$STAMP" ] || STAMP=manual
BACKUP_HU="$RW_DIR/backup/$STAMP"
mkdir -p "$BACKUP_HU/HomeScreen" 2>/dev/null || fail "cannot create $BACKUP_HU"
cp -f "$cur" "$BACKUP_HU/$ADV" || fail "backup failed: $ADV"
# Keep the first pre-install file as the restore point. On reinstall, the
# current file is already patched, so do not repoint latest/backup_current.
if [ "$already" = 0 ]; then
    ln -sfn "$BACKUP_HU" "$RW_DIR/backup/latest" 2>/dev/null || true
    mkdir -p "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/HomeScreen" 2>/dev/null || true
    cp -f "$cur" "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/$ADV" 2>/dev/null || true
fi
log "HU backup: $BACKUP_HU"

# Remount the rootfs rw while installing.
was_ro=0
grep -q ' / .* ro[, ]' /proc/mounts && was_ro=1
if [ "$was_ro" = 1 ]; then
    mount -o remount,rw / 2>/dev/null || fail "cannot remount / rw"
fi

# Assets.
mkdir -p "$ASSET_DIR" 2>/dev/null || fail "cannot create $ASSET_DIR"
for f in "$PAYLOAD/assets/battery/"*; do
    [ -f "$f" ] || continue
    cp -f "$f" "$ASSET_DIR/" || fail "asset copy failed: $f"
done
chmod 644 "$ASSET_DIR/"* 2>/dev/null || true

# Daemon.
mkdir -p "$RW_DIR" 2>/dev/null || fail "cannot create $RW_DIR"
cp -f "$PAYLOAD/bin/battery_daemon" "$RW_DIR/battery_daemon" || fail "daemon copy failed"
chmod 755 "$RW_DIR/battery_daemon" 2>/dev/null || true
chown root:root "$RW_DIR/battery_daemon" 2>/dev/null || true
# /rw_data is the SMACK floor label; the daemon inherits it. The floor label
# has rwx access to App::micommanager, which the charge-door D-Bus call needs.
# Do not use a Privileged exec label here: it has no micommanager access rule.
command -v chsmack >/dev/null 2>&1 && chsmack -e _ "$RW_DIR/battery_daemon" 2>/dev/null || true

# systemd unit.
cp -f "$PAYLOAD/systemd/battery-daemon.service" "$SERVICE" || fail "service copy failed"
chmod 644 "$SERVICE" 2>/dev/null || true
chown root:root "$SERVICE" 2>/dev/null || true

# QML.
installed=""
src="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$ADV"
adv_base="${ADV%.qml}"
case "$variant" in
    hello)
        src="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/${adv_base}.hello.qml"
        ;;
    wifi)
        src="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/${adv_base}.wifi.qml"
        ;;
    hello_wifi)
        src="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/${adv_base}.hello_wifi.qml"
        ;;
esac
[ -f "$src" ] || fail "payload variant missing: $src"
label=$(getfattr -n security.SMACK64 --only-values "$cur" 2>/dev/null || true)
copy_qml_file "$src" "$cur" "$ADV" || fail "copy failed: $ADV (see $LOG)"
chmod 644 "$cur" 2>/dev/null || true
chown root:root "$cur" 2>/dev/null || true
[ -n "$label" ] && setfattr -n security.SMACK64 -v "$label" "$cur" 2>/dev/null || true
installed="$installed $ADV($variant)"

for f in $NEW_FILES; do
    src="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$f"
    dst="$QML_DIR/$f"
    [ -f "$src" ] || fail "payload file missing: $src"
    mkdir -p "$QML_DIR/$(dirname "$f")" 2>/dev/null || fail "cannot create dir for $f"
    copy_qml_file "$src" "$dst" "$f" || fail "copy failed: $f (see $LOG)"
    chmod 644 "$dst" 2>/dev/null || true
    chown root:root "$dst" 2>/dev/null || true
    installed="$installed $f"
done
log "installed:$installed"

# Clear QML caches.
rm -rf /root/.cache/qtshadercache 2>/dev/null || true
rm -rf /root/.cache/AppHomeScreen 2>/dev/null || true
rm -rf /tmp/qmlcache 2>/dev/null || true
rm -rf /tmp/qt-qml-cache 2>/dev/null || true

sync

# Start the daemon now and enable it for future boots. This must happen while
# / is still rw because `systemctl enable` writes a wants symlink.
daemon_status="not-started"
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >> "$LOG" 2>&1 || true
    if systemctl enable battery-daemon.service >> "$LOG" 2>&1; then
        if systemctl restart battery-daemon.service >> "$LOG" 2>&1; then
            daemon_status="systemd-ok"
        else
            daemon_status="systemd-restart-failed"
        fi
    else
        daemon_status="systemd-enable-failed"
    fi
fi
if [ "$daemon_status" != "systemd-ok" ]; then
    # Fallback: run it detached.
    if [ -x "$RW_DIR/battery_daemon" ]; then
        if ! pidof battery_daemon >/dev/null 2>&1; then
            ( "$RW_DIR/battery_daemon" --iface can0 --port 8088 \
                --status-file "$RW_DIR/status.json" >> "$RW_DIR/daemon.log" 2>&1 & ) 2>/dev/null || true
            sleep 1
        fi
        if pidof battery_daemon >/dev/null 2>&1; then
            daemon_status="$daemon_status+manual"
        fi
    fi
fi
log "daemon=$daemon_status"

if [ "$was_ro" = 1 ]; then
    mount -o remount,ro / 2>/dev/null || true
fi

# Restart AppHomeScreen through AppLauncher (app id 11).
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
        if [ -n "$val" ]; then
            export "$var=$val"
        fi
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
else
    restart="dbus-send-missing"
fi

if [ "$restart" != "dbus-ok" ]; then
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        if [ "$(cat "$p/comm" 2>/dev/null)" = "AppHomeScreen" ]; then
            kill -TERM "${p#/proc/}" 2>/dev/null || true
        fi
    done
    restart="$restart (reboot required if UI does not return)"
fi

log "restart=$restart"
sync

result "status=OK" "already_patched=$already" "variant=$variant" \
       "hello_mismatch=$hello_mismatch" \
       "backup=$BACKUP_HU" "installed=$installed" "daemon=$daemon_status" \
       "restart=$restart" "log=$LOG"
log "=== battery payload done ==="
exit 0
