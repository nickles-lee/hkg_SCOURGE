#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU 260128
# WIFI_PAYLOAD_REV=1
# WiFi / CarPlay AP / SSH app for Ioniq 5 Gen5W (260128).
#
# Put this file, payload/, backup/, manifest.md5 and README.txt at the ROOT
# of a FAT/exFAT USB stick.  The initial-root failsafe runs it as root within
# ~8 s of a content change.
#
# Installs:
#   /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/WifiPage.qml
#   /app/share/AppHomeScreen/qml_eco3.0/component/Wifi{Tile,Switch,Button}.qml
#   patched Advanced / Wide / AllMenus / Setup QML
#   /app/share/AppHomeScreen/wifi/{wifi_icon_n,wifi_icon_d}.png
#   /rw_data/wifi/wifi_daemon
#   /rw_data/wifi/bin/dropbearmulti
#   /etc/systemd/system/wifi-daemon.service
#   user car (password electric) for SSH
#
# It backs up the current QML to /rw_data/wifi/backup/<timestamp>/ and to
# the USB backup_current/.  It does not flash firmware and does not touch
# the updater, bootloader or partitions.
set -u
umask 077
PATH="${WIFI_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
BACKUP="$USB_DIR/backup"
QML_ROOT="${WIFI_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/wifi"
RW_DIR="${WIFI_RW_DIR:-/rw_data/wifi}"
LOG="$RW_DIR/install.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${WIFI_SERVICE:-/etc/systemd/system/wifi-daemon.service}"
wifi_import=not-present

ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
WIDE="HomeScreen/QHMI_HomeScreenMain_Wide.qml"
ALL="HomeScreen/Screen_HomeScreen_AllMenus.qml"
SETUP="HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
CUSTOM="component/QCustomizedMenuListAllowEmpty.qml"
THEME="component/common/Q_ThemeImage.qml"

CAR_HASH='$6$ioniq5$/BDNqdHDOkg.wPr3W8PkQx0vbdQWmJBPkU9/.pwGNtL.v/IyXbR20nEEY/CYImn04cI8NlGDKK6A6kPA5noPZ.'

mkdir -p "$RW_DIR" 2>/dev/null
mkdir -p "$RW_DIR/backup" 2>/dev/null
mkdir -p "$RW_DIR/bin" 2>/dev/null
mkdir -p "$RW_DIR/ssh" 2>/dev/null
: > "$LOG" 2>/dev/null

log() {
    echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null
}

result() {
    {
        echo "WIFI_PAYLOAD"
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

# Always put / back read-only, even when a later step fails halfway through
# the install; otherwise a failed run leaves the rootfs writable.
cleanup_ro() {
    if [ "${transaction_started:-0}" = 1 ] && [ "${install_complete:-0}" != 1 ]; then
        log "rolling back incomplete install"
        systemctl stop wifi-daemon.service >> "$LOG" 2>&1 || true
        while IFS='|' read -r item existed destination; do
            [ -n "$destination" ] || continue
            if [ "$existed" = 1 ]; then
                if [ "$destination" = /app/bin/mdnsd ]; then
                    systemctl stop carplay_mdnsd.service >> "$LOG" 2>&1 || true
                    cp -f "$BACKUP_HU/transaction/$item" "$destination" 2>>"$LOG" || log "ROLLBACK FAILED: $destination"
                else
                    rm -f "$destination" 2>/dev/null || true
                    cp -pP "$BACKUP_HU/transaction/$item" "$destination" 2>/dev/null || log "ROLLBACK FAILED: $destination"
                fi
                for attr in SMACK64 SMACK64EXEC; do
                    value=$(cat "$BACKUP_HU/transaction/$item.$attr" 2>/dev/null || true)
                    [ -z "$value" ] || setfattr -n "security.$attr" -v "$value" "$destination" 2>/dev/null || true
                done
            else
                rm -f "$destination" 2>/dev/null || true
            fi
        done < "$BACKUP_HU/transaction/files"
        systemctl daemon-reload >> "$LOG" 2>&1 || true
        if [ "${mdnsd_changed:-0}" = 1 ] && [ "${mdnsd_was_active:-0}" = 1 ]; then
            systemctl restart carplay_mdnsd.service >> "$LOG" 2>&1 || true
        fi
        for unit in supplicantmanager.path supplicantmanager.service; do
            case "$unit" in *.path) prior=${old_oem_path:-unknown};; *) prior=${old_oem_service:-unknown};; esac
            case "$prior" in masked|masked-runtime) systemctl mask "$unit" >> "$LOG" 2>&1 || true;;
                unknown) :;; *) systemctl unmask "$unit" >> "$LOG" 2>&1 || true;; esac
        done
        [ "${daemon_was_enabled:-0}" = 1 ] || systemctl disable wifi-daemon.service >> "$LOG" 2>&1 || true
        [ "${daemon_was_active:-0}" != 1 ] || systemctl start wifi-daemon.service >> "$LOG" 2>&1 || true
    fi
    if [ "${was_ro:-0}" = 1 ]; then
        mount -o remount,ro / 2>/dev/null || true
    fi
}
trap cleanup_ro EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

