#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
# WIFI1B_PAYLOAD_REV=1
# Wifi1b — static wpa_supplicant + own DHCP + SSH, standalone or beside WiFi.
#
# Works on the canonical Battery/Tweaks (no-WiFi) layout and beside a WiFi
# install.  It never reinstalls WiFi, never runs a migration stage, and never
# stops or removes a WiFi app it does not own.  It adds only:
#   Wifi1bPage / Wifi1bTile / Wifi1bSwitch / Wifi1bButton
#   /app/share/AppHomeScreen/wifi1b/*
#   /rw_data/wifi1b/*
#   /etc/systemd/system/wifi1b-daemon.service
#   user wifi1b (UID 0, key-only SSH)
#
# It backs up the shared home-screen QML under /rw_data/wifi1b/backup and
# rolls back on failure.  No firmware/updater/bootloader changes.
set -u
umask 077
PATH="${WIFI1B_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
BACKUP="$USB_DIR/backup"
QML_ROOT="${WIFI1B_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/wifi1b"
RW_DIR="${WIFI1B_RW_DIR:-/rw_data/wifi1b}"
RUNTIME_ROOT="${WIFI1B_RUNTIME_ROOT:-$(dirname "$RW_DIR")}"
RUN_DIR="${WIFI1B_RUN_DIR:-/run}"
HOME_PROC_ROOT="${WIFI1B_HOME_PROC_ROOT:-/proc}"
LOG="$RW_DIR/install.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${WIFI1B_SERVICE:-/etc/systemd/system/wifi1b-daemon.service}"
MDNS_DROPIN="${WIFI1B_MDNS_DROPIN:-/etc/systemd/system/carplay_mdnsd.service.d/60-wifi1b-foreground.conf}"
CONNMAN_DROPIN="${WIFI1B_CONNMAN_DROPIN:-/etc/systemd/system/connman.service.d/60-scourge-nodevice.conf}"
CONNMAN_UNIT="${WIFI1B_CONNMAN_UNIT:-/lib/systemd/system/connman.service}"
CONNMAN_UNIT_MD5=58f77230485566943fc8b7a38175034a
# V21 shipped the previous comment-only variant of the same interface list.
# Accept that exact known file for an over-the-top V22 install; anything else
# is still an unknown owner override and must not be overwritten.
CONNMAN_DROPIN_LEGACY_MD5=ee5432d1e0bda5a4d1bc4bff65d4f2ed
wifi1b_import=not-present

ADV="HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
WIDE="HomeScreen/QHMI_HomeScreenMain_Wide.qml"
ALL="HomeScreen/Screen_HomeScreen_AllMenus.qml"
SETUP="HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
CUSTOM="component/QCustomizedMenuListAllowEmpty.qml"

# Earliest report-path guard.  No helper is verified/loaded yet, so this is a
# minimal inline check: the USB root must be a real directory with real-dir
# ancestors, and the result leaf must be a single-link regular file or absent.
# If the report path is unsafe, fail to stderr only: never redirect, cp, log,
# beep or open a FIFO through it.
_report_dir_safe() {
    _rds=$1
    [ -d "$_rds" ] && [ ! -L "$_rds" ] || return 1
    _rdp=$(dirname "$_rds") || return 1
    while [ -n "$_rdp" ] && [ "$_rdp" != "/" ]; do
        [ ! -L "$_rdp" ] || return 1
        [ -d "$_rdp" ] || return 1
        _rdn=$(dirname "$_rdp") || return 1
        [ "$_rdn" != "$_rdp" ] || break
        _rdp=$_rdn
    done
    return 0
}
_report_file_safe() {
    _rfs=$1
    _rfp=$(dirname "$_rfs") || return 1
    while [ -n "$_rfp" ] && [ "$_rfp" != "/" ]; do
        [ ! -L "$_rfp" ] || return 1
        [ -d "$_rfp" ] || return 1
        _rfn=$(dirname "$_rfp") || return 1
        [ "$_rfn" != "$_rfp" ] || break
        _rfp=$_rfn
    done
    if [ -e "$_rfs" ] || [ -L "$_rfs" ]; then
        [ -f "$_rfs" ] || return 1
        [ ! -L "$_rfs" ] || return 1
        _rfl=$(stat -c %h "$_rfs" 2>/dev/null) || return 1
        case "$_rfl" in
            1) : ;;
            *) return 1 ;;
        esac
    fi
    return 0
}
if ! _report_dir_safe "$USB_DIR" || ! _report_file_safe "$OUT"; then
    printf 'WIFI1B_%s\nstatus=FAIL\nerror=unsafe_report_path\n' "PAYLOAD" >&2
    exit 1
fi

early_fail() {
    if _report_file_safe "$OUT"; then
        printf 'WIFI1B_PAYLOAD\nstatus=FAIL\nerror=%s\n' "$*" > "$OUT" 2>/dev/null ||
            printf 'WIFI1B_PAYLOAD\nstatus=FAIL\nerror=%s\n' "$*" >&2
    else
        printf 'WIFI1B_PAYLOAD\nstatus=FAIL\nerror=%s\n' "$*" >&2
    fi
    exit 1
}
if [ -L "$RW_DIR" ]; then
    early_fail "runtime directory is a symlink"
fi

log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }

result() {
    {
        echo "WIFI1B_PAYLOAD"
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

transaction_started=0
install_complete=0
owned_service_installed=0
service_touched=0
cleanup_ro() {
    if [ "${transaction_started:-0}" = 1 ] && [ "${install_complete:-0}" != 1 ]; then
        log "rolling back incomplete Wifi1b install"
        # Only stop the unit when this invocation installed our exact service
        # bytes.  A pre-existing/foreign unit is never stopped on an early
        # failure, and a unit we never started needs no stop.
        if [ "${owned_service_installed:-0}" = 1 ]; then
            systemctl stop wifi1b-daemon.service >> "$LOG" 2>&1 || true
        fi
        # Signal a manual daemon only after proving the PID still belongs to
        # our exact owned executable.  Reused/foreign/unreadable PIDs are
        # preserved and reported.
        if [ -n "${MANUAL_PID:-}" ] && [ -d "/proc/$MANUAL_PID" ]; then
            if [ "$(readlink "/proc/$MANUAL_PID/exe" 2>/dev/null || true)" = "$RW_DIR/wifi1b_daemon" ]; then
                kill -TERM "$MANUAL_PID" 2>/dev/null || log "ROLLBACK: cannot signal owned manual daemon PID $MANUAL_PID"
            else
                log "ROLLBACK: preserved PID $MANUAL_PID (executable is not the owned daemon)"
            fi
        fi
        while IFS='|' read -r item existed destination; do
            [ -n "$destination" ] || continue
            if [ "$existed" = 1 ]; then
                io_restore_entry "$BACKUP_HU/transaction/$item" "$destination" || log "ROLLBACK FAILED: $destination"
                for attr in SMACK64 SMACK64EXEC; do
                    value=$(cat "$BACKUP_HU/transaction/$item.$attr" 2>/dev/null || true)
                    [ -z "$value" ] || setfattr -n "security.$attr" -v "$value" "$destination" 2>/dev/null || true
                done
            else
                rm -f "$destination" 2>/dev/null || true
            fi
        done < "$BACKUP_HU/transaction/files"
        systemctl daemon-reload >> "$LOG" 2>&1 || true
        _svc_existed=0; _svc_enabled=unknown; _svc_active=0
        if [ -f "$BACKUP_HU/transaction/service.state" ]; then
            _svc_existed=$(sed -n 's/^existed=//p' "$BACKUP_HU/transaction/service.state" 2>/dev/null)
            _svc_enabled=$(sed -n 's/^enabled=//p' "$BACKUP_HU/transaction/service.state" 2>/dev/null)
            _svc_active=$(sed -n 's/^active=//p' "$BACKUP_HU/transaction/service.state" 2>/dev/null)
        fi
        if [ "$_svc_existed" = 1 ]; then
            if [ "${service_touched:-0}" = 1 ]; then
            case "$_svc_enabled" in
                enabled) systemctl enable wifi1b-daemon.service >> "$LOG" 2>&1 || true ;;
                masked) systemctl mask wifi1b-daemon.service >> "$LOG" 2>&1 || true ;;
                masked-runtime) systemctl mask --runtime wifi1b-daemon.service >> "$LOG" 2>&1 || true ;;
                disabled) systemctl disable wifi1b-daemon.service >> "$LOG" 2>&1 || true ;;
            esac
            if [ "$_svc_active" = 1 ]; then
                systemctl start wifi1b-daemon.service >> "$LOG" 2>&1 || true
            elif [ "${owned_service_installed:-0}" = 1 ]; then
                systemctl stop wifi1b-daemon.service >> "$LOG" 2>&1 || true
            fi
            fi
        else
            if [ "${owned_service_installed:-0}" = 1 ]; then
                systemctl disable wifi1b-daemon.service >> "$LOG" 2>&1 || true
                systemctl stop wifi1b-daemon.service >> "$LOG" 2>&1 || true
            fi
        fi
    fi
    [ -z "${PATCH_WORK:-}" ] || rm -rf "$PATCH_WORK"
    if [ "${IO_ACTIVE:-0}" = 1 ]; then io_restore_root || true; fi
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

MD5SUM_BIN=md5sum
command -v md5sum >/dev/null 2>&1 || MD5SUM_BIN=md5sum.coreutils
SHA256SUM_BIN=sha256sum
command -v sha256sum >/dev/null 2>&1 || SHA256SUM_BIN=sha256sum.coreutils
md5() { "$MD5SUM_BIN" "$1" 2>/dev/null | cut -d' ' -f1; }

[ -d "$PAYLOAD" ] || early_fail "payload directory missing: $PAYLOAD"
[ -d "$BACKUP" ] || early_fail "backup directory missing: $BACKUP"
[ -f "$USB_DIR/manifest.md5" ] || early_fail "manifest.md5 missing"
[ -d "$QML_DIR" ] || early_fail "expected QML directory missing: $QML_DIR"

# Verify the payload before sourcing any helper or making the first runtime
# write.  The manifest log is written later, after bounded cleanup.
if ! ( cd "$USB_DIR" && "$MD5SUM_BIN" -c manifest.md5 ) >/dev/null 2>&1; then
    early_fail "manifest check failed"
fi
. "$PAYLOAD/lib/install_io.sh"
[ -f "$PAYLOAD/lib/runtime_cleanup.sh" ] || early_fail "runtime_cleanup.sh missing from payload"
. "$PAYLOAD/lib/runtime_cleanup.sh"

# Refuse unknown symlink ancestors and wrong leaf types for every
# product/rootfs destination before the first runtime write.  Directories and
# file leaves have separate predicates: a directory where a file is expected
# is never silently turned into a nested write.
for _dir in "$QML_DIR" "$QML_DIR/HomeScreen" "$QML_DIR/component" \
             "$ASSET_DIR" "$(dirname "$SERVICE")" \
             "$(dirname "$MDNS_DROPIN")" "$(dirname "$CONNMAN_DROPIN")" \
             "$(dirname /etc/passwd)" "$(dirname /tmp/shells)" \
             "$RW_DIR" "$RW_DIR/backup" "$RW_DIR/bin" "$RW_DIR/ssh" \
             "$RW_DIR/home" "$RW_DIR/home/.ssh" "$RW_DIR/backup/accounts" \
             "$USB_DIR/backup_current" "$USB_DIR/qml_before" \
              "$USB_DIR/qml_after" "$RUN_DIR/wifi1b" "$RUN_DIR/wifi" \
              "$RUN_DIR/udev" "$RUN_DIR/udev/rules.d" \
             "$USB_DIR/backup_current/AppHomeScreen" \
             "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0" \
             "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/HomeScreen" \
             "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/component" \
             "$USB_DIR/qml_before/HomeScreen" "$USB_DIR/qml_before/component" \
             "$USB_DIR/qml_after/HomeScreen" "$USB_DIR/qml_after/component"; do
    runtime_cleanup_safe_dir_path "$_dir" ||
        early_fail "unsafe directory destination: $_dir"
done
for _file in "$QML_DIR/$ADV" "$QML_DIR/$WIDE" "$QML_DIR/$ALL" \
             "$QML_DIR/$SETUP" "$QML_DIR/$CUSTOM" \
             "$QML_DIR/HomeScreen/Wifi1bPage.qml" \
             "$QML_DIR/component/Wifi1bTile.qml" \
             "$QML_DIR/component/Wifi1bSwitch.qml" \
             "$QML_DIR/component/Wifi1bButton.qml" \
             "$MDNS_DROPIN" "$CONNMAN_DROPIN" \
             "$OUT" "$LOG" "$RW_DIR/result.txt"; do
    runtime_cleanup_safe_file_path "$_file" ||
        early_fail "unsafe file destination: $_file"
done
# Service is either absent, an exact owned regular file, or the verified
# product /dev/null mask; any other type/link is preserved and refused.
if [ -L "$SERVICE" ]; then
    [ "$(readlink "$SERVICE" 2>/dev/null || true)" = "/dev/null" ] ||
        early_fail "unknown service symlink destination: $SERVICE"
elif [ -e "$SERVICE" ]; then
    runtime_cleanup_safe_file_path "$SERVICE" ||
        early_fail "unsafe service destination: $SERVICE"
