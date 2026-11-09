#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TWEAKS_PAYLOAD_REV=1
# TWEAKS_PAYLOAD_ID=dev
# Tweaks / night-mode fixes for Ioniq 5 Gen5W (260128).
#
# Copy the contents of the USB tree to the root of a FAT/exFAT stick.  The
# initial_root failsafe runs run_stuff.sh as root.
#
# Installs:
#   /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/TweaksPage.qml
#   /app/share/AppHomeScreen/qml_eco3.0/component/Tweaks{Tile,Switch,Button}.qml
#   patched Advanced / Wide / AllMenus / Setup / shared menu QML
#   /app/share/AppHomeScreen/tweaks/{tweaks_icon_n,tweaks_icon_d}.png
#   /rw_data/tweaks/tweaks_daemon
#   /rw_data/tweaks/tweaks_persist
#   /usr/bin/wrandr (wrapper) + /usr/bin/wrandr.real (original)
#   /etc/systemd/system/tweaks-daemon.service
#
# It does not flash firmware, touch the updater, bootloader or partitions.
set -u
PATH="${TWEAKS_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
BACKUP="$USB_DIR/backup"
QML_ROOT="${TWEAKS_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/tweaks"
STANDBY_ROOT="${TWEAKS_STANDBY_ROOT:-/app/share/AppStandbyClock}"
OFFLINE_STATUS="${TWEAKS_OFFLINE_STATUS:-/update/offline_status}"
RW_DIR="${TWEAKS_RW_DIR:-/rw_data/tweaks}"
LOG="$RW_DIR/install.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${TWEAKS_SERVICE:-/etc/systemd/system/tweaks-daemon.service}"

ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
WIDE="HomeScreen/QHMI_HomeScreenMain_Wide.qml"
ALL="HomeScreen/Screen_HomeScreen_AllMenus.qml"
SETUP="HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
CUSTOM="component/QCustomizedMenuListAllowEmpty.qml"
THEME="component/common/Q_ThemeImage.qml"

mkdir -p "$RW_DIR" 2>/dev/null
mkdir -p "$RW_DIR/backup" 2>/dev/null
: > "$LOG" 2>/dev/null

log() {
    echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null
}

result() {
    {
        echo "TWEAKS_PAYLOAD"
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

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
log "=== tweaks payload start usb=$USB_DIR ==="

[ -d "$PAYLOAD" ] || fail "payload directory missing: $PAYLOAD"
[ -d "$BACKUP" ] || fail "backup directory missing: $BACKUP"
[ -f "$USB_DIR/manifest.md5" ] || fail "manifest.md5 missing"
[ -d "$QML_DIR" ] || fail "expected QML directory missing: $QML_DIR"

if ! ( cd "$USB_DIR" && md5sum -c manifest.md5 ) > "$RW_DIR/manifest-check.log" 2>&1; then
    log "manifest check failed"
    fail "manifest check failed (see /rw_data/tweaks/manifest-check.log)"
fi
log "manifest OK"

# ---- pick QML variants -------------------------------------------------
cur_adv="$QML_DIR/$ADV"
cur_wide="$QML_DIR/$WIDE"
cur_all="$QML_DIR/$ALL"
cur_setup="$QML_DIR/$SETUP"
cur_custom="$QML_DIR/$CUSTOM"
cur_theme="$QML_DIR/$THEME"
[ -f "$cur_adv" ] || fail "target missing: $cur_adv"
[ -f "$cur_wide" ] || fail "target missing: $cur_wide"
[ -f "$cur_all" ] || fail "target missing: $cur_all"
[ -f "$cur_setup" ] || fail "target missing: $cur_setup"
[ -f "$cur_custom" ] || fail "target missing: $cur_custom"

md5() { md5sum "$1" 2>/dev/null | cut -d' ' -f1; }
ref_md5() { cat "$PAYLOAD/ref/$1.md5" 2>/dev/null | cut -d' ' -f1; }

has_mark() { grep -q "$2" "$1" 2>/dev/null; }

adv_variant=""
has_battery_tile=0
grep -q 'id: batteryTile1' "$cur_adv" 2>/dev/null && has_battery_tile=1

if [ "$has_battery_tile" = 1 ]; then
    has_hello=0
    has_battery=0
    has_wifi=0
    has_mark "$cur_adv" "HELLO_PAYLOAD_260128" && has_hello=1
    has_mark "$cur_adv" "BATTERY_PAYLOAD_260128" && has_battery=1
    has_mark "$cur_adv" "WIFI_PAYLOAD_260128" && has_wifi=1
    if [ "$has_battery" = 1 ]; then
        if [ "$has_wifi" = 1 ]; then
            if [ "$has_hello" = 1 ]; then
                adv_variant=hello_battery_tile_wifi
            else
                adv_variant=battery_tile_wifi
            fi
        else
            if [ "$has_hello" = 1 ]; then
                adv_variant=battery_tile_hello
            else
                adv_variant=battery_tile
            fi
        fi
    fi
fi

if [ -z "$adv_variant" ]; then
    for v in hello_battery_wifi battery_wifi hello_wifi wifi; do
        if ! has_mark "$cur_adv" "WIFI_PAYLOAD_260128"; then
            break
        fi
        case "$v" in
            hello_battery_wifi)
                has_mark "$cur_adv" "HELLO_PAYLOAD_260128" && has_mark "$cur_adv" "BATTERY_PAYLOAD_260128" && adv_variant="$v"
                ;;
            battery_wifi)
                has_mark "$cur_adv" "BATTERY_PAYLOAD_260128" && ! has_mark "$cur_adv" "HELLO_PAYLOAD_260128" && adv_variant="$v"
                ;;
            hello_wifi)
                has_mark "$cur_adv" "HELLO_PAYLOAD_260128" && ! has_mark "$cur_adv" "BATTERY_PAYLOAD_260128" && adv_variant="$v"
                ;;
            wifi)
                ! has_mark "$cur_adv" "HELLO_PAYLOAD_260128" && ! has_mark "$cur_adv" "BATTERY_PAYLOAD_260128" && adv_variant="$v"
                ;;
        esac
        [ -n "$adv_variant" ] && break
    done