remember() {
    destination=$1
    transaction_index=$((transaction_index + 1))
    existed=0
    if [ -e "$destination" ] || [ -L "$destination" ]; then
        cp -pP "$destination" "$BACKUP_HU/transaction/$transaction_index" || fail "transaction backup failed: $destination"
        existed=1
        for attr in SMACK64 SMACK64EXEC; do
            getfattr -n "security.$attr" --only-values "$destination" > "$BACKUP_HU/transaction/$transaction_index.$attr" 2>/dev/null || true
        done
    fi
    echo "$transaction_index|$existed|$destination" >> "$BACKUP_HU/transaction/files"
}

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
log "=== wifi payload start usb=$USB_DIR ==="

# The firmware ships md5sum only as a busybox applet; the coreutils binary
# is /usr/bin/md5sum.coreutils.  Resolve once, before any use.
MD5SUM_BIN=md5sum
command -v md5sum >/dev/null 2>&1 || MD5SUM_BIN=md5sum.coreutils

[ -d "$PAYLOAD" ] || fail "payload directory missing: $PAYLOAD"
[ -d "$BACKUP" ] || fail "backup directory missing: $BACKUP"
[ -f "$USB_DIR/manifest.md5" ] || fail "manifest.md5 missing"
[ -d "$QML_DIR" ] || fail "expected QML directory missing: $QML_DIR"

if ! ( cd "$USB_DIR" && "$MD5SUM_BIN" -c manifest.md5 ) > "$RW_DIR/manifest-check.log" 2>&1; then
    log "manifest check failed"
    fail "manifest check failed (see /rw_data/wifi/manifest-check.log)"
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

md5() { "$MD5SUM_BIN" "$1" 2>/dev/null | cut -d' ' -f1; }

ref_md5() {
    cat "$PAYLOAD/ref/$1.md5" 2>/dev/null | cut -d' ' -f1
}

adv_variant=wifi
has_hello=0
has_battery=0
has_battery_tile=0
has_wifi=0
grep -q 'HELLO_PAYLOAD_260128' "$cur_adv" 2>/dev/null && has_hello=1
grep -q 'BATTERY_PAYLOAD_260128' "$cur_adv" 2>/dev/null && has_battery=1
grep -q 'id: batteryTile1' "$cur_adv" 2>/dev/null && has_battery_tile=1
grep -q 'WIFI_PAYLOAD_260128' "$cur_adv" 2>/dev/null && has_wifi=1

if [ "$has_battery" = 1 ]; then
    if [ "$has_battery_tile" = 1 ]; then
        if [ "$has_hello" = 1 ]; then
            adv_variant=hello_battery_tile_wifi
        else
            adv_variant=battery_tile_wifi
        fi
    else
        if [ "$has_hello" = 1 ]; then
            adv_variant=hello_battery_wifi
        else
            adv_variant=battery_wifi
        fi
    fi
elif [ "$has_hello" = 1 ]; then
    adv_variant=hello_wifi
elif [ "$has_wifi" = 1 ]; then
    adv_variant=wifi
else
    cur_md5=$(md5 "$cur_adv")
    if [ "$cur_md5" = "$(ref_md5 stock_advanced)" ]; then
        adv_variant=wifi
    else
        fail "unknown Advanced QML (md5=$cur_md5)"
    fi
fi

wide_variant=wifi
if grep -q 'WIFI_PAYLOAD_260128' "$cur_wide" 2>/dev/null; then
    if grep -q 'HELLO_PAYLOAD_260128' "$cur_wide" 2>/dev/null; then
        wide_variant=hello_wifi
    fi
elif grep -q 'HELLO_PAYLOAD_260128' "$cur_wide" 2>/dev/null; then
    wide_variant=hello_wifi
else
    cur_md5=$(md5 "$cur_wide")
    if [ "$cur_md5" = "$(ref_md5 stock_wide)" ]; then
        wide_variant=wifi
    else
        fail "unknown Wide QML (md5=$cur_md5)"
    fi