fi
for _asset in "$PAYLOAD/assets/wifi1b/"*; do
    [ -f "$_asset" ] || continue
    runtime_cleanup_safe_file_path "$ASSET_DIR/$(basename "$_asset")" ||
        early_fail "unsafe asset destination: $ASSET_DIR/$(basename "$_asset")"
done
for _leaf in "$RW_DIR/config.conf" "$RW_DIR/wifi.txt" "$RW_DIR/status.json" \
             "$RW_DIR/daemon.log" "$RW_DIR/wpa_supplicant.conf" \
             "$RW_DIR/wpa_supplicant.log" "$RW_DIR/wpa_supplicant.pid" \
             "$RW_DIR/dhcp.leases" "$RW_DIR/dhcpd.pid" "$RW_DIR/dhcpd.log" \
             "$RW_DIR/client.route" "$RW_DIR/client.owner" \
              "$RW_DIR/client.lineage" "$RW_DIR/scan.json" \
              "$RW_DIR/usb0.owner" \
             "$RW_DIR/bt.state" "$RW_DIR/ssh.bound" "$RW_DIR/home.json" \
             "$RW_DIR/apply.result" "$RW_DIR/ap.station-ip" \
             "$RW_DIR/sigterm-ready" "$RW_DIR/ssh.log" "$RW_DIR/daemon.lock" \
             "$RW_DIR/wifi1b_daemon" "$RW_DIR/bin/wifi1b_qmlpatch" \
              "$RW_DIR/bin/dropbearmulti" \
              "$RW_DIR/bin/opi-usb0-up.sh" \
              "$RUN_DIR/udev/rules.d/99-opi-usb0.rules" \
             "$RW_DIR/ssh/authorized_keys" \
             "$RW_DIR/ssh/dropbear.pid" \
             "$RW_DIR/ssh/dropbear_ed25519_host_key" \
             "$RW_DIR/ssh/dropbear_rsa_host_key" \
             "$RW_DIR/home/.ssh/authorized_keys" \
             "$RUN_DIR/wifi1b/active" "$RUN_DIR/wifi/active" \
             /tmp/shells; do
    runtime_cleanup_safe_file_path "$_leaf" ||
        early_fail "unsafe file destination: $_leaf"
done
# Atomic-write temporaries use the product's fixed "<path>.tmp.<pid>"
# convention.  Refuse any wrong-type pre-existing entry at those names before
# the daemon can open/truncate through it.
# dropbear/dropbearkey are product-owned symlinks to dropbearmulti.  Allow
# only that exact relative target; any other link/type is preserved and FAILs.
for _db in "$RW_DIR/bin/dropbear" "$RW_DIR/bin/dropbearkey"; do
    if [ -L "$_db" ]; then
        [ "$(readlink "$_db" 2>/dev/null || true)" = "dropbearmulti" ] ||
            early_fail "unexpected dropbear helper symlink: $_db"
    else
        runtime_cleanup_safe_file_path "$_db" ||
            early_fail "unsafe dropbear helper destination: $_db"
    fi
done
for _atomic in "$RW_DIR/config.conf" "$RW_DIR/wifi.txt" \
               "$RW_DIR/status.json" "$RW_DIR/wpa_supplicant.conf" \
               "$RW_DIR/wpa_supplicant.pid" "$RW_DIR/dhcp.leases" \
               "$RW_DIR/dhcpd.pid" "$RW_DIR/client.route" \
                "$RW_DIR/client.owner" "$RW_DIR/client.lineage" \
                "$RW_DIR/usb0.owner" \
               "$RW_DIR/scan.json" "$RW_DIR/bt.state" \
               "$RW_DIR/ssh.bound" "$RW_DIR/home.json" \
               "$RW_DIR/apply.result" "$RW_DIR/ap.station-ip" \
                "$RW_DIR/sigterm-ready" "$RW_DIR/ssh/authorized_keys" \
                "$RW_DIR/bin/opi-usb0-up.sh" \
                "$RUN_DIR/udev/rules.d/99-opi-usb0.rules" \
               "$RUN_DIR/wifi1b/active"; do
    for _tmp in "$_atomic".tmp.*; do
        [ -e "$_tmp" ] || [ -L "$_tmp" ] || continue
        runtime_cleanup_safe_file_path "$_tmp" ||
            early_fail "unsafe atomic temp destination: $_tmp"
    done
done
for _acct in passwd passwd.legacy passwd.secure shadow shadow.Global; do
    runtime_cleanup_safe_file_path "$RW_DIR/backup/accounts/$_acct" ||
        early_fail "unsafe account-marker destination: $RW_DIR/backup/accounts/$_acct"
