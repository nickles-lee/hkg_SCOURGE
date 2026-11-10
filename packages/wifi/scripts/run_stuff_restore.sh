#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# WIFI_PAYLOAD_RESTORE=1
# Removes the WiFi app and restores the QML that was present before install.
# Rename/copy this file to run_stuff.sh on the USB root, then plug it in.
set -u
umask 077
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

was_ro=0
cleanup_ro() {
    if [ "${IO_ACTIVE:-0}" = 1 ]; then io_restore_root || true; fi
}
trap cleanup_ro EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
[ -f "$USB_DIR/manifest.md5" ] || fail "manifest.md5 missing"
MD5SUM_BIN=md5sum
command -v md5sum >/dev/null 2>&1 || MD5SUM_BIN=md5sum.coreutils
if ! ( cd "$USB_DIR" && "$MD5SUM_BIN" -c manifest.md5 ) > "$RW_DIR/restore-manifest.log" 2>&1; then
    fail "manifest check failed"
fi

LATEST="$RW_DIR/backup/latest"
[ -d "$LATEST" ] || fail "no backup/latest; cannot restore"

. "$PAYLOAD/lib/install_io.sh"
io_init && io_settle_root || fail "root filesystem is not stably writable"

# Stop the service before synchronous cleanup: two independent controllers
# must never race to re-enable the radio while restore is removing files.
if command -v systemctl >/dev/null 2>&1; then
    systemctl stop wifi-daemon.service 2>/dev/null || true
    systemctl disable wifi-daemon.service 2>/dev/null || true
fi
if [ -x "$RW_DIR/wifi_daemon" ]; then
    "$RW_DIR/wifi_daemon" --mode off >> "$LOG" 2>&1 || fail "network cleanup failed; preserving runtime files"
fi
for p in /proc/[0-9]*; do
    executable=$(readlink "$p/exe" 2>/dev/null || true)
    case "$executable" in
        "$RW_DIR/wifi_daemon"|"$RW_DIR/wifi_daemon (deleted)"|\
        "$RW_DIR/bin/dropbearmulti"|"$RW_DIR/bin/dropbearmulti (deleted)")
            kill -TERM "${p#/proc/}" 2>/dev/null || true ;;
    esac
done
# Remove only our namespaced chains. Legacy untagged INPUT rules cannot be
# distinguished from vendor/owner rules and are deliberately not guessed at.
for family in iptables ip6tables; do
    IPT=$(command -v "$family" 2>/dev/null || true)
    [ ! -x "$RW_DIR/bin/$family" ] || IPT="$RW_DIR/bin/$family"
    [ -n "$IPT" ] || continue
    for chain in WIFI_APP_SSH WIFI_APP_CP; do
        for attempt in 1 2 3 4; do
            "$IPT" -D INPUT -i wlan0 -j "$chain" 2>/dev/null || break
        done
        for attempt in 1 2 3 4; do
            "$IPT" -D OUTPUT -o wlan0 -j "$chain" 2>/dev/null || break
        done
        for attempt in 1 2 3 4; do
            "$IPT" -D OUTPUT -o wlan0 -j "$chain" 2>/dev/null || break
        done
        "$IPT" -F "$chain" 2>/dev/null || true
        "$IPT" -X "$chain" 2>/dev/null || true
    done
done

# Restore QML from the pre-install backup.
for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM" "$THEME"; do
    if [ -f "$LATEST/$f" ]; then
        label=$(getfattr -n security.SMACK64 --only-values "$QML_DIR/$f" 2>/dev/null || true)
        mkdir -p "$QML_DIR/$(dirname "$f")" 2>/dev/null
        io_copy "$LATEST/$f" "$QML_DIR/$f" || fail "restore failed: $f"
        chmod 644 "$QML_DIR/$f" 2>/dev/null || true
        chown 0:0 "$QML_DIR/$f" 2>/dev/null || true
        if [ -n "$label" ]; then
            # Restore is a recovery path: onlycap can make security-xattr
            # writes fail from the '_' installer context.  The restored file
            # already carries the inherited '_' floor label (readable by all),
            # so warn and continue instead of stranding the user.
            if setfattr -n security.SMACK64 -v "$label" "$QML_DIR/$f" 2>/dev/null; then
                actual=$(getfattr -n security.SMACK64 --only-values "$QML_DIR/$f" 2>/dev/null || true)
                [ "$actual" = "$label" ] || log "WARNING: SMACK64 readback mismatch on $f; expected $label, got '$actual'"
            else
                actual=$(getfattr -n security.SMACK64 --only-values "$QML_DIR/$f" 2>/dev/null || true)
                log "WARNING: cannot set SMACK64=$label on $f (onlycap?); keeping label '$actual'"
            fi
        fi
        log "restored $f"
    fi
done