fi

if [ -z "$adv_variant" ]; then
    if has_mark "$cur_adv" "BATTERY_PAYLOAD_260128"; then
        if has_mark "$cur_adv" "HELLO_PAYLOAD_260128"; then adv_variant="battery_hello"; else adv_variant="battery"; fi
    elif has_mark "$cur_adv" "HELLO_PAYLOAD_260128"; then
        adv_variant="hello"
    elif has_mark "$cur_adv" "TWEAKS_PAYLOAD_260128"; then
        adv_variant="stock"
    elif [ "$(md5 "$cur_adv")" = "$(ref_md5 stock_advanced)" ]; then
        adv_variant="stock"
    else
        fail "unknown Advanced QML (md5=$(md5 "$cur_adv"))"
    fi
fi

pick_named() {
    file="$1"
    ref="$2"
    if has_mark "$file" "TWEAKS_PAYLOAD_260128" && ! has_mark "$file" "WIFI_PAYLOAD_260128"; then
        if has_mark "$file" "HELLO_PAYLOAD_260128"; then echo hello; else echo stock; fi
    elif has_mark "$file" "TWEAKS_PAYLOAD_260128" || has_mark "$file" "WIFI_PAYLOAD_260128"; then
        if has_mark "$file" "HELLO_PAYLOAD_260128"; then echo hello_wifi; else echo wifi; fi
    elif has_mark "$file" "HELLO_PAYLOAD_260128"; then
        echo hello
    elif [ "$(md5 "$file")" = "$(ref_md5 "$ref")" ]; then
        echo stock
    else
        echo unknown
    fi
}
wide_variant=$(pick_named "$cur_wide" stock_wide)
[ "$wide_variant" != unknown ] || fail "unknown Wide QML"
all_variant=$(pick_named "$cur_all" stock_allmenus)
[ "$all_variant" != unknown ] || fail "unknown AllMenus QML"
setup_variant=$(pick_named "$cur_setup" stock_setup)
[ "$setup_variant" != unknown ] || fail "unknown Setup QML"
custom_variant=$(pick_named "$cur_custom" stock_custom)
[ "$custom_variant" != unknown ] || fail "unknown shared menu QML"

log "variants: adv=$adv_variant wide=$wide_variant all=$all_variant setup=$setup_variant custom=$custom_variant"

if [ "${TWEAKS_DRY_RUN:-0}" = "1" ]; then
    result "status=DRY_RUN" "variant=$adv_variant/$wide_variant/$all_variant/$setup_variant/$custom_variant"
    exit 0
fi

