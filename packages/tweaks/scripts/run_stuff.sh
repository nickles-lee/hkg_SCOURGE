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
#   /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/RebootPage.qml
#   /app/share/AppHomeScreen/qml_eco3.0/component/Tweaks{Tile,Switch,Button}.qml
#   /app/share/AppHomeScreen/qml_eco3.0/component/RebootTile.qml
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
# The daemon and its persistence helper are stamped with this exact label.
# On the car this is read from the installer's own /proc/self/attr/current
# ("_" in the normal initial-root/USB flow); TWEAKS_SMACK_SELF is test-only.
SMACK_SELF="${TWEAKS_SMACK_SELF:-}"

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

smack_init() {
    if [ -n "${TWEAKS_SMACK_SELF:-}" ]; then
        SMACK_SELF=$TWEAKS_SMACK_SELF
        [ "$SMACK_SELF" = "_" ] || \
            fail "expected SMACK floor label '_', got '$SMACK_SELF'"
        log "SMACK: using override label '$SMACK_SELF'"
        return 0
    fi
    grep -q ' smackfs ' /proc/mounts 2>/dev/null || \
        fail "smackfs is not mounted (SMACK is required)"
    command -v chsmack >/dev/null 2>&1 || fail "chsmack missing (SMACK is required)"
    SMACK_SELF=$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')
    [ -n "$SMACK_SELF" ] || fail "cannot read /proc/self/attr/current"
    [ "$SMACK_SELF" = "_" ] || \
        fail "expected SMACK floor label '_', got '$SMACK_SELF'"
    log "SMACK: installer label '$SMACK_SELF'"
}