fi

all_variant=wifi
if grep -q 'WIFI_PAYLOAD_260128' "$cur_all" 2>/dev/null; then
    if grep -q 'HELLO_PAYLOAD_260128' "$cur_all" 2>/dev/null; then
        all_variant=hello_wifi
    fi
elif grep -q 'HELLO_PAYLOAD_260128' "$cur_all" 2>/dev/null; then
    all_variant=hello_wifi
else
    cur_md5=$(md5 "$cur_all")
    if [ "$cur_md5" = "$(ref_md5 stock_allmenus)" ]; then
        all_variant=wifi
    else
        fail "unknown AllMenus QML (md5=$cur_md5)"
    fi
fi

setup_variant=wifi
if grep -q 'WIFI_PAYLOAD_260128' "$cur_setup" 2>/dev/null; then
    if grep -q 'HELLO_PAYLOAD_260128' "$cur_setup" 2>/dev/null; then
        setup_variant=hello_wifi
    fi
elif grep -q 'HELLO_PAYLOAD_260128' "$cur_setup" 2>/dev/null; then
    setup_variant=hello_wifi
else
    cur_md5=$(md5 "$cur_setup")
    if [ "$cur_md5" = "$(ref_md5 stock_setup)" ]; then
        setup_variant=wifi
    else
        fail "unknown Setup QML (md5=$cur_md5)"
    fi
fi

custom_variant=wifi
if grep -q 'WIFI_PAYLOAD_260128' "$cur_custom" 2>/dev/null; then
    if grep -q 'HELLO_PAYLOAD_260128' "$cur_custom" 2>/dev/null; then
        custom_variant=hello_wifi
    fi
elif grep -q 'HELLO_PAYLOAD_260128' "$cur_custom" 2>/dev/null; then
    custom_variant=hello_wifi
else
    cur_md5=$(md5 "$cur_custom")
    if [ "$cur_md5" = "$(ref_md5 stock_custom)" ]; then
        custom_variant=wifi
    else
        fail "unknown QCustomizedMenuListAllowEmpty QML (md5=$cur_md5)"
    fi
fi

log "variants: adv=$adv_variant wide=$wide_variant all=$all_variant setup=$setup_variant custom=$custom_variant"

for selected in \
    "HomeScreen/QHMI_HomeScreenMain_Advanced.$adv_variant.qml" \
    "HomeScreen/QHMI_HomeScreenMain_Wide.$wide_variant.qml" \
    "HomeScreen/Screen_HomeScreen_AllMenus.$all_variant.qml" \
    "HomeScreen/Screen_HomeScreen_Submenu_Setup.$setup_variant.qml" \
    "component/QCustomizedMenuListAllowEmpty.$custom_variant.qml" \
    HomeScreen/WifiPage.qml component/WifiTile.qml component/WifiSwitch.qml component/WifiButton.qml; do
    [ -f "$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$selected" ] || fail "selected QML missing: $selected"
done
[ -s "$PAYLOAD/build-id.txt" ] || fail "build-id.txt missing"
BUILD_ID=$(cat "$PAYLOAD/build-id.txt")

if [ "${WIFI_DRY_RUN:-0}" = "1" ]; then
    result "status=DRY_RUN" "variant=$adv_variant/$wide_variant/$all_variant/$setup_variant/$custom_variant"
    exit 0
fi

# ---- backup ------------------------------------------------------------
STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null)
[ -n "$STAMP" ] || STAMP=manual
BACKUP_HU="$RW_DIR/backup/$STAMP"
suffix=0
while ! mkdir -m 700 "$BACKUP_HU" 2>/dev/null; do
    [ -e "$BACKUP_HU" ] || [ -L "$BACKUP_HU" ] || fail "cannot create backup directory"
    suffix=$((suffix + 1))
    [ "$suffix" -le 100 ] || fail "too many backup timestamp collisions"
    BACKUP_HU="$RW_DIR/backup/$STAMP-$suffix"
done
mkdir -p "$BACKUP_HU/HomeScreen" "$BACKUP_HU/component/common" 2>/dev/null || fail "cannot create $BACKUP_HU"

for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM" "$THEME"; do
    if [ -f "$QML_DIR/$f" ]; then
        mkdir -p "$BACKUP_HU/$(dirname "$f")" 2>/dev/null
        cp -f "$QML_DIR/$f" "$BACKUP_HU/$f" || fail "backup failed: $f"
        mkdir -p "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/$(dirname "$f")" 2>/dev/null
        cp -f "$QML_DIR/$f" "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/$f" 2>/dev/null || true
    fi