# ---- backup ------------------------------------------------------------
STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null)
[ -n "$STAMP" ] || STAMP=manual
BACKUP_HU="$RW_DIR/backup/$STAMP"
[ -e "$BACKUP_HU" ] && BACKUP_HU="$BACKUP_HU.$$"
mkdir -p "$BACKUP_HU/HomeScreen" "$BACKUP_HU/component/common" 2>/dev/null || fail "cannot create $BACKUP_HU"

for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM" "$THEME"; do
    if [ -f "$QML_DIR/$f" ]; then
        mkdir -p "$BACKUP_HU/$(dirname "$f")" 2>/dev/null
        cp -f "$QML_DIR/$f" "$BACKUP_HU/$f" || fail "backup failed: $f"
        mkdir -p "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/$(dirname "$f")" 2>/dev/null
        cp -f "$QML_DIR/$f" "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/$f" 2>/dev/null || true
    fi
done
for base in qml_eco3.0 new_qml qml_genesis; do
    for f in main.qml Screen_AppStandbyClock_Confirm.qml Screen_AppStandbyClock_Confirm_Profile.qml; do
        src="$STANDBY_ROOT/$base/$f"
        [ -f "$src" ] || continue
        mkdir -p "$BACKUP_HU/AppStandbyClock/$base" 2>/dev/null
        cp -f "$src" "$BACKUP_HU/AppStandbyClock/$base/$f" || fail "backup failed: $base/$f"
        mkdir -p "$USB_DIR/backup_current/AppStandbyClock/$base" 2>/dev/null
        cp -f "$src" "$USB_DIR/backup_current/AppStandbyClock/$base/$f" 2>/dev/null || true
    done
done
if ! has_mark "$cur_adv" "TWEAKS_PAYLOAD_260128"; then
    ln -sfn "$BACKUP_HU" "$RW_DIR/backup/latest" 2>/dev/null || true
fi
# Upgrading from an older Tweaks install: make sure the restore point also
# contains the stock AppStandbyClock files captured above.
if [ -d "$RW_DIR/backup/latest" ]; then
    for base in qml_eco3.0 new_qml qml_genesis; do
        for f in main.qml Screen_AppStandbyClock_Confirm.qml Screen_AppStandbyClock_Confirm_Profile.qml; do
            src="$BACKUP_HU/AppStandbyClock/$base/$f"
            dst="$RW_DIR/backup/latest/AppStandbyClock/$base/$f"
            [ -f "$src" ] || continue
            [ -f "$dst" ] && continue
            mkdir -p "$(dirname "$dst")" 2>/dev/null
            cp -f "$src" "$dst" 2>/dev/null || true
        done
    done
fi
log "HU backup: $BACKUP_HU"

# ---- rootfs rw ---------------------------------------------------------
was_ro=0
grep -q ' / .* ro[, ]' /proc/mounts && was_ro=1
if [ "$was_ro" = 1 ]; then
    mount -o remount,rw / 2>/dev/null || fail "cannot remount / rw"
fi

# ---- assets / icons ----------------------------------------------------
mkdir -p "$ASSET_DIR" 2>/dev/null || fail "cannot create $ASSET_DIR"
for f in "$PAYLOAD/assets/tweaks/"*; do
    [ -f "$f" ] || continue
    cp -f "$f" "$ASSET_DIR/" || fail "asset copy failed: $f"
done
chmod 644 "$ASSET_DIR/"* 2>/dev/null || true

# ---- daemon / persist helper / wrapper / service ----------------------
cp -f "$PAYLOAD/bin/tweaks_daemon" "$RW_DIR/tweaks_daemon" || fail "daemon copy failed"
chmod 755 "$RW_DIR/tweaks_daemon" 2>/dev/null || true
chown 0:0 "$RW_DIR/tweaks_daemon" 2>/dev/null || true
# /rw_data is the SMACK floor label; the daemon inherits it. The floor label
# has rwx access to App::micommanager, which the cluster blue-light CAN calls
# need. Do not use a Privileged exec label here: it has no micommanager or
# system-bus rule (the Battery daemon is the car-proven pattern).
command -v chsmack >/dev/null 2>&1 && chsmack -e _ "$RW_DIR/tweaks_daemon" 2>/dev/null || true
DAEMON_SMACK=$(getfattr -n security.SMACK64EXEC --only-values "$RW_DIR/tweaks_daemon" 2>/dev/null || true)
[ -n "$DAEMON_SMACK" ] || DAEMON_SMACK=unset
PERSIST_SMACK=absent