set_exec_label() {
    _bin=$1
    command -v chsmack >/dev/null 2>&1 || fail "chsmack missing (SMACK is required)"
    chsmack -e "$SMACK_SELF" "$_bin" >> "$LOG" 2>&1 || \
        fail "exec label failed: $_bin"
    _actual=$(getfattr -n security.SMACK64EXEC --only-values "$_bin" 2>/dev/null || true)
    # Readback-unreadable is a WARNING, not a gate failure — this is the
    # on-car-proven onlycap=Privileged behavior (2026-11-10 install log:
    # getfattr returns "Operation not permitted" from the '_' installer
    # context; state.txt shows SMACK64EXEC "No such attribute" while the
    # daemons run as '_' via systemd inheritance).  Compensating controls:
    #   - a READABLE wrong label is still fatal (the case that matters);
    #   - the daemon's running label is verified below (smack_check);
    #   - the persist helper is exec'd by that daemon, so it inherits the
    #     verified '_' context when the exec attribute is absent.
    if [ -z "$_actual" ]; then
        log "WARNING: cannot read back SMACK64EXEC for $_bin; trusting chsmack"
    elif [ "$_actual" != "$SMACK_SELF" ]; then
        fail "exec label readback failed: $_bin (expected '$SMACK_SELF', got '$_actual')"
    fi
    log "exec label ok: $_bin = ${_actual:-unverified}"
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
smack_init

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
. "$PAYLOAD/lib/install_io.sh"
io_init || fail "cannot identify root mount"
trap 'io_restore_root || true' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
io_settle_root || fail "root filesystem is not stably writable"

# ---- assets / icons ----------------------------------------------------
for f in "$PAYLOAD/assets/tweaks/"*; do
    [ -f "$f" ] || continue
    io_asset "$f" "$ASSET_DIR/$(basename "$f")"
done
chmod 644 "$ASSET_DIR/"* 2>/dev/null || true

# ---- daemon / persist helper / wrapper / service ----------------------
# Remove first: cp -f onto an existing inode keeps its extended attributes,
# including a stale SMACK64EXEC from an older install.
rm -f "$RW_DIR/tweaks_daemon" 2>/dev/null || true
cp -f "$PAYLOAD/bin/tweaks_daemon" "$RW_DIR/tweaks_daemon" || fail "daemon copy failed"
chmod 755 "$RW_DIR/tweaks_daemon" 2>/dev/null || true
chown 0:0 "$RW_DIR/tweaks_daemon" 2>/dev/null || true
# The daemon (and every helper it forks) must run on the installer's own
# SMACK floor label.  `_` has rwx access to App::micommanager, which both the
# cluster CAN writes and the Reboot hard-reset fallback need.  Never use
# `Privileged`: it has no micommanager or system-bus rule.
set_exec_label "$RW_DIR/tweaks_daemon"
DAEMON_SMACK=$(getfattr -n security.SMACK64EXEC --only-values "$RW_DIR/tweaks_daemon" 2>/dev/null || true)
[ -n "$DAEMON_SMACK" ] || DAEMON_SMACK=unverified
PERSIST_SMACK=absent

if [ -f "$PAYLOAD/bin/tweaks_persist" ]; then
    rm -f "$RW_DIR/tweaks_persist" 2>/dev/null || true
    cp -f "$PAYLOAD/bin/tweaks_persist" "$RW_DIR/tweaks_persist" || fail "persist helper copy failed"
    chmod 755 "$RW_DIR/tweaks_persist" 2>/dev/null || true
    chown 0:0 "$RW_DIR/tweaks_persist" 2>/dev/null || true
    set_exec_label "$RW_DIR/tweaks_persist"
    PERSIST_SMACK=$(getfattr -n security.SMACK64EXEC --only-values "$RW_DIR/tweaks_persist" 2>/dev/null || true)
    [ -n "$PERSIST_SMACK" ] || PERSIST_SMACK=unverified
    # The vendor library normally has this SONAME symlink already; make it
    # best-effort if a stripped rootfs is missing it.
    if [ ! -e /usr/lib/libpersistence_client_library.so.7 ]; then
        PCL=$(ls /usr/lib/libpersistence_client_library.so.7.* 2>/dev/null | head -n 1)
        [ -n "$PCL" ] && ln -sf "$(basename "$PCL")" /usr/lib/libpersistence_client_library.so.7 2>/dev/null || true
    fi
fi

# Expected label for the daemon's self-check; the Reboot fallback depends on
# the daemon actually running in this context.
printf '%s\n' "$SMACK_SELF" > "$RW_DIR/smack_expected" || \
    fail "cannot write smack_expected"
chmod 644 "$RW_DIR/smack_expected" 2>/dev/null || true
chown 0:0 "$RW_DIR/smack_expected" 2>/dev/null || true

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
io_copy "$PAYLOAD/bin/wrandr" "$WRANDR" || fail "wrapper copy failed"
chmod 755 "$WRANDR" 2>/dev/null || true
chown 0:0 "$WRANDR" 2>/dev/null || true
[ -n "$WRANDR_LABEL" ] && setfattr -n security.SMACK64 -v "$WRANDR_LABEL" "$WRANDR" 2>/dev/null || true

io_copy "$PAYLOAD/systemd/tweaks-daemon.service" "$SERVICE" || fail "service copy failed"
chmod 644 "$SERVICE" 2>/dev/null || true
chown 0:0 "$SERVICE" 2>/dev/null || true

# ---- micom CAN helper --------------------------------------------------
# The vendor micom service silently drops CAN sends unless the sender's
# executable basename is whitelisted (AppHomeScreen, TmsRemoteApp,
# micommanager_userspace, CANService); see
# battery/docs/MICOM_WHITELIST.md. Battery ships the car-proven helper, so
# prefer it. Otherwise install a local copy of the vendor dbus-send under the
# whitelisted name plus a wrapper with the same interface.
micom_helper_supports_fixed() {
    [ -f "$1" ] && grep -q 'SetCANFixedMicomRecv' "$1" 2>/dev/null && \
        grep -q '^fixed)' "$1" 2>/dev/null
}
BATTERY_DIR="${TWEAKS_BATTERY_DIR:-/rw_data/battery}"
CAN_SERVICE="$BATTERY_DIR/bin/micom-send"
CAN_SOURCE=battery
if [ -x "$CAN_SERVICE" ] && micom_helper_supports_fixed "$CAN_SERVICE"; then
    log "micom CAN helper: using Battery helper $CAN_SERVICE"
else
    if [ -x "$CAN_SERVICE" ]; then
        log "WARNING: Battery micom helper is raw-only (no fixed mode); installing the Tweaks copy"
    fi
    CAN_SOURCE=none
    CAN_BIN_DIR="$RW_DIR/bin"
    _dbus_send="${TWEAKS_DBUS_SEND:-$(command -v dbus-send 2>/dev/null || true)}"
    if [ -n "$_dbus_send" ] && [ -x "$_dbus_send" ]; then
        mkdir -p "$CAN_BIN_DIR" 2>/dev/null || fail "cannot create $CAN_BIN_DIR"
        # Remove first: cp -f keeps the destination inode and its xattrs.
        rm -f "$CAN_BIN_DIR/CANService" 2>/dev/null || true
        cp -f "$_dbus_send" "$CAN_BIN_DIR/CANService" || fail "helper copy failed: CANService"
        chmod 755 "$CAN_BIN_DIR/CANService" 2>/dev/null || fail "cannot make CANService executable"
        chown 0:0 "$CAN_BIN_DIR/CANService" 2>/dev/null || true
        set_exec_label "$CAN_BIN_DIR/CANService"
        rm -f "$CAN_BIN_DIR/micom-send" 2>/dev/null || true
        cat > "$CAN_BIN_DIR/micom-send" <<EOF || fail "cannot write micom-send wrapper"
#!/bin/sh
# Micom CAN-send helper (Tweaks fallback copy). The vendor service whitelists
# the sender's executable basename (CANService); see
# packages/battery/docs/MICOM_WHITELIST.md.
#   micom-send <array>                        raw SetCANMicomRecv (legacy)
#   micom-send raw <array>                    explicit raw form
#   micom-send fixed <idhi> <idlo> <start> <size> <data>
#                                             SetCANFixedMicomRecv
# --print-reply keeps this process alive while the micom resolves
# /proc/<pid>/exe. Exit 0 even when the micom does not reply (factory
# senders use no-reply), so callers treat the frame as queued.
case "\$1" in
fixed)
    "$CAN_BIN_DIR/CANService" --system --type=method_call --print-reply \\
        --reply-timeout=500 --dest=com.lge.car.micom /Can \\
        com.lge.car.micom.can.SetCANFixedMicomRecv \\
        "array:byte:\$2,\$3" "int32:\$4" "int32:\$5" "int32:\$6" >/dev/null 2>&1
    exit 0
    ;;