done
# Keep the first pre-install tree as the restore point.
new_restore_point=0
grep -q 'WIFI_PAYLOAD_260128' "$cur_adv" 2>/dev/null || new_restore_point=1
log "HU backup: $BACKUP_HU"

# ---- rootfs rw ---------------------------------------------------------
was_ro=0
grep -q ' / .* ro[, ]' /proc/mounts && was_ro=1
if [ "$was_ro" = 1 ]; then
    mount -o remount,rw / 2>/dev/null || fail "cannot remount / rw"
fi

mkdir -p "$BACKUP_HU/transaction" "$RW_DIR/backup/accounts" || fail "cannot create transaction backup"
transaction_index=0
: > "$BACKUP_HU/transaction/files"
for destination in "$RW_DIR/wifi_daemon" "$RW_DIR/bin/dropbearmulti" "$SERVICE" "$RW_DIR/status.json" \
    "$RW_DIR/config.conf" "$RW_DIR/wifi.txt" "$RW_DIR/ssh/authorized_keys" \
    "$RW_DIR/home/.ssh/authorized_keys" /etc/passwd /etc/passwd.legacy /etc/passwd.secure /etc/shadow /etc/shadow.Global \
    /etc/systemd/system/carplay_mdnsd.service.d/override.conf; do
    remember "$destination"
    if [ -L "$destination" ]; then
        target=$(readlink -f "$destination" 2>/dev/null || true)
        [ -z "$target" ] || [ ! -f "$target" ] || remember "$target"
    fi
done
for f in passwd passwd.legacy passwd.secure shadow shadow.Global; do remember "$RW_DIR/backup/accounts/$f"; done
for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM" "$THEME" \
    HomeScreen/WifiPage.qml component/WifiTile.qml component/WifiSwitch.qml component/WifiButton.qml \
    component/HelloTile.qml component/BatteryTile.qml; do remember "$QML_DIR/$f"; done
daemon_was_active=0
systemctl is-active --quiet wifi-daemon.service 2>/dev/null && daemon_was_active=1
daemon_was_enabled=0
systemctl is-enabled --quiet wifi-daemon.service 2>/dev/null && daemon_was_enabled=1
if [ "$daemon_was_active" = 1 ]; then
    systemctl stop wifi-daemon.service >> "$LOG" 2>&1 || fail "cannot stop existing daemon safely"
fi
transaction_started=1

# ---- SSH user ----------------------------------------------------------
# /etc/passwd is a symlink to /tmp/passwd on this firmware.  esinit
# rewrites /tmp/passwd from /etc/passwd.legacy or /etc/passwd.secure on
# every boot, so the user must live in those templates too.
add_car_passwd() {
    f="$1"
    [ -f "$f" ] || return 0
    grep -q '^car:' "$f" 2>/dev/null && return 0
    [ -n "$(tail -c1 "$f" 2>/dev/null)" ] && echo >> "$f"
    entry='car:x:0:0:Car User:/rw_data/wifi/home:/bin/sh'
    echo "$entry" >> "$f" || fail "cannot add account: $f"
    printf '%s\n' "$entry" > "$RW_DIR/backup/accounts/$(basename "$f")" || fail "cannot record account ownership"
    chmod 600 "$RW_DIR/backup/accounts/$(basename "$f")" 2>/dev/null || true
    log "added user car to $f"
}
add_car_shadow() {
    f="$1"
    [ -f "$f" ] || return 0
    grep -q '^car:' "$f" 2>/dev/null && return 0
    [ -n "$(tail -c1 "$f" 2>/dev/null)" ] && echo >> "$f"
    entry="car:$CAR_HASH:19000:0:99999:7:::"
    echo "$entry" >> "$f" || fail "cannot add shadow: $f"
    printf '%s\n' "$entry" > "$RW_DIR/backup/accounts/$(basename "$f")" || fail "cannot record shadow ownership"
    chmod 600 "$RW_DIR/backup/accounts/$(basename "$f")" 2>/dev/null || true
    log "added car shadow to $f"
}

if [ ! -e /etc/passwd ] && [ -f /etc/passwd.legacy ]; then
    cp -f /etc/passwd.legacy /etc/passwd 2>/dev/null || true
    chmod 644 /etc/passwd 2>/dev/null || true
fi
if [ ! -e /etc/shadow ] && [ -f /etc/shadow.Global ]; then
    cp -f /etc/shadow.Global /etc/shadow 2>/dev/null || true
fi