if [ -f "$PAYLOAD/bin/tweaks_persist" ]; then
    cp -f "$PAYLOAD/bin/tweaks_persist" "$RW_DIR/tweaks_persist" || fail "persist helper copy failed"
    chmod 755 "$RW_DIR/tweaks_persist" 2>/dev/null || true
    chown 0:0 "$RW_DIR/tweaks_persist" 2>/dev/null || true
    command -v chsmack >/dev/null 2>&1 && chsmack -e _ "$RW_DIR/tweaks_persist" 2>/dev/null || true
    PERSIST_SMACK=$(getfattr -n security.SMACK64EXEC --only-values "$RW_DIR/tweaks_persist" 2>/dev/null || true)
    [ -n "$PERSIST_SMACK" ] || PERSIST_SMACK=unset
    # The vendor library normally has this SONAME symlink already; make it
    # best-effort if a stripped rootfs is missing it.
    if [ ! -e /usr/lib/libpersistence_client_library.so.7 ]; then
        PCL=$(ls /usr/lib/libpersistence_client_library.so.7.* 2>/dev/null | head -n 1)
        [ -n "$PCL" ] && ln -sf "$(basename "$PCL")" /usr/lib/libpersistence_client_library.so.7 2>/dev/null || true
    fi
fi

# Back up the vendor wrandr once.  Keep the real binary next to the wrapper.
WRANDR="${TWEAKS_WRANDR:-/usr/bin/wrandr}"
WRANDR_REAL="${TWEAKS_WRANDR_REAL:-/usr/bin/wrandr.real}"
if [ -f "$WRANDR" ] && ! grep -q 'TWEAKS_WRANDR_WRAPPER' "$WRANDR" 2>/dev/null; then
    # Always refresh the real binary; a firmware update may have replaced it.
    cp -p "$WRANDR" "$WRANDR_REAL" 2>/dev/null || cp -f "$WRANDR" "$WRANDR_REAL" || fail "cannot install wrandr.real"
    chmod 755 "$WRANDR_REAL" 2>/dev/null || true
    chown 0:0 "$WRANDR_REAL" 2>/dev/null || true
    if [ ! -f "$RW_DIR/backup/wrandr.orig" ]; then
        cp -f "$WRANDR" "$RW_DIR/backup/wrandr.orig" 2>/dev/null || true
        cp -f "$WRANDR" "$USB_DIR/backup_current/wrandr.orig" 2>/dev/null || true
    fi
fi
WRANDR_LABEL=""
if [ -e "$WRANDR" ]; then
    WRANDR_LABEL=$(getfattr -n security.SMACK64 --only-values "$WRANDR" 2>/dev/null || true)
fi
cp -f "$PAYLOAD/bin/wrandr" "$WRANDR" || fail "wrapper copy failed"
chmod 755 "$WRANDR" 2>/dev/null || true
chown 0:0 "$WRANDR" 2>/dev/null || true
[ -n "$WRANDR_LABEL" ] && setfattr -n security.SMACK64 -v "$WRANDR_LABEL" "$WRANDR" 2>/dev/null || true

cp -f "$PAYLOAD/systemd/tweaks-daemon.service" "$SERVICE" || fail "service copy failed"
chmod 644 "$SERVICE" 2>/dev/null || true
chown 0:0 "$SERVICE" 2>/dev/null || true

# ---- default config ----------------------------------------------------
if [ ! -f "$RW_DIR/config.conf" ]; then
    cat > "$RW_DIR/config.conf" <<EOF
# Tweaks daemon configuration
config_version=2
cluster_fix=1
infotainment_fix=1
cluster_crtc=2
main_crtc=1
warmth_override=0
force_filter_on=0
deep_red=0
projection_always=0
projection_app_id=0
poll_ms=1000
disclaimer_auto=1
disclaimer_timeout_ms=2000
auto_dst=1
dst_zone=Europe/Amsterdam
dst_check_ms=60000
ota_block=0
wrandr_path=/usr/bin/wrandr.real
persist_helper=$RW_DIR/tweaks_persist
tzdata_dir=$RW_DIR/zoneinfo
zones_file=$RW_DIR/zones.txt
tzdata_version_file=$RW_DIR/tzdata_version
offline_status_path=/update/offline_status
session_bus_file=/tmp/session_appmgr
dbus_send_path=/usr/bin/dbus-send
xdg_runtime_dir=/var/run/user/root
EOF
    chmod 600 "$RW_DIR/config.conf" 2>/dev/null || true