done
# backup/latest is our own symlink; only an unknown real entry or one that
# escapes the backup directory is refused.
if [ -L "$RW_DIR/backup/latest" ]; then
    _latest_real=$(readlink -f "$RW_DIR/backup/latest" 2>/dev/null || true)
    case "$_latest_real" in
        "$RW_DIR"/backup/*) : ;;
        *) early_fail "backup/latest escapes the backup directory" ;;
    esac
elif [ -e "$RW_DIR/backup/latest" ]; then
    early_fail "backup/latest is not an owned symlink"
fi
for _qml in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM" \
            HomeScreen/BatteryTile.qml component/BatteryTile.qml \
            component/WifiTile.qml component/TweaksTile.qml \
            component/RebootTile.qml component/Wifi1bTile.qml; do
    runtime_cleanup_safe_file_path "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/$_qml" ||
        early_fail "unsafe backup_current destination: $_qml"
    runtime_cleanup_safe_file_path "$USB_DIR/qml_before/$_qml" ||
        early_fail "unsafe qml_before destination: $_qml"
    runtime_cleanup_safe_file_path "$USB_DIR/qml_after/$_qml" ||
        early_fail "unsafe qml_after destination: $_qml"
done
for _hashfile in "$USB_DIR/qml_before/hashes.md5" "$USB_DIR/qml_after/hashes.md5"; do
    runtime_cleanup_safe_file_path "$_hashfile" ||
        early_fail "unsafe qml snapshot hash destination: $_hashfile"
done
for _acct in /etc/passwd /etc/passwd.legacy /etc/passwd.secure \
             /etc/shadow /etc/shadow.Global; do
    if [ -L "$_acct" ]; then
        _acct_target=$(readlink -f "$_acct" 2>/dev/null || true)
        case "$_acct_target" in
            "${INSTALL_IO_ROOT:-/}"/tmp/*|/tmp/*|"${INSTALL_IO_ROOT:-/}"/etc/*|/etc/*) : ;;
            *) early_fail "account path has unapproved symlink target: $_acct" ;;
        esac
        runtime_cleanup_safe_file_path "$_acct_target" ||
            early_fail "account symlink target is not a regular file: $_acct"
    else
        runtime_cleanup_safe_file_path "$_acct" ||
            early_fail "unsafe account file destination: $_acct"
    fi
done

# FIRST runtime action: reclaim bounded logs before any mkdir/log/backup.
RUNTIME_CLEANUP_LOG=""
cleanup_bounded_logs "$RUNTIME_ROOT" || true
mkdir -p "$RW_DIR/backup" "$RW_DIR/bin" "$RW_DIR/ssh" 2>/dev/null ||
    early_fail "cannot create runtime directories"
: > "$LOG" 2>/dev/null
RUNTIME_CLEANUP_LOG="$LOG"
[ -z "$RUNTIME_CLEANUP_NOTES" ] || log "cleanup: $RUNTIME_CLEANUP_NOTES"
log "manifest OK"
log "=== wifi1b payload start usb=$USB_DIR ==="
io_init || fail "cannot determine initial root mount state"
io_capture_qml "$QML_DIR" "$USB_DIR/qml_before" || true

# ---- identify the installed WiFi QML ------------------------------------
cur_adv="$QML_DIR/$ADV"
cur_wide="$QML_DIR/$WIDE"
cur_all="$QML_DIR/$ALL"
cur_setup="$QML_DIR/$SETUP"
cur_custom="$QML_DIR/$CUSTOM"
# In a WiFi-removal bundle the migration stage rebuilds the shared QML from
# the canonical no-WiFi base, so this stage must not fail on unknown current
# bytes and must not patch them.  It still installs the Wifi1b components.
qml_deferred=0
[ "${WIFI1B_DEFER_QML:-0}" = "1" ] && qml_deferred=1
if [ "$qml_deferred" = 1 ]; then
    log "QML deferred to the migration stage; current shared QML is backed up there"
    layout=deferred
else
for f in "$cur_adv" "$cur_wide" "$cur_all" "$cur_setup" "$cur_custom"; do
    [ -f "$f" ] || fail "target missing: $f"
    grep -qx "$(md5 "$f")" "$PAYLOAD/accepted-qml.md5" || fail "unknown shared QML bytes: $f"
done
grep -q 'SCOURGE_LAYOUT_V2' "$cur_adv" || fail "canonical reactive layout required"
# The accepted-digest gate above already fail-closes unknown bytes.  The
# markers only detect which canonical layout is present: WiFi-based (all
# five files carry the WiFi marker) or standalone Tweaks-based (none do).
layout=standalone
if grep -q 'WIFI_PAYLOAD_260128' "$cur_adv" 2>/dev/null; then
    layout=wifi
    for f in "$cur_wide" "$cur_all" "$cur_setup" "$cur_custom"; do
        grep -q 'WIFI_PAYLOAD_260128' "$f" 2>/dev/null || fail "mixed WiFi markers; shared QML inconsistent"
    done
else
    for f in "$cur_wide" "$cur_all" "$cur_setup" "$cur_custom"; do
        grep -q 'WIFI_PAYLOAD_260128' "$f" 2>/dev/null && fail "mixed WiFi markers; shared QML inconsistent"
    done
    grep -q 'TWEAKS_PAYLOAD_260128' "$cur_adv" 2>/dev/null || \
        fail "no Battery/Tweaks base layout; install Tweaks first (standalone) or WiFi first"
fi
fi

has_tweaks=0
grep -q 'TWEAKS_PAYLOAD_260128' "$cur_adv" 2>/dev/null && has_tweaks=1
grep -q 'TWEAKS_PAYLOAD_260128' "$cur_wide" 2>/dev/null && has_tweaks=1
grep -q 'TWEAKS_PAYLOAD_260128' "$cur_custom" 2>/dev/null && has_tweaks=1

Q="$PAYLOAD/qml/AppHomeScreen/qml_eco3.0"
for f in "$Q/HomeScreen/Wifi1bPage.qml" "$Q/component/Wifi1bTile.qml" \
         "$Q/component/Wifi1bSwitch.qml" "$Q/component/Wifi1bButton.qml"; do
    [ -f "$f" ] || fail "Wifi1b component missing: $f"
done
[ -x "$PAYLOAD/bin/wifi1b_qmlpatch" ] || fail "wifi1b_qmlpatch missing"
[ -s "$PAYLOAD/build-id.txt" ] || fail "build-id.txt missing"
BUILD_ID=$(cat "$PAYLOAD/build-id.txt")
log "additive QML patch: tweaks=$has_tweaks layout=$layout"

if [ "${WIFI1B_DRY_RUN:-0}" = "1" ]; then
    result "status=DRY_RUN" "variant=additive" "layout=$layout" "tweaks=$has_tweaks"
    exit 0
fi

# ---- backup (retention, then snapshot) ----------------------------------
# The loader re-runs this stick on every power-on (/run is tmpfs, so its md5
# gate resets), and each run writes a full transaction snapshot (~3 MB).
# Unbounded, that history filled /rw_data (23 snapshots = 51 MB), so keep the
# restore point plus the two newest snapshots and prune the rest before
# allocating a new one.
prune_backups() {
    _b="$1"
    [ -d "$_b" ] || return 0
    # readlink -f prints the target of a dangling symlink, so require that it
    # resolves to an existing directory before treating it as the restore point.
    _lat=$(readlink -f "$_b/latest" 2>/dev/null)
    [ -n "$_lat" ] && [ -d "$_lat" ] || _lat=""
    _n=0
    _freed=0
    for _d in $(ls -d "$_b"/20* 2>/dev/null | sort -r); do
        [ -d "$_d" ] || continue
        case "$_d" in
            "$_b"/[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]T[0-9][0-9][0-9][0-9][0-9][0-9]Z*) ;;
            *) continue ;;
        esac
        _n=$((_n + 1))
        if [ "$_n" -le 2 ]; then continue; fi
        # _d keeps the caller's $RW_DIR spelling while _lat is canonical, so a
        # trailing slash or a symlinked parent must not hide the restore point.
        if [ -n "$_lat" ] && [ "$(readlink -f "$_d" 2>/dev/null)" = "$_lat" ]; then
            continue
        fi
        _kb=$(du -sk "$_d" 2>/dev/null | cut -f1)
        rm -rf "$_d" || fail "cannot prune $_d"
        _freed=$((_freed + ${_kb:-0}))
    done
    if [ -n "$_lat" ] && [ ! -d "$_lat" ]; then
        fail "restore point $_lat disappeared while pruning"
    fi
    [ "$_freed" -gt 0 ] && log "prune: freed ${_freed} KB (kept restore point + 2 snapshots)"
    return 0
}

STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null)
[ -n "$STAMP" ] || STAMP=manual
prune_backups "$RW_DIR/backup"
BACKUP_HU="$RW_DIR/backup/$STAMP"
suffix=0
while ! mkdir -m 700 "$BACKUP_HU" 2>/dev/null; do
    [ -e "$BACKUP_HU" ] || [ -L "$BACKUP_HU" ] || fail "cannot create backup directory"
    suffix=$((suffix + 1))
    [ "$suffix" -le 100 ] || fail "too many backup timestamp collisions"
    BACKUP_HU="$RW_DIR/backup/$STAMP-$suffix"
done
mkdir -p "$BACKUP_HU/HomeScreen" "$BACKUP_HU/component" 2>/dev/null || fail "cannot create $BACKUP_HU"

for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM"; do
    if [ -f "$QML_DIR/$f" ]; then
        mkdir -p "$BACKUP_HU/$(dirname "$f")" 2>/dev/null
        cp -f "$QML_DIR/$f" "$BACKUP_HU/$f" || fail "backup failed: $f"
        mkdir -p "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/$(dirname "$f")" 2>/dev/null
        cp -f "$QML_DIR/$f" "$USB_DIR/backup_current/AppHomeScreen/qml_eco3.0/$f" 2>/dev/null || true
    fi
done
new_restore_point=0
grep -q 'WIFI1B_PAYLOAD_260128' "$cur_adv" 2>/dev/null || new_restore_point=1
log "HU backup: $BACKUP_HU"

# ---- make / writable only for the service -------------------------------
io_settle_root || fail "root did not settle writable"

mkdir -p "$BACKUP_HU/transaction" "$RW_DIR/backup/accounts" || fail "cannot create transaction backup"
transaction_index=0
: > "$BACKUP_HU/transaction/files"
for destination in "$RW_DIR/wifi1b_daemon" "$RW_DIR/bin/wifi1b_qmlpatch" "$RW_DIR/bin/dropbearmulti" "$SERVICE" \
    "$MDNS_DROPIN" "$CONNMAN_DROPIN" \
    "$RW_DIR/status.json" "$RW_DIR/config.conf" "$RW_DIR/wifi.txt" \
    "$RW_DIR/ssh/authorized_keys" "$RW_DIR/home/.ssh/authorized_keys" \
    /etc/passwd /etc/passwd.legacy /etc/passwd.secure /etc/shadow /etc/shadow.Global; do
    remember "$destination"
    if [ -L "$destination" ]; then
        target=$(readlink -f "$destination" 2>/dev/null || true)
        [ -z "$target" ] || [ ! -f "$target" ] || remember "$target"
    fi
done
for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM" \
    HomeScreen/Wifi1bPage.qml component/Wifi1bTile.qml component/Wifi1bSwitch.qml component/Wifi1bButton.qml; do
    remember "$QML_DIR/$f"
done
for source in "$PAYLOAD/assets/wifi1b/"*; do
    [ -f "$source" ] || continue
    remember "$ASSET_DIR/$(basename "$source")"
done

daemon_was_active=0
systemctl is-active --quiet wifi1b-daemon.service 2>/dev/null && daemon_was_active=1
daemon_enabled_state=$(systemctl is-enabled wifi1b-daemon.service 2>/dev/null || true)
[ -n "$daemon_enabled_state" ] || daemon_enabled_state=unknown
service_existed=0
if [ -e "$SERVICE" ] || [ -L "$SERVICE" ]; then service_existed=1; fi
# Record the exact original service bytes (regular files only) so restore can
# verify the transaction source before replacing a /dev/null mask.
if [ "$service_existed" = 1 ] && [ -f "$SERVICE" ] && [ ! -L "$SERVICE" ]; then
    _svc_item=""
    while IFS='|' read -r _item _existed _destination; do
        [ "$_destination" = "$SERVICE" ] && _svc_item=$_item
    done < "$BACKUP_HU/transaction/files"
    [ -n "$_svc_item" ] || fail "cannot locate original service transaction entry"
    _svc_hash=$("$SHA256SUM_BIN" "$BACKUP_HU/transaction/$_svc_item" 2>/dev/null | cut -d' ' -f1)
    [ -n "$_svc_hash" ] || fail "cannot hash original service transaction entry"
    printf '%s\n' "$_svc_hash" > "$BACKUP_HU/transaction/service.sha256" || \
        fail "cannot record original service hash"
fi
# Persist the original service transaction: restore must put back a
# pre-existing file and its enablement/active state, not blindly rm it.
printf 'existed=%s\nenabled=%s\nactive=%s\n' \
    "$service_existed" "$daemon_enabled_state" "$daemon_was_active" \
    > "$BACKUP_HU/transaction/service.state" || fail "cannot record service transaction"
chmod 600 "$BACKUP_HU/transaction/service.state" 2>/dev/null || true
if [ "$daemon_was_active" = 1 ]; then
    service_touched=1
    systemctl stop wifi1b-daemon.service >> "$LOG" 2>&1 || fail "cannot stop Wifi1b daemon safely"
fi
transaction_started=1

# ---- reclaim log space (stale temps only) -------------------------------
# Bounded logs were already truncated before any mkdir/log/backup.  Stale
# atomic-write temps are removed only when the in-scope owned radios are
# verifiably stopped; WiFi temps stay out of scope unless WiFi is dead too.
runtime_cleanup_process_alive "$RUNTIME_ROOT" wifi1b wifi1b_daemon
_cleanup_w1b=$?
runtime_cleanup_process_alive "$RUNTIME_ROOT" wifi wifi_daemon
_cleanup_wifi=$?
if [ "$_cleanup_w1b" = 1 ]; then
    RUNTIME_CLEANUP_RADIOS_STOPPED=1
    if [ "$_cleanup_wifi" = 1 ]; then
        RUNTIME_CLEANUP_SCAN_WIFI=1
    else
        RUNTIME_CLEANUP_SCAN_WIFI=0
        runtime_cleanup_note "WiFi process not verifiably stopped; WiFi temps excluded"
    fi
fi
cleanup_stale_temps "$RUNTIME_ROOT" || true
[ -z "$RUNTIME_CLEANUP_NOTES" ] || log "cleanup: $RUNTIME_CLEANUP_NOTES"
df -h "$RUNTIME_ROOT" >> "$LOG" 2>&1 || true
du -xk "$RUNTIME_ROOT" 2>/dev/null | sort -n | tail -20 >> "$LOG" 2>&1 || true

# Removed-WiFi leftovers are never deleted by the direct payload: it carries
# no bundle archive/hash proof.  The helper preserves and reports them.
cleanup_removed_wifi "$RUNTIME_ROOT" "$QML_DIR" "${SCOURGE_STICK_DIR:-$USB_DIR}" \
    "" "" "${CLEANUP_WIFI_SERVICE:-/etc/systemd/system/wifi-daemon.service}" || true
[ -z "$RUNTIME_CLEANUP_NOTES" ] || log "cleanup: $RUNTIME_CLEANUP_NOTES"
df -h "$RUNTIME_ROOT" >> "$LOG" 2>&1 || true

# ---- SSH user -----------------------------------------------------------
add_wifi1b_passwd() {
    f="$1"
    [ -f "$f" ] || return 0
    grep -q '^wifi1b:' "$f" 2>/dev/null && return 0
    entry='wifi1b:x:0:0:WiFi2:/rw_data/wifi1b/home:/bin/sh'
    io_append_line "$f" "$entry" || fail "cannot add account: $f"
    printf '%s\n' "$entry" > "$RW_DIR/backup/accounts/$(basename "$f")" || fail "cannot record account ownership"
    chmod 600 "$RW_DIR/backup/accounts/$(basename "$f")" 2>/dev/null || true
    log "added user wifi1b to $f"
}
add_wifi1b_shadow() {
    f="$1"
    [ -f "$f" ] || return 0
    grep -q '^wifi1b:' "$f" 2>/dev/null && return 0
    entry="wifi1b:!:19000:0:99999:7:::"
    io_append_line "$f" "$entry" || fail "cannot add shadow: $f"
    printf '%s\n' "$entry" > "$RW_DIR/backup/accounts/$(basename "$f")" || fail "cannot record shadow ownership"
    chmod 600 "$RW_DIR/backup/accounts/$(basename "$f")" 2>/dev/null || true
    log "added wifi1b shadow to $f"
}

if [ ! -e /etc/passwd ] && [ -f /etc/passwd.legacy ]; then
    cp -f /etc/passwd.legacy /etc/passwd 2>/dev/null || true
fi
if [ ! -e /etc/shadow ] && [ -f /etc/shadow.Global ]; then
    cp -f /etc/shadow.Global /etc/shadow 2>/dev/null || true
fi
add_wifi1b_passwd /etc/passwd
add_wifi1b_passwd /etc/passwd.legacy
add_wifi1b_passwd /etc/passwd.secure
add_wifi1b_shadow /etc/shadow.Global
add_wifi1b_shadow /etc/shadow

io_mkdir "$RW_DIR/home/.ssh" || fail "cannot create SSH directory"
chmod 700 "$RW_DIR/home" "$RW_DIR/home/.ssh" 2>/dev/null || true
chown 0:0 "$RW_DIR/home" "$RW_DIR/home/.ssh" 2>/dev/null || true
sh_chsmack() { command -v chsmack >/dev/null 2>&1 && chsmack -a _ "$@" >> "$LOG" 2>&1 || true; }
sh_chsmack "$RW_DIR/home" "$RW_DIR/home/.ssh"
if [ -f /tmp/shells ]; then
    grep -qx '/bin/sh' /tmp/shells 2>/dev/null || echo '/bin/sh' >> /tmp/shells
else
    printf '/bin/sh\n' > /tmp/shells 2>/dev/null || true
fi

# ---- assets -------------------------------------------------------------
for f in "$PAYLOAD/assets/wifi1b/"*; do
    [ -f "$f" ] || continue
    io_asset "$f" "$ASSET_DIR/$(basename "$f")"
    chmod 644 "$ASSET_DIR/$(basename "$f")" 2>/dev/null || true
done

# ---- daemon / dropbear / service ---------------------------------------
io_copy "$PAYLOAD/bin/wifi1b_daemon" "$RW_DIR/wifi1b_daemon" || fail "daemon copy failed"
chmod 755 "$RW_DIR/wifi1b_daemon" 2>/dev/null || true
chown 0:0 "$RW_DIR/wifi1b_daemon" 2>/dev/null || true
# No SMACK label operations at all.  The 2026-11-10 on-car readback showed
# chsmack -e succeeded but the getfattr readback returned EPERM under
# onlycap=Privileged, and the resulting gate aborted the whole install.
# Binaries copied by this process carry the inherited floor label "_",
# which is the same label the proven public recipe runs with.
io_copy "$PAYLOAD/bin/wifi1b_qmlpatch" "$RW_DIR/bin/wifi1b_qmlpatch" || fail "qmlpatch copy failed"
chmod 755 "$RW_DIR/bin/wifi1b_qmlpatch" 2>/dev/null || true
chown 0:0 "$RW_DIR/bin/wifi1b_qmlpatch" 2>/dev/null || true

if [ -f "$PAYLOAD/bin/dropbearmulti" ]; then
    io_copy "$PAYLOAD/bin/dropbearmulti" "$RW_DIR/bin/dropbearmulti" || fail "dropbear copy failed"
    chmod 755 "$RW_DIR/bin/dropbearmulti" 2>/dev/null || true
    chown 0:0 "$RW_DIR/bin/dropbearmulti" 2>/dev/null || true
    ln -sf dropbearmulti "$RW_DIR/bin/dropbear" 2>/dev/null || true
    ln -sf dropbearmulti "$RW_DIR/bin/dropbearkey" 2>/dev/null || true
fi

if [ -L "$SERVICE" ] && [ "$(readlink "$SERVICE" 2>/dev/null || true)" != "/dev/null" ]; then
    fail "unknown wifi1b service symlink; refusing overwrite"
fi
if [ -L "$SERVICE" ] && [ "$(readlink "$SERVICE" 2>/dev/null || true)" = "/dev/null" ]; then
    rm -f "$SERVICE" 2>/dev/null || fail "cannot clear pre-existing masked service link"
fi
io_copy "$PAYLOAD/systemd/wifi1b-daemon.service" "$SERVICE" || fail "service copy failed"
if ! cmp -s "$PAYLOAD/systemd/wifi1b-daemon.service" "$SERVICE"; then
    fail "service copy verification failed"
fi
chmod 644 "$SERVICE" 2>/dev/null || true
chown 0:0 "$SERVICE" 2>/dev/null || true
owned_service_installed=1
service_touched=1

# The stock daemon forks; WiFi's old Restart=always override otherwise
# supervises the exiting parent. A later, owned drop-in runs it foreground.
[ ! -e "$MDNS_DROPIN" ] || cmp -s "$MDNS_DROPIN" "$PAYLOAD/systemd/60-wifi1b-foreground.conf" || fail "unknown mdns foreground override; refusing overwrite"
io_mkdir "$(dirname "$MDNS_DROPIN")" || fail "cannot create mdns override directory"
io_copy "$PAYLOAD/systemd/60-wifi1b-foreground.conf" "$MDNS_DROPIN" || fail "mdns foreground override copy failed"
chmod 644 "$MDNS_DROPIN" 2>/dev/null || true

# Keep ConnMan away from rmnet_usb0: that interface carries the primary
# BlueLink/nav data route, and ConnMan reconfiguring it breaks online route
# calculation.  The vendor unit is hash-gated; an unknown unit is left alone.
connman_dropin=skipped
if [ -f "$CONNMAN_UNIT" ] && [ "$(md5 "$CONNMAN_UNIT")" = "$CONNMAN_UNIT_MD5" ]; then
    if [ -e "$CONNMAN_DROPIN" ] && \
       ! cmp -s "$CONNMAN_DROPIN" "$PAYLOAD/systemd/60-connman-nodevice.conf"; then
        [ "$(md5 "$CONNMAN_DROPIN")" = "$CONNMAN_DROPIN_LEGACY_MD5" ] || \
            fail "unknown connman override; refusing overwrite"
        log "upgrading known V21 connman nodevice override"
    fi
    io_mkdir "$(dirname "$CONNMAN_DROPIN")" || fail "cannot create connman override directory"
    io_copy "$PAYLOAD/systemd/60-connman-nodevice.conf" "$CONNMAN_DROPIN" || fail "connman override copy failed"
    chmod 644 "$CONNMAN_DROPIN" 2>/dev/null || true
    connman_dropin=installed
    if command -v systemctl >/dev/null 2>&1; then
        systemctl daemon-reload >> "$LOG" 2>&1 || true
        # Apply the modem exclusion now if ConnMan is running; no-op otherwise.
        systemctl try-restart connman.service >> "$LOG" 2>&1 || true
    fi
    log "installed connman nodevice override (modem interfaces protected)"
else
    log "connman unit hash unknown or missing; modem nodevice override skipped"
fi

# ---- default config / import -------------------------------------------
if [ ! -f "$RW_DIR/config.conf" ]; then
    RAND=$(dd if=/dev/urandom bs=16 count=1 2>/dev/null | "$MD5SUM_BIN" | cut -c1-16)
    cat > "$RW_DIR/config.conf" <<EOF
config_version=2
mode=off
wlan_on=0
ssid=
psk=
carplay_ssid=IONIQ5-CarPlay-B
carplay_psk=$RAND
carplay_band=5
carplay_channel=36
carplay_hidden=0
carplay_max_sta=2
carplay_manufacturer=Hyundai
carplay_model=Ioniq 5
carplay_name=Ioniq 5
ssh=0
ssh_port=2224
EOF
    chmod 600 "$RW_DIR/config.conf" 2>/dev/null || true
fi

if [ -f "$USB_DIR/authorized_keys" ] && [ ! -L "$USB_DIR/authorized_keys" ] &&
   [ -r "$USB_DIR/authorized_keys" ]; then
    # Import only through verified copies.  Log success only after BOTH
    # product destinations match the stick bytes; any failure aborts the
    # install so rollback restores the previous keys and the stick/backup
    # stay untouched.
    io_copy "$USB_DIR/authorized_keys" "$RW_DIR/ssh/authorized_keys" &&
    cmp -s "$USB_DIR/authorized_keys" "$RW_DIR/ssh/authorized_keys" &&
    io_copy "$USB_DIR/authorized_keys" "$RW_DIR/home/.ssh/authorized_keys" &&
    cmp -s "$USB_DIR/authorized_keys" "$RW_DIR/home/.ssh/authorized_keys" ||
        fail "authorized_keys import failed; previous keys retained"
    chmod 600 "$RW_DIR/ssh/authorized_keys" 2>/dev/null || true
    chmod 600 "$RW_DIR/home/.ssh/authorized_keys" 2>/dev/null || true
    sh_chsmack "$RW_DIR/ssh" "$RW_DIR/ssh/authorized_keys" "$RW_DIR/home/.ssh" "$RW_DIR/home/.ssh/authorized_keys"
    log "imported authorized_keys (both destinations verified)"
fi

# Import is best-effort and must never activate WLAN, a mode, or SSH.
profile=""
[ -f "$USB_DIR/iphone_wifi.txt" ] && profile="$USB_DIR/iphone_wifi.txt"
[ -z "$profile" ] && [ -f "$USB_DIR/wifi.txt" ] && profile="$USB_DIR/wifi.txt"
if [ -n "$profile" ]; then
    cp -f "$profile" "$RW_DIR/wifi.txt" 2>/dev/null || true
    chmod 600 "$RW_DIR/wifi.txt" 2>/dev/null || true
    if [ -x "$RW_DIR/wifi1b_daemon" ]; then
        if "$RW_DIR/wifi1b_daemon" --import-file "$RW_DIR/wifi.txt" >> "$LOG" 2>&1; then
            wifi1b_import=OK
        else
            wifi1b_import=FAIL
            log "WARNING: profile import failed; keeping existing config (UI can import later)"
        fi
    else
        wifi1b_import=no-daemon
    fi
fi

# ---- QML ---------------------------------------------------------------
install_qml() {
    src="$1"
    dst="$2"
    [ -f "$src" ] || fail "payload QML missing: $src"
    io_mkdir "$(dirname "$dst")" || fail "cannot create dir for $dst"
    io_copy "$src" "$dst" || fail "copy failed: $dst"
    chmod 644 "$dst" 2>/dev/null || true
    chown 0:0 "$dst" 2>/dev/null || true
    # No label operations: files created here inherit the readable floor
    # label, and onlycap=Privileged makes setfattr fail from this context.
}

for f in HomeScreen/Wifi1bPage.qml component/Wifi1bTile.qml component/Wifi1bSwitch.qml component/Wifi1bButton.qml; do
    install_qml "$Q/$f" "$QML_DIR/$f" "$cur_adv"
done
QMLPATCH="$RW_DIR/bin/wifi1b_qmlpatch"
# Patch private copies, then install verified bytes without replacing live
# QML inodes/labels. No partially-written file is fed back into the patcher.
if [ "$qml_deferred" = 0 ]; then
PATCH_WORK=$(mktemp -d "$RW_DIR/.qml-patch.XXXXXX") || fail "cannot stage QML"
for entry in "advanced|$ADV" "wide|$WIDE" "menu|$ALL" "menu|$SETUP" "custom|$CUSTOM"; do
    kind=${entry%%|*}; relative=${entry#*|}
    temporary="$PATCH_WORK/$(basename "$relative")"
    cp -f "$QML_DIR/$relative" "$temporary" || fail "cannot stage $relative"
    "$QMLPATCH" "$kind" "$temporary" >> "$LOG" 2>&1 || fail "QML patch failed: $relative"
    io_copy "$temporary" "$QML_DIR/$relative" || fail "QML install failed: $relative"
done
for f in "$cur_adv" "$cur_wide" "$cur_all" "$cur_setup" "$cur_custom"; do
    grep -q 'WIFI1B_PAYLOAD_260128' "$f" || fail "QML patch verification failed: $f"
done
grep -q 'id: wifi1bPage' "$cur_adv" || fail "Wifi1b page missing after patch"
grep -q 'id: wifi1bTile1' "$cur_adv" || fail "Wifi1b Advanced tile missing after patch"
grep -q 'id: itemWifi1b' "$cur_wide" || fail "Wifi1b Wide tile missing after patch"
grep -q 'signal wifi1bClicked()' "$cur_custom" || fail "Wifi1b custom signal missing after patch"
fi

# Never recursively delete generic Qt cache directories: an ordinary
# pre-existing owner directory cannot be proven to belong to this product.
# Leaving them is the safe choice; QML file replacement still forces a parse
# of the new bytes by the UI process.
sync

# ---- start daemon idle --------------------------------------------------
rm -f "$RUN_DIR/wifi1b/active" 2>/dev/null || true
systemctl daemon-reload >> "$LOG" 2>&1 || true
daemon_status="not-started"
if command -v systemctl >/dev/null 2>&1; then
    if systemctl enable wifi1b-daemon.service >> "$LOG" 2>&1; then
        if systemctl restart wifi1b-daemon.service >> "$LOG" 2>&1; then
            daemon_status="systemd-ok"
        else
            daemon_status="systemd-restart-failed"
        fi
    else
        daemon_status="systemd-enable-failed"
    fi
fi
log "daemon=$daemon_status"
MANUAL_PID=""
if [ "$daemon_status" != systemd-ok ]; then
    # The 2026-11-10 car readback showed systemd unit starts can fail in the
    # installer context even though the binary itself is fine.  Start the
    # daemon directly so the install completes; the unit stays installed for
    # the next boot.  This mirrors the public recipe, which never uses
    # systemctl in its hot path.
    log "systemd start failed; attempting manual start"
    rm -f "$RW_DIR/status.json" 2>/dev/null || true
    SETSID_BIN=$(command -v setsid 2>/dev/null || true)
    $SETSID_BIN "$RW_DIR/wifi1b_daemon" --port 8093 --status-file "$RW_DIR/status.json" \
        >> "$RW_DIR/daemon.log" 2>&1 &
    # setsid may fork before exec, so $! is not reliable; find the real
    # daemon by its executable path, the same way the restore script does.
    MANUAL_PID=""
    for attempt in 1 2 3 4 5; do
        for p in /proc/[0-9]*; do
            [ "$(readlink "$p/exe" 2>/dev/null)" = "$RW_DIR/wifi1b_daemon" ] && { MANUAL_PID="${p#/proc/}"; break; }
        done
        [ -n "$MANUAL_PID" ] && break
        sleep 1
    done
    log "manual start pid=$MANUAL_PID"
    ok=0
    for attempt in 1 2 3 4 5 6 7 8; do
        if grep -q "\"build_id\":\"$BUILD_ID\"" "$RW_DIR/status.json" 2>/dev/null; then
            ok=1; break
        fi
        sleep 1
    done
    if [ "$ok" = 1 ]; then
        daemon_status="manual-ok"
    else
        daemon_status="manual-failed"
    fi
    log "daemon=$daemon_status"
fi
[ "$daemon_status" = systemd-ok ] || [ "$daemon_status" = manual-ok ] || \
    fail "daemon did not start (systemd and manual both failed)"

io_capture_qml "$QML_DIR" "$USB_DIR/qml_after" || true
io_restore_root || fail "cannot restore original root state"

# ---- restart AppHomeScreen ---------------------------------------------
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
    home_exe=$(readlink -f /app/bin/AppHomeScreen 2>/dev/null || true)
    killed_home=0
    preserved_home=0
    for p in "$HOME_PROC_ROOT"/[0-9]*; do
        [ -r "$p/comm" ] || continue
        [ "$(cat "$p/comm" 2>/dev/null)" = "AppHomeScreen" ] || continue
        exe=$(readlink -f "$p/exe" 2>/dev/null || true)
        if [ -n "$home_exe" ] && [ "$exe" = "$home_exe" ] &&
           [ "$(basename "$exe")" = "AppHomeScreen" ]; then
            kill -TERM "$(basename "$p")" 2>/dev/null && killed_home=1
        else
            preserved_home=1
        fi
    done
    if [ "$killed_home" = 1 ] && [ "$preserved_home" = 1 ]; then
        restart="$restart (killed owned AppHomeScreen; preserved non-owned process; reboot if UI does not return)"
    elif [ "$killed_home" = 1 ]; then
        restart="$restart (killed AppHomeScreen; reboot if UI does not return)"
    elif [ "$preserved_home" = 1 ]; then
        restart="$restart (preserved non-owned AppHomeScreen; reboot if UI does not return)"
    else
        restart="$restart (no AppHomeScreen process; reboot if UI does not return)"
    fi
fi
sleep 3
new_home_pid=$(pidof AppHomeScreen 2>/dev/null || true)
if [ -n "$new_home_pid" ] && [ "$new_home_pid" != "$old_home_pid" ]; then
    restart="$restart verified=1"
else
    restart="$restart verified=0 REBOOT_REQUIRED"
fi
log "restart=$restart"
sync

verified_build=0
for attempt in 1 2 3 4 5; do
    if grep -q "\"build_id\":\"$BUILD_ID\"" "$RW_DIR/status.json" 2>/dev/null; then
        if [ "$daemon_status" = systemd-ok ]; then
            systemctl is-active --quiet wifi1b-daemon.service 2>/dev/null && verified_build=1
        else
            [ -n "$MANUAL_PID" ] && [ -d "/proc/$MANUAL_PID" ] && verified_build=1
        fi
        [ "$verified_build" = 1 ] && break
    fi
    sleep 1
done
[ "$verified_build" = 1 ] || { log "status.json: $(head -c 400 "$RW_DIR/status.json" 2>/dev/null)"; fail "running Wifi1b build identity did not match payload"; }
if [ "$daemon_status" = systemd-ok ]; then
    main_pid=$(systemctl show -p MainPID wifi1b-daemon.service 2>/dev/null | sed -n 's/^MainPID=//p')
else
    main_pid=$MANUAL_PID
fi
case "$main_pid" in ''|*[!0-9]*|0|1) fail "cannot verify Wifi1b daemon MainPID";; esac
[ "$(md5 "/proc/$main_pid/exe")" = "$(md5 "$PAYLOAD/bin/wifi1b_daemon")" ] || fail "running Wifi1b daemon bytes do not match payload"
if [ "$new_restore_point" = 1 ]; then
    ln -sfn "$BACKUP_HU" "$RW_DIR/backup/latest" || fail "cannot record restore point"
fi
install_complete=1
final_status=OK
case "$restart" in *verified=0*) final_status=REBOOT_REQUIRED;; esac

result "status=$final_status" "variant=additive" "layout=$layout" "tweaks=$has_tweaks" \
       "backup=$BACKUP_HU" "daemon=$daemon_status" "wifi1b_import=$wifi1b_import" \
       "connman_dropin=$connman_dropin" \
       "restart=$restart" "build_id=$BUILD_ID" "log=$LOG"
log "=== wifi1b payload done ==="
exit 0