add_car_passwd /etc/passwd
add_car_passwd /etc/passwd.legacy
add_car_passwd /etc/passwd.secure
add_car_shadow /etc/shadow.Global
add_car_shadow /etc/shadow

mkdir -p "$RW_DIR/home/.ssh" 2>/dev/null
chmod 700 "$RW_DIR/home" "$RW_DIR/home/.ssh" 2>/dev/null || true
chown 0:0 "$RW_DIR/home" "$RW_DIR/home/.ssh" 2>/dev/null || true
# /etc/shells is /tmp/shells; it is recreated at boot.  Add /bin/sh for
# this boot only; dropbear does not require it, but some login paths do.
if [ -f /tmp/shells ]; then
    grep -qx '/bin/sh' /tmp/shells 2>/dev/null || echo '/bin/sh' >> /tmp/shells
else
    printf '/bin/sh\n' > /tmp/shells 2>/dev/null || true
    chmod 644 /tmp/shells 2>/dev/null || true
fi

# ---- assets / icons ----------------------------------------------------
# WiFi assets plus the refreshed Hello/Battery tile icons.  The latter are
# installed only when their app is present; the directories are harmless.
for app in wifi hello battery; do
    src="$PAYLOAD/assets/$app"
    [ -d "$src" ] || continue
    dst="$QML_ROOT/$app"
    mkdir -p "$dst" 2>/dev/null || fail "cannot create $dst"
    for f in "$src/"*; do
        [ -f "$f" ] || continue
        remember "$dst/$(basename "$f")"
        cp -f "$f" "$dst/" || fail "asset copy failed: $f"
    done
    chmod 644 "$dst/"* 2>/dev/null || true
done

# ---- daemon / dropbear / service --------------------------------------
cp -f "$PAYLOAD/bin/wifi_daemon" "$RW_DIR/wifi_daemon" || fail "daemon copy failed"
chmod 755 "$RW_DIR/wifi_daemon" 2>/dev/null || true
chown 0:0 "$RW_DIR/wifi_daemon" 2>/dev/null || true
# Verify the requested label; do not claim it establishes effective runtime
# access. Vendor executables can carry their own exec labels. A failed label
# gate aborts and rolls back instead of silently reporting a successful install.
set_exec_label() {
    binary=$1
    if grep -q ' smackfs ' /proc/mounts 2>/dev/null; then
        command -v chsmack >/dev/null 2>&1 || fail "chsmack missing"
        chsmack -e _ "$binary" >> "$LOG" 2>&1 || fail "exec label failed: $binary"
        actual=$(getfattr -n security.SMACK64EXEC --only-values "$binary" 2>/dev/null || true)
        if [ -z "$actual" ]; then
            log "WARNING: cannot read back SMACK64EXEC for $binary; trusting chsmack"
        elif [ "$actual" != _ ]; then
            fail "exec label readback failed: $binary"
        fi
    fi
}
set_exec_label "$RW_DIR/wifi_daemon"

if [ -f "$PAYLOAD/bin/dropbearmulti" ]; then
    cp -f "$PAYLOAD/bin/dropbearmulti" "$RW_DIR/bin/dropbearmulti" || fail "dropbear copy failed"
    chmod 755 "$RW_DIR/bin/dropbearmulti" 2>/dev/null || true
    chown 0:0 "$RW_DIR/bin/dropbearmulti" 2>/dev/null || true
    set_exec_label "$RW_DIR/bin/dropbearmulti"
    ln -sf dropbearmulti "$RW_DIR/bin/dropbear" 2>/dev/null || true
    ln -sf dropbearmulti "$RW_DIR/bin/dropbearkey" 2>/dev/null || true
fi

cp -f "$PAYLOAD/systemd/wifi-daemon.service" "$SERVICE" || fail "service copy failed"
chmod 644 "$SERVICE" 2>/dev/null || true
chown 0:0 "$SERVICE" 2>/dev/null || true

# ---- carplay_mdnsd restart drop-in -------------------------------------
# The vendor unit is RemainAfterExit=true with no Restart: if mdnsd dies the
# unit still reports "active", so nothing ever restarts it (observed on-car:
# BonjourBrowser "kDNSServiceErr_ServiceNotRunning").  A drop-in makes the
# unit restart mdnsd when the process exits.  run_stuff_restore.sh removes it.
MDNSD_UNIT_DIR="/etc/systemd/system/carplay_mdnsd.service.d"
if [ -d /etc/systemd/system ] && command -v systemctl >/dev/null 2>&1; then
    if [ ! -f "$MDNSD_UNIT_DIR/override.conf" ] || \
       ! grep -q 'Restart=always' "$MDNSD_UNIT_DIR/override.conf" 2>/dev/null; then
        if mkdir -p "$MDNSD_UNIT_DIR" 2>/dev/null; then
            cat > "$MDNSD_UNIT_DIR/override.conf" <<'EOF'