fi

# ---- tzdata -----------------------------------------------------------
if [ -d "$PAYLOAD/tzdata/zoneinfo" ]; then
    rm -rf "$RW_DIR/zoneinfo.new" 2>/dev/null || true
    cp -a "$PAYLOAD/tzdata/zoneinfo" "$RW_DIR/zoneinfo.new" || fail "tzdata install failed"
    rm -rf "$RW_DIR/zoneinfo" 2>/dev/null || true
    mv "$RW_DIR/zoneinfo.new" "$RW_DIR/zoneinfo" || fail "tzdata activation failed"
    [ -f "$PAYLOAD/tzdata/zones.txt" ] && cp -f "$PAYLOAD/tzdata/zones.txt" "$RW_DIR/zones.txt" || true
    [ -f "$PAYLOAD/tzdata/version.txt" ] && cp -f "$PAYLOAD/tzdata/version.txt" "$RW_DIR/tzdata_version" || true
    chmod -R a+rX "$RW_DIR/zoneinfo" 2>/dev/null || true
fi

set_conf_key() {
    ck="$1"
    cv="$2"
    cf="$3"
    awk -v k="$ck" -v v="$cv" '
        BEGIN { done = 0 }
        index($0, k "=") == 1 { print k "=" v; done = 1; next }
        { print }
        END { if (!done) print k "=" v }
    ' "$cf" > "$cf.tmp.$$" && mv -f "$cf.tmp.$$" "$cf"
}

# Migrate the v1 configuration in place. Only the exact legacy pair is
# rewritten; deliberate custom CRTC numbers are preserved.
cfg_value() {
    key="$1"
    fallback="$2"
    value=$(sed -n "s/^${key}=//p" "$RW_DIR/config.conf" 2>/dev/null | tail -n 1)
    [ -n "$value" ] && printf '%s' "$value" || printf '%s' "$fallback"
}
if ! grep -q '^config_version=2$' "$RW_DIR/config.conf" 2>/dev/null; then
    if [ "$(cfg_value main_crtc 1)" = "0" ] && \
       [ "$(cfg_value cluster_crtc 2)" = "1" ]; then
        set_conf_key main_crtc 1 "$RW_DIR/config.conf"
        set_conf_key cluster_crtc 2 "$RW_DIR/config.conf"
        log "config migrated: legacy CRTC pair 0/1 -> 1/2"
    fi
    set_conf_key config_version 2 "$RW_DIR/config.conf"
fi
if ! grep -q '^xdg_runtime_dir=' "$RW_DIR/config.conf" 2>/dev/null; then
    set_conf_key xdg_runtime_dir /var/run/user/root "$RW_DIR/config.conf"
fi

# Mirror the two disclaimer settings as small files for AppStandbyClock.
printf '%s' "$(cfg_value disclaimer_auto 1)" > "$RW_DIR/disclaimer_auto"
printf '%s' "$(cfg_value disclaimer_timeout_ms 2000)" > "$RW_DIR/disclaimer_timeout_ms"
chmod 600 "$RW_DIR/disclaimer_auto" "$RW_DIR/disclaimer_timeout_ms" 2>/dev/null || true

if [ "$(cfg_value ota_block 0)" = "1" ]; then
    mkdir -p "$(dirname "$OFFLINE_STATUS")" 2>/dev/null || true
    printf '1' > "$OFFLINE_STATUS" 2>/dev/null || true
    printf '1' > "$RW_DIR/ota_block_owned" 2>/dev/null || true
else
    if [ -f "$RW_DIR/ota_block_owned" ]; then
        printf '0' > "$OFFLINE_STATUS" 2>/dev/null || true
    fi
    rm -f "$RW_DIR/ota_block_owned" 2>/dev/null || true
fi

# ---- QML ---------------------------------------------------------------
install_qml() {
    src="$1"
    dst="$2"
    ref="$3"
    [ -f "$src" ] || fail "payload QML missing: $src"
    label=$(getfattr -n security.SMACK64 --only-values "$ref" 2>/dev/null || true)
    mkdir -p "$(dirname "$dst")" 2>/dev/null || fail "cannot create dir for $dst"
    cp -f "$src" "$dst" || fail "copy failed: $dst"
    chmod 644 "$dst" 2>/dev/null || true
    chown 0:0 "$dst" 2>/dev/null || true
    [ -n "$label" ] && setfattr -n security.SMACK64 -v "$label" "$dst" 2>/dev/null || true
}