# Remove new files/assets.
for f in HomeScreen/WifiPage.qml component/WifiTile.qml component/WifiSwitch.qml component/WifiButton.qml; do
    if cmp -s "$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$f" "$QML_DIR/$f"; then
        rm -f "$QML_DIR/$f" 2>/dev/null || true
    else
        log "kept changed/unknown component: $f"
    fi
done
for source in "$PAYLOAD/assets/wifi/"*; do
    [ -f "$source" ] || continue
    target="$ASSET_DIR/$(basename "$source")"
    if cmp -s "$source" "$target"; then rm -f "$target" 2>/dev/null || true; fi
done
rmdir "$ASSET_DIR" 2>/dev/null || true
rm -f "$SERVICE" 2>/dev/null || true

# Restore only our flag, from the immutable baseline for this exact path.
# Never pick a timestamp backup belonging to another file or replace newer
# unrelated Bluetooth settings. Legacy ambiguous backups are left alone.
vbt_changed=0
restore_vbt() {
    identity=$1
    vbt=$2
    bak="$RW_DIR/backup/vbt-$identity.original"
    applied="$RW_DIR/backup/vbt-$identity.applied"
    [ -f "$vbt" ] && [ -f "$bak" ] && [ -f "$applied" ] || return 0
    original=$(sed -n 's/.*"ENABLE_WIRELESS_CARPLAY"[[:space:]]*:[[:space:]]*\(true\|false\).*/\1/p' "$bak" | head -1)
    tmp="$vbt.wifi-restore.$$"
    cp -p "$vbt" "$tmp" || fail "cannot prepare VBT restore: $vbt"
    if [ -n "$original" ]; then
        sed "s/\(\"ENABLE_WIRELESS_CARPLAY\"[[:space:]]*:[[:space:]]*\)\(true\|false\)/\1$original/" "$vbt" > "$tmp" || fail "cannot restore VBT flag"
    elif cmp -s "$vbt" "$applied"; then
        cat "$bak" > "$tmp" || fail "cannot restore absent VBT flag"
    else
        rm -f "$tmp"
        fail "VBT baseline lacked flag and file changed independently; manual restore required: $vbt"
    fi
    label=$(getfattr -n security.SMACK64 --only-values "$vbt" 2>/dev/null || true)
    if [ -n "$label" ]; then
        setfattr -n security.SMACK64 -v "$label" "$tmp" 2>/dev/null \
            || log "WARNING: cannot set SMACK64=$label on VBT restore (onlycap?); keeping inherited label"
    fi
    mv -f "$tmp" "$vbt" || fail "VBT restore failed: $vbt"
    vbt_changed=1
}
restore_vbt app /app/data/vbt/VBTConfig.json
restore_vbt rw /rw_data/app_data/vbt/VBTConfig.json
[ "$vbt_changed" != 1 ] || systemctl restart vbtd.service >> "$LOG" 2>&1 || fail "vbtd restart failed"

# ---- mdnsd patch + carplay_mdnsd drop-in -------------------------------
# Undo the optional mdnsd "continue as root" patch (restore the stock
# binary from the installer backup) and remove the Restart=always drop-in
# so the vendor unit behaves like stock again.
MDNSD_BIN=/app/bin/mdnsd
MDNSD_STOCK_MD5=f63a65c4c2b0c011ae8ed17557b45c1b
MDNSD_PATCHED_MD5=f847373d7c5a6906453ba921bb790d22
MDNSD_BAK="$RW_DIR/backup/mdnsd.stock"
if [ -f "$MDNSD_BAK" ] && [ -f "$MDNSD_BIN" ]; then
    if [ "$("$MD5SUM_BIN" "$MDNSD_BIN" 2>/dev/null | cut -d' ' -f1)" != "$MDNSD_STOCK_MD5" ]; then
        [ "$("$MD5SUM_BIN" "$MDNSD_BAK" | cut -d' ' -f1)" = "$MDNSD_STOCK_MD5" ] || fail "mdnsd stock backup hash mismatch"
        [ "$("$MD5SUM_BIN" "$MDNSD_BIN" | cut -d' ' -f1)" = "$MDNSD_PATCHED_MD5" ] || fail "mdnsd changed independently; refusing restore"
        systemctl stop carplay_mdnsd.service >> "$LOG" 2>&1 || fail "cannot stop mdnsd for restore"
        if io_copy "$MDNSD_BAK" "$MDNSD_BIN"; then
            if [ "$("$MD5SUM_BIN" "$MDNSD_BIN" 2>/dev/null | cut -d' ' -f1)" = "$MDNSD_STOCK_MD5" ]; then
                log "restored stock mdnsd from backup"
                systemctl restart carplay_mdnsd.service >> "$LOG" 2>&1 || true
            else
                log "WARNING: mdnsd restore verification failed"
            fi
        else
            log "WARNING: cannot copy mdnsd backup over $MDNSD_BIN"
        fi
    fi