[Service]
RemainAfterExit=no
Restart=always
RestartSec=2
EOF
            chmod 644 "$MDNSD_UNIT_DIR/override.conf" 2>/dev/null || true
            log "installed carplay_mdnsd restart drop-in"
        else
            log "WARNING: cannot create $MDNSD_UNIT_DIR (mdnsd will not auto-restart)"
        fi
    fi
fi

# ---- optional mdnsd "continue as root" patch ----------------------------
# mdnsd drops to user "nobody" (setuid in its main) and then fails to send
# mDNS multicast with EPERM on the car ("mDNSPlatformSendUDP got error 1"),
# which would keep the phone from ever discovering the CarPlay service.
# The binary already contains the fallback path ("continuing as root
# because user nobody does not exist"); NOPing the 5-byte setuid call at
# file offset 0x43c2 takes it.  The patch is applied by copying a
# pre-patched binary from the payload (md5-verified), NOT by byte-patching
# on the car: cp -f over the existing file keeps its inode and therefore
# the vendor SMACK64EXEC label (App::projection).
#
# Opt-in: create the marker file MDNSD_ROOT_ARM at the stick root.
# Guarded: the on-car binary must match the stock md5 (or already be the
# patched md5) and the payload copy must match the patched md5; anything
# else aborts without touching the vendor binary.  The stock binary is
# backed up and run_stuff_restore.sh restores it.
MDNSD_BIN=/app/bin/mdnsd
MDNSD_STOCK_MD5=f63a65c4c2b0c011ae8ed17557b45c1b
MDNSD_PATCHED_MD5=f847373d7c5a6906453ba921bb790d22
mdnsd_patch=not-attempted
md5_of() { "$MD5SUM_BIN" "$1" 2>/dev/null | cut -d' ' -f1; }
if [ -f "$USB_DIR/MDNSD_ROOT_ARM" ]; then
    if [ ! -f "$MDNSD_BIN" ]; then
        mdnsd_patch=missing-binary
        log "mdnsd patch: $MDNSD_BIN not found"
    elif [ ! -f "$PAYLOAD/bin/mdnsd.rooted" ]; then
        mdnsd_patch=payload-missing
        log "mdnsd patch: $PAYLOAD/bin/mdnsd.rooted missing"
    else
        _cur=$(md5_of "$MDNSD_BIN")
        if [ "$_cur" = "$MDNSD_PATCHED_MD5" ]; then
            mdnsd_patch=already-patched
            log "mdnsd patch: already applied"
        elif [ "$_cur" != "$MDNSD_STOCK_MD5" ]; then
            mdnsd_patch=md5-mismatch
            log "mdnsd patch: unexpected md5 for $MDNSD_BIN ($_cur); refusing"
        elif [ "$(md5_of "$PAYLOAD/bin/mdnsd.rooted")" != "$MDNSD_PATCHED_MD5" ]; then
            mdnsd_patch=payload-corrupt
            log "mdnsd patch: payload mdnsd.rooted md5 mismatch; refusing"
        elif [ -w / ] || touch / 2>/dev/null; then
            _bak="$RW_DIR/backup/mdnsd.stock"
            if cp -p "$MDNSD_BIN" "$_bak" 2>/dev/null; then
                remember "$MDNSD_BIN"
                mdnsd_changed=1
                mdnsd_was_active=0
                systemctl is-active --quiet carplay_mdnsd.service && mdnsd_was_active=1
                systemctl stop carplay_mdnsd.service >> "$LOG" 2>&1 || fail "cannot stop mdnsd for patch"
                if cp -f "$PAYLOAD/bin/mdnsd.rooted" "$MDNSD_BIN" 2>/dev/null; then
                    if [ "$(md5_of "$MDNSD_BIN")" = "$MDNSD_PATCHED_MD5" ]; then
                        mdnsd_patch=ok
                        log "mdnsd patch: applied (backup: $_bak)"
                        systemctl restart carplay_mdnsd.service >> "$LOG" 2>&1 || true
                    else
                        mdnsd_patch=verify-failed
                        cp -f "$_bak" "$MDNSD_BIN" 2>/dev/null || true
                        log "mdnsd patch: verification failed; stock restored"
                    fi
                else
                    mdnsd_patch=copy-failed
                    log "mdnsd patch: copy over $MDNSD_BIN failed"
                fi
            else
                mdnsd_patch=backup-failed
                log "mdnsd patch: cannot back up $MDNSD_BIN"
            fi
        else
            mdnsd_patch=not-writable
            log "mdnsd patch: / is not writable"
        fi
    fi