Q="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0"
install_qml "$Q/HomeScreen/QHMI_HomeScreenMain_Advanced.$adv_variant.qml" "$cur_adv" "$cur_adv"
install_qml "$Q/HomeScreen/QHMI_HomeScreenMain_Wide.$wide_variant.qml" "$cur_wide" "$cur_wide"
install_qml "$Q/HomeScreen/Screen_HomeScreen_AllMenus.$all_variant.qml" "$cur_all" "$cur_all"
install_qml "$Q/HomeScreen/Screen_HomeScreen_Submenu_Setup.$setup_variant.qml" "$cur_setup" "$cur_setup"
install_qml "$Q/component/QCustomizedMenuListAllowEmpty.$custom_variant.qml" "$cur_custom" "$cur_custom"
if ! grep -q 'indexOf("file://")' "$cur_theme" 2>/dev/null; then
    install_qml "$Q/component/common/Q_ThemeImage.qml" "$cur_theme" "$cur_theme"
fi

for f in HomeScreen/TweaksPage.qml component/TweaksTile.qml component/TweaksSwitch.qml \
         component/TweaksButton.qml component/TweaksZonePicker.qml; do
    install_qml "$Q/$f" "$QML_DIR/$f" "$cur_adv"
done

# AppStandbyClock disclaimer auto-accept. Takes effect on the next boot.
for base in qml_eco3.0 new_qml qml_genesis; do
    dst_dir="$STANDBY_ROOT/$base"
    [ -d "$dst_dir" ] || continue
    for f in main.qml Screen_AppStandbyClock_Confirm.qml Screen_AppStandbyClock_Confirm_Profile.qml; do
        src="$PAYLOAD/qml/AppStandbyClock/$base/$f"
        [ -f "$src" ] || continue
        install_qml "$src" "$dst_dir/$f" "$dst_dir/$f"
    done
done

# Clear QML caches.
rm -rf /root/.cache/qtshadercache 2>/dev/null || true
rm -rf /root/.cache/AppHomeScreen 2>/dev/null || true
rm -rf /tmp/qmlcache 2>/dev/null || true
rm -rf /tmp/qt-qml-cache 2>/dev/null || true

sync

# ---- start daemon ------------------------------------------------------
daemon_status="not-started"
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >> "$LOG" 2>&1 || true
    if systemctl enable tweaks-daemon.service >> "$LOG" 2>&1; then
        if systemctl restart tweaks-daemon.service >> "$LOG" 2>&1; then
            daemon_status="systemd-ok"
        else
            daemon_status="systemd-restart-failed"
        fi
    else
        daemon_status="systemd-enable-failed"
    fi
fi
if [ "$daemon_status" != "systemd-ok" ]; then
    if [ -x "$RW_DIR/tweaks_daemon" ] && ! pidof tweaks_daemon >/dev/null 2>&1; then
        ( "$RW_DIR/tweaks_daemon" --port 8091 >> "$RW_DIR/daemon.log" 2>&1 & ) 2>/dev/null || true
        sleep 1
    fi
    if pidof tweaks_daemon >/dev/null 2>&1; then
        daemon_status="$daemon_status+manual"
    fi
fi
log "daemon=$daemon_status"

if [ "$was_ro" = 1 ]; then
    mount -o remount,ro / 2>/dev/null || true
fi

# ---- restart AppHomeScreen --------------------------------------------
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

if [ "$DAEMON_SMACK" != "_" ]; then
    log "WARNING: tweaks_daemon SMACK64EXEC=$DAEMON_SMACK (expected _); daemon may be unable to reach micom"
fi
if [ "$PERSIST_SMACK" != "_" ] && [ "$PERSIST_SMACK" != "absent" ]; then
    log "WARNING: tweaks_persist SMACK64EXEC=$PERSIST_SMACK (expected _)"
fi

result "status=OK" "variant=$adv_variant/$wide_variant/$all_variant/$setup_variant/$custom_variant" \
       "backup=$BACKUP_HU" "daemon=$daemon_status" "restart=$restart" \
       "smack=$DAEMON_SMACK" "persist_smack=$PERSIST_SMACK" \
       "disclaimer=next-boot" "tzdata=$(cat "$RW_DIR/tzdata_version" 2>/dev/null | head -n 1)" \
       "ota_block=$(cfg_value ota_block 0)" "log=$LOG"
log "=== tweaks payload done ==="
exit 0