fi
if grep -q '^# SCOURGE_MDNSD_RESTART=1$' /etc/systemd/system/carplay_mdnsd.service.d/50-scourge-restart.conf 2>/dev/null; then
    if io_remove /etc/systemd/system/carplay_mdnsd.service.d/50-scourge-restart.conf; then
        rmdir /etc/systemd/system/carplay_mdnsd.service.d 2>/dev/null || true
        systemctl daemon-reload >> "$LOG" 2>&1 || true
        log "removed carplay_mdnsd restart drop-in"
    else
        log "WARNING: cannot remove carplay_mdnsd drop-in"
    fi
fi

# Remove only car: entries this installer created (recorded at install
# time).  An pre-existing car account from earlier owner tooling is kept.
remove_car() {
    f="$1"
    [ -f "$f" ] || return 0
    marker="$RW_DIR/backup/accounts/$(basename "$f")"
    [ -f "$marker" ] || return 0     # legacy/global ownership is ambiguous
    expected=$(cat "$marker")
    current=$(grep '^car:' "$f" 2>/dev/null || true)
    [ "$current" = "$expected" ] || { log "kept independently changed account in $f"; return 0; }
    tmp="/tmp/wifi-car-remove.$$"
    grep -v '^car:' "$f" > "$tmp" 2>/dev/null || true
    cat "$tmp" > "$f" 2>/dev/null || true
    rm -f "$tmp"
    rm -f "$marker"
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
      "$RW_DIR/config.conf" "$RW_DIR/wifi.txt" "$RW_DIR/home.json" \
       "$RW_DIR/apply.result" "$RW_DIR/bt.state" "$RW_DIR/ssh.bound" \
       "$RW_DIR/ap.station-ip" \
      "$RW_DIR/wpa_supplicant.conf" "$RW_DIR/wpa_supplicant.log" \
      "$RW_DIR/wpa_supplicant.pid" "$RW_DIR/connman.agent" "$RW_DIR/scan.json" 2>/dev/null || true
rm -f "$RW_DIR/bin/dropbearmulti" "$RW_DIR/bin/dropbear" "$RW_DIR/bin/dropbearkey" \
      "$RW_DIR/bin/iptables" "$RW_DIR/bin/ip6tables" \
      "$RW_DIR/ssh/authorized_keys" "$RW_DIR/ssh/dropbear.pid" \
      "$RW_DIR/ssh/dropbear_ed25519_host_key" "$RW_DIR/ssh/dropbear_rsa_host_key" \
      "$RW_DIR/home/.ssh/authorized_keys" 2>/dev/null || true
rmdir "$RW_DIR/bin" "$RW_DIR/ssh" "$RW_DIR/home/.ssh" "$RW_DIR/home" 2>/dev/null || true

# Restore pre-existing private keys and icon assets rather than deleting
# unrelated owner tooling's files. First-install transaction paths are exact.
if [ -f "$LATEST/transaction/files" ]; then
    while IFS='|' read -r item existed destination; do
        [ "$existed" = 1 ] || continue
        case "$destination" in
            "$RW_DIR/ssh/authorized_keys"|"$RW_DIR/home/.ssh/authorized_keys"|"$QML_ROOT/wifi/"*|"$QML_ROOT/hello/"*|"$QML_ROOT/battery/"*)
                io_mkdir "$(dirname "$destination")" || fail "cannot restore original file directory"
                io_restore_entry "$LATEST/transaction/$item" "$destination" || fail "cannot restore original owner file: $destination"
                ;;
        esac
    done < "$LATEST/transaction/files"
fi

if command -v systemctl >/dev/null 2>&1; then
    # The installer masked the OEM WiFi manager so it could not respawn and
    # fight for the control socket.  Put the stock state back.
    for unit in supplicantmanager.path supplicantmanager.service; do
        state=$(cat "$RW_DIR/backup/$unit.state" 2>/dev/null || true)
        if [ -n "$state" ] && [ "$state" != masked ] && [ "$state" != masked-runtime ]; then
            systemctl unmask "$unit" 2>/dev/null || fail "cannot restore OEM unit mask: $unit"
        else
            log "preserved pre-existing/unknown mask state: $unit"
        fi
    done
    systemctl daemon-reload 2>/dev/null || true
    state=$(cat "$RW_DIR/backup/supplicantmanager.path.state" 2>/dev/null || true)
    case "$state" in enabled|static|indirect) systemctl start supplicantmanager.path 2>/dev/null || true;; esac
fi
sync

# Restart AppHomeScreen.
restart="not-attempted"
old_home_pid=$(pidof AppHomeScreen 2>/dev/null || true)
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

io_restore_root || fail "cannot restore root mount state"

sleep 3
new_home_pid=$(pidof AppHomeScreen 2>/dev/null || true)
status=OK
if [ -z "$new_home_pid" ] || [ "$new_home_pid" = "$old_home_pid" ]; then
    status=REBOOT_REQUIRED
fi
result "status=$status" "restored_from=$LATEST" "restart=$restart" "log=$LOG"
log "=== wifi restore done ==="
exit 0