fi

# ---- default config / import ------------------------------------------
if [ ! -f "$RW_DIR/config.conf" ]; then
    RAND=$(dd if=/dev/urandom bs=16 count=1 2>/dev/null | "$MD5SUM_BIN" | cut -c1-16)
    cat > "$RW_DIR/config.conf" <<EOF
config_version=2
mode=off
wlan_on=0
ssid=
psk=
carplay_ssid=IONIQ5-CarPlay
carplay_psk=$RAND
carplay_band=5
carplay_channel=36
carplay_hidden=0
carplay_max_sta=2
carplay_manufacturer=Hyundai
carplay_model=Ioniq 5
carplay_name=Ioniq 5
ssh=0
ssh_port=2222
EOF
    chmod 600 "$RW_DIR/config.conf" 2>/dev/null || true
fi

if [ -f "$USB_DIR/authorized_keys" ]; then
    cp -f "$USB_DIR/authorized_keys" "$RW_DIR/ssh/authorized_keys" 2>/dev/null || true
    chmod 600 "$RW_DIR/ssh/authorized_keys" 2>/dev/null || true
    cp -f "$USB_DIR/authorized_keys" "$RW_DIR/home/.ssh/authorized_keys" 2>/dev/null || true
    chmod 600 "$RW_DIR/home/.ssh/authorized_keys" 2>/dev/null || true
    log "imported authorized_keys"
fi
# Import before the service starts.  Import is best-effort: the daemon may
# not be able to persist config in the installer context, but an existing
# /rw_data/wifi/config.conf is still usable and the UI can import again later.
if [ -f "$USB_DIR/wifi.txt" ]; then
    cp -f "$USB_DIR/wifi.txt" "$RW_DIR/wifi.txt" 2>/dev/null || true
    chmod 600 "$RW_DIR/wifi.txt" 2>/dev/null || true
    if [ -x "$RW_DIR/wifi_daemon" ]; then
        if "$RW_DIR/wifi_daemon" --import-file "$RW_DIR/wifi.txt" >> "$LOG" 2>&1; then
            wifi_import=OK
        else
            wifi_import=FAIL
            log "WARNING: wifi.txt import failed; keeping existing config (UI can import later)"
        fi
    else
        wifi_import=no-daemon
    fi
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
    if [ -n "$label" ]; then
        # onlycap=Privileged rejects security-xattr writes from the '_' USB
        # installer context (setfattr -> EPERM), which used to abort the whole
        # install and leave the previous WifiPage.qml in place.  A file created
        # by this process already carries the '_' floor label, and every reader
        # (including AppHomeScreen) may open floor-labelled files, so keep the
        # inherited label and continue with a warning instead of rolling back.
        if setfattr -n security.SMACK64 -v "$label" "$dst" 2>/dev/null; then
            actual=$(getfattr -n security.SMACK64 --only-values "$dst" 2>/dev/null || true)
            [ "$actual" = "$label" ] || fail "QML label readback failed: $dst"
        else
            actual=$(getfattr -n security.SMACK64 --only-values "$dst" 2>/dev/null || true)
            if [ "$actual" = "$label" ]; then
                # cp -f kept the destination inode, so the copy already
                # carries exactly the target label; nothing to set.
                :
            else
                case "$actual" in
                    ""|_) log "WARNING: cannot set SMACK64=$label on $dst (onlycap?); keeping inherited floor label" ;;
                    *) fail "QML label failed: $dst (unexpected inherited label '$actual')" ;;
                esac
            fi
        fi
    fi
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

for f in HomeScreen/WifiPage.qml component/WifiTile.qml component/WifiSwitch.qml component/WifiButton.qml component/HelloTile.qml component/BatteryTile.qml; do
    [ -f "$Q/$f" ] || continue
    install_qml "$Q/$f" "$QML_DIR/$f" "$cur_adv"
done

# Clear QML caches (including the Qt5 QML disk cache, which may hold
# compiled bytecode of the previous WifiPage).
rm -rf /root/.cache/qtshadercache 2>/dev/null || true
rm -rf /root/.cache/AppHomeScreen 2>/dev/null || true
rm -rf /root/.cache/qmlcache 2>/dev/null || true
rm -rf /tmp/qmlcache 2>/dev/null || true
rm -rf /tmp/qt-qml-cache 2>/dev/null || true
sync