raw)
    shift
    ;;
esac
"$CAN_BIN_DIR/CANService" --system --type=method_call --print-reply \\
    --reply-timeout=500 --dest=com.lge.car.micom /Can \\
    com.lge.car.micom.can.SetCANMicomRecv "\$1" >/dev/null 2>&1
exit 0
EOF
        chmod 755 "$CAN_BIN_DIR/micom-send" 2>/dev/null || fail "cannot make micom-send executable"
        chown 0:0 "$CAN_BIN_DIR/micom-send" 2>/dev/null || true
        [ -x "$CAN_BIN_DIR/micom-send" ] || fail "micom-send wrapper is not executable"
        set_exec_label "$CAN_BIN_DIR/micom-send"
        CAN_SERVICE="$CAN_BIN_DIR/micom-send"
        CAN_SOURCE=local
    else
        log "WARNING: dbus-send not found; cluster CAN writes will fail the micom whitelist"
    fi
fi
log "micom CAN helper available: path=${CAN_SERVICE:-unset}"

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
can_service=$CAN_SERVICE
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
# Existing installs: record the detected helper, or migrate away from a helper
# that no longer exists (Battery uninstalled, local copy removed) or is an old
# raw-only wrapper without the fixed-field call.
can_conf=$(cfg_value can_service "")
if [ -n "$CAN_SERVICE" ] && { [ -z "$can_conf" ] || [ ! -x "$can_conf" ] || \
   ! micom_helper_supports_fixed "$can_conf"; }; then
    set_conf_key can_service "$CAN_SERVICE" "$RW_DIR/config.conf"
    [ "$(cfg_value can_service "")" = "$CAN_SERVICE" ] || \
        fail "cannot record can_service in config"
    log "config can_service -> $CAN_SERVICE ($CAN_SOURCE)"
fi
# Report the path the daemon will actually use, not just the one detected:
# an existing valid config is deliberately preserved.
CAN_SERVICE=$(cfg_value can_service "$CAN_SERVICE")
if [ -n "$CAN_SERVICE" ] && [ -x "$CAN_SERVICE" ] && \
   micom_helper_supports_fixed "$CAN_SERVICE"; then
    case "$CAN_SERVICE" in
        "$BATTERY_DIR/bin/micom-send") CAN_SOURCE=battery ;;
        "$RW_DIR/bin/micom-send") CAN_SOURCE=local ;;
        *) CAN_SOURCE=config ;;
    esac