sync

# ---- stop OEM WiFi race / start daemon --------------------------------
oem_mask=not-attempted
if command -v systemctl >/dev/null 2>&1; then
    systemctl stop supplicantmanager.path 2>/dev/null || true
    systemctl stop supplicantmanager.service 2>/dev/null || true
    # supplicantmanager is Restart=always behind a path watcher; stopping it
    # only makes it respawn and steal /var/run/wpa_supplicant/wlan0.  Mask
    # both units persistently.  run_stuff_restore.sh unmasks them again.
    oem_mask=ok
    for unit in supplicantmanager.path supplicantmanager.service; do
        state="$RW_DIR/backup/$unit.state"
        remember "$state"
        prior=$(systemctl is-enabled "$unit" 2>/dev/null || true)
        case "$unit" in *.path) old_oem_path=$prior;; *) old_oem_service=$prior;; esac
        if [ ! -f "$state" ]; then printf '%s\n' "$prior" > "$state"; fi
        systemctl mask "$unit" >> "$LOG" 2>&1 || oem_mask=partial
    done
    rm -f /tmp/no_unece 2>/dev/null || true
    systemctl daemon-reload >> "$LOG" 2>&1 || true
fi
[ "$oem_mask" = ok ] || fail "OEM manager masking failed: $oem_mask"

daemon_status="not-started"
rm -f "$RW_DIR/status.json" 2>/dev/null || fail "cannot clear old daemon status"
if command -v systemctl >/dev/null 2>&1; then
    if systemctl enable wifi-daemon.service >> "$LOG" 2>&1; then
        if systemctl restart wifi-daemon.service >> "$LOG" 2>&1; then
            daemon_status="systemd-ok"
        else
            daemon_status="systemd-restart-failed"
        fi
    else
        daemon_status="systemd-enable-failed"
    fi
fi
log "daemon=$daemon_status"
[ "$daemon_status" = systemd-ok ] || fail "daemon did not start through systemd: $daemon_status"

if [ "$was_ro" = 1 ]; then
    mount -o remount,ro / 2>/dev/null || true
fi

# ---- restart AppHomeScreen --------------------------------------------
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
    restart="$restart (killed AppHomeScreen; reboot if UI does not return)"
fi
# The new QML is only live once AppHomeScreen has actually restarted.  The
# daemon updates via systemd, but the UI process keeps the old QML in
# memory, so verify and tell the user when a reboot is required.
sleep 3
new_home_pid=$(pidof AppHomeScreen 2>/dev/null || true)
if [ -n "$new_home_pid" ] && [ "$new_home_pid" != "$old_home_pid" ]; then
    restart="$restart verified=1"
else
    restart="$restart verified=0 REBOOT_REQUIRED"
fi
log "restart=$restart"
sync

# A running PID alone is not proof of the deployed daemon version.
verified_build=0
for attempt in 1 2 3 4 5; do
    if systemctl is-active --quiet wifi-daemon.service 2>/dev/null && \
       grep -q "\"build_id\":\"$BUILD_ID\"" "$RW_DIR/status.json" 2>/dev/null; then verified_build=1; break; fi
    sleep 1
done
[ "$verified_build" = 1 ] || fail "running daemon build identity did not match payload"
main_pid=$(systemctl show -p MainPID wifi-daemon.service 2>/dev/null | sed -n 's/^MainPID=//p')
case "$main_pid" in ''|*[!0-9]*|0|1) fail "cannot verify daemon MainPID";; esac
[ "$(md5 "/proc/$main_pid/exe")" = "$(md5 "$PAYLOAD/bin/wifi_daemon")" ] || fail "running daemon bytes do not match payload"
if [ "$new_restore_point" = 1 ]; then ln -sfn "$BACKUP_HU" "$RW_DIR/backup/latest" || fail "cannot record restore point"; fi
install_complete=1
final_status=OK
case "$restart" in *verified=0*) final_status=REBOOT_REQUIRED;; esac

result "status=$final_status" "variant=$adv_variant/$wide_variant/$all_variant/$setup_variant/$custom_variant" \
       "backup=$BACKUP_HU" "daemon=$daemon_status" "oem_mask=$oem_mask" \
       "wifi_import=$wifi_import" "restart=$restart" "build_id=$BUILD_ID" \
       "mdnsd_patch=$mdnsd_patch" "log=$LOG"
log "=== wifi payload done ==="
exit 0