else
    CAN_SOURCE=dbus
fi
log "micom CAN helper effective: source=$CAN_SOURCE path=${CAN_SERVICE:-unset}"

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
    io_mkdir "$(dirname "$dst")" || fail "cannot create dir for $dst"
    io_copy "$src" "$dst" || fail "copy failed: $dst"
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

for f in HomeScreen/TweaksPage.qml HomeScreen/RebootPage.qml \
         component/TweaksTile.qml component/RebootTile.qml \
         component/TweaksSwitch.qml component/TweaksButton.qml \
         component/TweaksZonePicker.qml; do
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

io_restore_root || fail "cannot restore root mount state"

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

# The Reboot hard-reset fallback is only allowed to reach the micom service
# from the floor label.  Prove both the file exec label and the running
# process label; TWEAKS_DAEMON_SMACK is a test-only override for fake roots.
RUNNING_SMACK="${TWEAKS_DAEMON_SMACK:-}"
if [ -z "$RUNNING_SMACK" ]; then
    for _try in 1 2 3 4 5; do
        for p in /proc/[0-9]*; do
            [ -r "$p/comm" ] || continue
            if [ "$(cat "$p/comm" 2>/dev/null)" = "tweaks_daemon" ]; then
                RUNNING_SMACK=$(cat "$p/attr/current" 2>/dev/null | tr -d '\r\n')
                break
            fi
        done
        [ -n "$RUNNING_SMACK" ] && break
        sleep 1
    done
fi
smack_check=unknown
case "$DAEMON_SMACK" in
    "$SMACK_SELF")
        if [ -n "$RUNNING_SMACK" ]; then
            if [ "$RUNNING_SMACK" = "$SMACK_SELF" ]; then
                smack_check=ok
            else
                smack_check=mismatch
                log "SMACK MISMATCH: running tweaks_daemon='$RUNNING_SMACK' expected='$SMACK_SELF'"
            fi
        else
            log "SMACK UNKNOWN: tweaks_daemon is not running"
        fi
        ;;
    unverified)
        if [ "$RUNNING_SMACK" = "$SMACK_SELF" ]; then
            smack_check=ok
        elif [ -n "$RUNNING_SMACK" ]; then
            smack_check=mismatch
            log "SMACK MISMATCH: running tweaks_daemon='$RUNNING_SMACK' expected='$SMACK_SELF'"
        else
            log "SMACK UNKNOWN: tweaks_daemon file label unreadable and process not found"
        fi
        ;;
    *)
        smack_check=mismatch
        log "SMACK MISMATCH: tweaks_daemon file label='$DAEMON_SMACK' expected='$SMACK_SELF'"
        ;;
esac
if [ "$PERSIST_SMACK" != "$SMACK_SELF" ] && [ "$PERSIST_SMACK" != "absent" ] && [ "$PERSIST_SMACK" != "unverified" ]; then
    log "SMACK MISMATCH: tweaks_persist='$PERSIST_SMACK' expected='$SMACK_SELF'"
    smack_check=mismatch
fi
# PERSIST_SMACK=unverified is accepted only because the helper is exec'd by
# the daemon whose running label smack_check verifies (see set_exec_label):
# with no readable exec attribute the helper inherits the daemon's '_'
# context.  A readable wrong label above remains fatal.
final_status=OK
[ "$smack_check" = "ok" ] || final_status=FAIL

result "status=$final_status" "variant=$adv_variant/$wide_variant/$all_variant/$setup_variant/$custom_variant" \
       "backup=$BACKUP_HU" "daemon=$daemon_status" "restart=$restart" \
       "smack_self=$SMACK_SELF" "smack=$DAEMON_SMACK" \
       "persist_smack=$PERSIST_SMACK" "daemon_smack=${RUNNING_SMACK:-none}" \
       "smack_check=$smack_check" \
       "can_service=${CAN_SERVICE:-unset}" "can_source=$CAN_SOURCE" \
       "disclaimer=next-boot" "tzdata=$(cat "$RW_DIR/tzdata_version" 2>/dev/null | head -n 1)" \
       "ota_block=$(cfg_value ota_block 0)" "asset_warnings=$ASSET_WARNINGS" "log=$LOG"
log "=== tweaks payload done ==="
[ "$final_status" = "OK" ] || exit 1
exit 0
