#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
# WIFI1B_PAYLOAD_RESTORE=1
# Removes Wifi1b and restores the pre-Wifi1b shared QML, leaving WiFi intact.
set -u
umask 077
PATH="${WIFI1B_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
QML_ROOT="${WIFI1B_QML_ROOT:-/app/share/AppHomeScreen}"
QML_DIR="$QML_ROOT/qml_eco3.0"
ASSET_DIR="$QML_ROOT/wifi1b"
RW_DIR="${WIFI1B_RW_DIR:-/rw_data/wifi1b}"
RUNTIME_ROOT="${WIFI1B_RUNTIME_ROOT:-$(dirname "$RW_DIR")}"
RUN_DIR="${WIFI1B_RUN_DIR:-/run}"
LOG="$RW_DIR/restore.log"
OUT="$USB_DIR/run_stuff_result.txt"
SERVICE="${WIFI1B_SERVICE:-/etc/systemd/system/wifi1b-daemon.service}"
MDNS_DROPIN="${WIFI1B_MDNS_DROPIN:-/etc/systemd/system/carplay_mdnsd.service.d/60-wifi1b-foreground.conf}"
CONNMAN_DROPIN="${WIFI1B_CONNMAN_DROPIN:-/etc/systemd/system/connman.service.d/60-scourge-nodevice.conf}"

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
    printf 'WIFI1B_%s\nstatus=FAIL\nerror=unsafe_report_path\n' "RESTORE" >&2
    exit 1
fi

early_fail() {
    if _report_file_safe "$OUT"; then
        printf 'WIFI1B_RESTORE\nstatus=FAIL\nerror=%s\n' "$*" > "$OUT" 2>/dev/null ||
            printf 'WIFI1B_RESTORE\nstatus=FAIL\nerror=%s\n' "$*" >&2
    else
        printf 'WIFI1B_RESTORE\nstatus=FAIL\nerror=%s\n' "$*" >&2
    fi
    exit 1
}
if [ -L "$RW_DIR" ]; then
    early_fail "runtime directory is a symlink"
fi

log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }
result() {
    { echo "WIFI1B_RESTORE"; echo "time=$(date -u 2>/dev/null)"; echo "usb=$USB_DIR"; echo "$@"; } > "$OUT" 2>&1
    cp -f "$OUT" "$RW_DIR/restore-result.txt" 2>/dev/null
}
fail() { log "FAIL: $*"; result "status=FAIL" "error=$*"; exit 1; }
restore_errors=0
restore_failed() {
    log "FAILED: $*"
    restore_errors=$((restore_errors + 1))
}

was_ro=0
cleanup_ro() { if [ "${IO_ACTIVE:-0}" = 1 ]; then io_restore_root || true; fi; }
trap cleanup_ro EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
[ -f "$USB_DIR/manifest.md5" ] || early_fail "manifest.md5 missing"
[ -d "$PAYLOAD" ] || early_fail "payload directory missing: $PAYLOAD"
MD5SUM_BIN=md5sum
command -v md5sum >/dev/null 2>&1 || MD5SUM_BIN=md5sum.coreutils
SHA256SUM_BIN=sha256sum
command -v sha256sum >/dev/null 2>&1 || SHA256SUM_BIN=sha256sum.coreutils
if ! ( cd "$USB_DIR" && "$MD5SUM_BIN" -c manifest.md5 ) >/dev/null 2>&1; then
    early_fail "manifest check failed"
fi
. "$PAYLOAD/lib/install_io.sh"
[ -f "$PAYLOAD/lib/runtime_cleanup.sh" ] || early_fail "runtime_cleanup.sh missing from payload"
. "$PAYLOAD/lib/runtime_cleanup.sh"

# Refuse unknown symlink ancestors and wrong leaf types for every restore
# destination before the first runtime write.  Directories and file leaves
# have separate predicates so a directory cannot silently absorb a file write.
for _dir in "$QML_DIR" "$QML_DIR/HomeScreen" "$QML_DIR/component" \
             "$ASSET_DIR" "$(dirname "$SERVICE")" \
             "$(dirname "$MDNS_DROPIN")" "$(dirname "$CONNMAN_DROPIN")" \
             "$(dirname /etc/passwd)" "$(dirname /tmp/shells)" \
             "$RW_DIR" "$RW_DIR/backup" "$RW_DIR/bin" "$RW_DIR/ssh" \
             "$RW_DIR/home" "$RW_DIR/home/.ssh" "$RW_DIR/backup/accounts" \
              "$USB_DIR/qml_before" "$RUN_DIR/wifi1b" "$RUN_DIR/wifi" \
              "$RUN_DIR/udev" "$RUN_DIR/udev/rules.d" \
             "$USB_DIR/qml_before/HomeScreen" "$USB_DIR/qml_before/component" \
             /app/data/vbt /rw_data/app_data/vbt; do
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
             "$OUT" "$LOG" "$RW_DIR/restore-result.txt" \
             /app/data/vbt/VBTConfig.json \
             /rw_data/app_data/vbt/VBTConfig.json /tmp/shells; do
    runtime_cleanup_safe_file_path "$_file" ||
        early_fail "unsafe file destination: $_file"
done
for _asset in "$PAYLOAD/assets/wifi1b/"*; do
    [ -f "$_asset" ] || continue
    runtime_cleanup_safe_file_path "$ASSET_DIR/$(basename "$_asset")" ||
        early_fail "unsafe asset destination: $ASSET_DIR/$(basename "$_asset")"
done
for _qml in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM" \
            HomeScreen/BatteryTile.qml component/BatteryTile.qml \
            component/WifiTile.qml component/TweaksTile.qml \
            component/RebootTile.qml component/Wifi1bTile.qml; do
    runtime_cleanup_safe_file_path "$USB_DIR/qml_before/$_qml" ||
        early_fail "unsafe qml_before destination: $_qml"
done
runtime_cleanup_safe_file_path "$USB_DIR/qml_before/hashes.md5" ||
    early_fail "unsafe qml snapshot hash destination"
for _leaf in "$RW_DIR/wifi1b_daemon" "$RW_DIR/status.json" "$RW_DIR/config.conf" \
             "$RW_DIR/wifi.txt" "$RW_DIR/daemon.log" \
             "$RW_DIR/wpa_supplicant.conf" "$RW_DIR/wpa_supplicant.log" \
             "$RW_DIR/wpa_supplicant.pid" "$RW_DIR/dhcp.leases" \
             "$RW_DIR/dhcpd.pid" "$RW_DIR/dhcpd.log" \
             "$RW_DIR/client.route" "$RW_DIR/client.owner" \
              "$RW_DIR/client.lineage" "$RW_DIR/scan.json" \
              "$RW_DIR/usb0.owner" \
             "$RW_DIR/bt.state" "$RW_DIR/ssh.bound" "$RW_DIR/home.json" \
             "$RW_DIR/apply.result" "$RW_DIR/ap.station-ip" \
             "$RW_DIR/sigterm-ready" "$RW_DIR/ssh.log" "$RW_DIR/daemon.lock" \
             "$RW_DIR/bin/wifi1b_qmlpatch" \
              "$RW_DIR/bin/dropbearmulti" \
              "$RW_DIR/bin/opi-usb0-up.sh" \
              "$RUN_DIR/udev/rules.d/99-opi-usb0.rules" \
             "$RW_DIR/ssh/authorized_keys" \
             "$RW_DIR/ssh/dropbear.pid" \
             "$RW_DIR/ssh/dropbear_ed25519_host_key" \
             "$RW_DIR/ssh/dropbear_rsa_host_key" \
             "$RW_DIR/home/.ssh/authorized_keys" \
             "$RUN_DIR/wifi1b/active" "$RUN_DIR/wifi/active"; do
    runtime_cleanup_safe_file_path "$_leaf" ||
        early_fail "unsafe file destination: $_leaf"
done
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
if [ -L "$RW_DIR/backup/latest" ]; then
    _latest_real=$(readlink -f "$RW_DIR/backup/latest" 2>/dev/null || true)
    case "$_latest_real" in
        "$RW_DIR"/backup/*) : ;;
        *) early_fail "backup/latest escapes the backup directory" ;;
    esac
elif [ -e "$RW_DIR/backup/latest" ]; then
    early_fail "backup/latest is not an owned symlink"
fi

RUNTIME_CLEANUP_LOG=""

# Read-only foreign-ownership gate BEFORE bounded owned-log truncation.  A
# foreign-modified unit must retain product logs as well as runtime/keys.
LATEST="$RW_DIR/backup/latest"
[ -d "$LATEST" ] || early_fail "no backup/latest; cannot restore"
[ ! -L "$RW_DIR/backup" ] || early_fail "backup directory is a symlink; refusing restore"
LATEST_REAL=$(readlink -f "$LATEST" 2>/dev/null || true)
case "$LATEST_REAL" in
    "$RW_DIR"/backup/*) : ;;
    *) early_fail "restore point escapes the Wifi1b backup directory" ;;
esac
if { [ -e "$SERVICE" ] || [ -L "$SERVICE" ]; } &&
   ! { [ -f "$SERVICE" ] && [ ! -L "$SERVICE" ] &&
       cmp -s "$SERVICE" "$PAYLOAD/systemd/wifi1b-daemon.service"; } &&
   ! { [ -L "$SERVICE" ] &&
       [ "$(readlink "$SERVICE" 2>/dev/null || true)" = "/dev/null" ]; }; then
    restore_failed "foreign-modified wifi1b service; restore refused and logs preserved"
    result "status=FAIL" "error=foreign_service" "backup=$LATEST" \
           "cleanup_errors=$restore_errors" "log=$LOG"
    exit 1
fi

cleanup_bounded_logs "$RUNTIME_ROOT" || true
mkdir -p "$RW_DIR" 2>/dev/null || early_fail "cannot create runtime directory"
: > "$LOG" 2>/dev/null
RUNTIME_CLEANUP_LOG="$LOG"
[ -z "$RUNTIME_CLEANUP_NOTES" ] || log "cleanup: $RUNTIME_CLEANUP_NOTES"
log "manifest OK"

LATEST="$RW_DIR/backup/latest"
[ -d "$LATEST" ] || fail "no backup/latest; cannot restore"
[ ! -L "$RW_DIR/backup" ] || fail "backup directory is a symlink; refusing restore"
LATEST_REAL=$(readlink -f "$LATEST" 2>/dev/null || true)
case "$LATEST_REAL" in
    "$RW_DIR"/backup/*) : ;;
    *) fail "restore point escapes the Wifi1b backup directory" ;;
esac

. "$PAYLOAD/lib/install_io.sh"
io_init || fail "cannot determine root mount state"
io_capture_qml "$QML_DIR" "$USB_DIR/qml_before" || true
io_settle_root || fail "root did not settle writable"

# Read the original service transaction.  A pre-existing file and its
# enablement/active state are restored; a foreign-modified service is never
# removed or stopped.  Missing/legacy ledger entries preserve the file.
SERVICE_ITEM=""
SERVICE_EXISTED=""
SERVICE_ENABLED=""
SERVICE_ACTIVE=""
if [ -f "$LATEST/transaction/files" ]; then
    while IFS='|' read -r _item _existed _destination; do
        [ "$_destination" = "$SERVICE" ] || continue
        SERVICE_ITEM=$_item
        SERVICE_EXISTED=$_existed
    done < "$LATEST/transaction/files"
fi
if [ -f "$LATEST/transaction/service.state" ]; then
    SERVICE_ENABLED=$(sed -n 's/^enabled=//p' "$LATEST/transaction/service.state" 2>/dev/null)
    SERVICE_ACTIVE=$(sed -n 's/^active=//p' "$LATEST/transaction/service.state" 2>/dev/null)
fi
[ -n "$SERVICE_ENABLED" ] || SERVICE_ENABLED=unknown
[ -n "$SERVICE_ACTIVE" ] || SERVICE_ACTIVE=0
service_is_owned=0
if [ -f "$SERVICE" ] && [ ! -L "$SERVICE" ] &&
   cmp -s "$SERVICE" "$PAYLOAD/systemd/wifi1b-daemon.service"; then
    service_is_owned=1
fi
service_is_masked=0
if [ -L "$SERVICE" ] && [ "$(readlink "$SERVICE" 2>/dev/null || true)" = "/dev/null" ]; then
    service_is_masked=1
fi
service_restore_error=0
# A service file changed after install is not ours to stop, replace or leave
# half-broken.  Refuse before any destructive restore step and preserve all
# runtime, credentials and backups for owner resolution.
if { [ -e "$SERVICE" ] || [ -L "$SERVICE" ]; } &&
   [ "$service_is_owned" = 0 ] && [ "$service_is_masked" = 0 ]; then
    restore_failed "foreign-modified wifi1b service; restore refused and runtime preserved"
    result "status=FAIL" "error=foreign_service" "backup=$LATEST" \
           "cleanup_errors=$restore_errors" "log=$LOG"
    exit 1
fi
service_may_stop=0
if [ "$service_is_owned" = 1 ]; then
    service_may_stop=1
elif [ ! -e "$SERVICE" ] && [ ! -L "$SERVICE" ]; then
    # No service file, but a stale unit process may still need stopping.
    service_may_stop=1
fi

# Stop Wifi1b and release wlan0.  Never stop/disable a foreign-modified
# service: only exact owned bytes (or an absent service) are touched.
if [ "$service_may_stop" = 1 ] && command -v systemctl >/dev/null 2>&1; then
    systemctl stop wifi1b-daemon.service 2>/dev/null || {
        restore_failed "cannot stop owned wifi1b service"; service_restore_error=1; }
    if [ "$SERVICE_EXISTED" = 0 ] && [ "$service_is_owned" = 1 ]; then
        systemctl disable wifi1b-daemon.service 2>/dev/null || {
            restore_failed "cannot disable owned wifi1b service"; service_restore_error=1; }
    fi
else
    log "kept independently changed/foreign wifi1b service; not stopping or disabling it"
fi
if [ "$service_may_stop" = 1 ]; then
    [ "$restore_errors" -eq 0 ] && [ "$service_restore_error" -eq 0 ] ||
        fail "restore prerequisites failed; preserve USB ownership proof and runtime"
    if [ -x "$RW_DIR/wifi1b_daemon" ]; then
        "$RW_DIR/wifi1b_daemon" --mode off >> "$LOG" 2>&1 || fail "Wifi1b network cleanup failed; retain daemon for recovery"
    fi
    rm -f "$RUN_DIR/wifi1b/active" 2>/dev/null || restore_failed "cannot remove $RUN_DIR/wifi1b/active"
    owned_wifi1b_pids() {
        for p in /proc/[0-9]*; do
            [ -r "$p/stat" ] || continue
            executable=$(readlink "$p/exe" 2>/dev/null || true)
            case "$executable" in
                "$RW_DIR/wifi1b_daemon"|"$RW_DIR/wifi1b_daemon (deleted)"|\
                "$RW_DIR/bin/dropbearmulti"|"$RW_DIR/bin/dropbearmulti (deleted)")
                    return 0 ;;
            esac
        done
        return 1
    }
    for p in /proc/[0-9]*; do
        [ -r "$p/stat" ] || continue
        executable=$(readlink "$p/exe" 2>/dev/null || true)
        case "$executable" in
            "$RW_DIR/wifi1b_daemon"|"$RW_DIR/wifi1b_daemon (deleted)"|\
            "$RW_DIR/bin/dropbearmulti"|"$RW_DIR/bin/dropbearmulti (deleted)")
                kill -TERM "${p#/proc/}" 2>/dev/null || true ;;
        esac
    done
    for attempt in 1 2 3; do
        owned_wifi1b_pids || break
        sleep 1
    done
    if owned_wifi1b_pids; then
        fail "owned Wifi1b process survived SIGTERM; retain runtime for recovery"
    fi
    # Use this verified payload's cleanup implementation: an older installed
    # daemon may not understand the new option and would start its normal loop.
    # Retain runtime/backup if exact USB ownership or absence is not confirmed.
    [ "$restore_errors" -eq 0 ] && [ "$service_restore_error" -eq 0 ] ||
        fail "restore prerequisites failed; preserve USB ownership proof and runtime"
    runtime_cleanup_safe_regular_path "$PAYLOAD/bin/wifi1b_daemon" &&
        [ -x "$PAYLOAD/bin/wifi1b_daemon" ] ||
        fail "usb0 cleanup unavailable; retain backup/runtime for recovery"
    "$PAYLOAD/bin/wifi1b_daemon" --usb0-cleanup >> "$LOG" 2>&1 ||
        fail "usb0 cleanup failed; retain backup/runtime for recovery"
else
    log "kept foreign-modified service process; not stopping or cleaning its network state"
fi
for family in iptables ip6tables; do
    IPT=$(command -v "$family" 2>/dev/null || true)
    [ -n "$IPT" ] || continue
    for chain in WIFI1B_APP_SSH WIFI1B_APP_CP WIFI1B_APP_AP; do
        "$IPT" -S "$chain" >/dev/null 2>&1 || continue
        for attempt in 1 2 3 4; do "$IPT" -D INPUT -i wlan0 -j "$chain" 2>/dev/null || break; done
        for attempt in 1 2 3 4; do "$IPT" -D OUTPUT -o wlan0 -j "$chain" 2>/dev/null || break; done
        "$IPT" -F "$chain" 2>/dev/null || restore_failed "cannot flush owned $family chain: $chain"
        "$IPT" -X "$chain" 2>/dev/null || restore_failed "cannot delete owned $family chain: $chain"
    done
done

# Restore the shared QML exactly as it was before Wifi1b was installed.
# The backup still contains the WiFi page/tile and the WiFi installer's
# fixes, because Wifi1b only ran after WiFi.
current_adv="$QML_DIR/$ADV"
if ! grep -q 'WIFI1B_PAYLOAD_260128' "$current_adv" 2>/dev/null && [ "${WIFI1B_FORCE_RESTORE:-0}" != "1" ]; then
    fail "current Advanced QML has no Wifi1b marker; refusing to overwrite unknown changes"
fi
for f in "$ADV" "$WIDE" "$ALL" "$SETUP" "$CUSTOM"; do
    if [ -f "$LATEST/$f" ]; then
        label=$(getfattr -n security.SMACK64 --only-values "$QML_DIR/$f" 2>/dev/null || true)
        mkdir -p "$QML_DIR/$(dirname "$f")" 2>/dev/null
        io_copy "$LATEST/$f" "$QML_DIR/$f" || fail "restore failed: $f"
        chmod 644 "$QML_DIR/$f" 2>/dev/null || restore_failed "cannot set QML mode: $f"
        chown 0:0 "$QML_DIR/$f" 2>/dev/null || restore_failed "cannot set QML owner: $f"
        if [ -n "$label" ]; then
            if setfattr -n security.SMACK64 -v "$label" "$QML_DIR/$f" 2>/dev/null; then
                actual=$(getfattr -n security.SMACK64 --only-values "$QML_DIR/$f" 2>/dev/null || true)
                [ "$actual" = "$label" ] || restore_failed "SMACK64 readback mismatch on $f"
            else
                # Optional by design: the inherited floor label is the proven
                # operating label and onlycap=Privileged may deny relabeling.
                log "NOTE: SMACK64 restore unsupported for $f; inherited floor label retained"
            fi
        fi
        log "restored $f"
    fi
done

# Remove only Wifi1b's own QML components and assets.
for f in HomeScreen/Wifi1bPage.qml component/Wifi1bTile.qml component/Wifi1bSwitch.qml component/Wifi1bButton.qml; do
    if [ -f "$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$f" ] && cmp -s "$PAYLOAD/qml/AppHomeScreen/qml_eco3.0/$f" "$QML_DIR/$f"; then
        rm -f "$QML_DIR/$f" 2>/dev/null || restore_failed "cannot remove Wifi1b QML component: $f"
    else
        log "kept changed/unknown component: $f"
    fi
done
for source in "$PAYLOAD/assets/wifi1b/"*; do
    [ -f "$source" ] || continue
    target="$ASSET_DIR/$(basename "$source")"
    if cmp -s "$source" "$target"; then
        rm -f "$target" 2>/dev/null || restore_failed "cannot remove Wifi1b asset: $target"
    fi
done
rmdir "$ASSET_DIR" 2>/dev/null || true

# Restore the service file and its original enabled/active state from the
# install transaction.  Never remove or stop an owner-modified/foreign file.
restore_service() {
    if [ "$SERVICE_EXISTED" = 1 ]; then
        if [ -n "$SERVICE_ITEM" ] &&
           { [ ! -e "$SERVICE" ] && [ ! -L "$SERVICE" ] ||
             [ "$service_is_owned" = 1 ] || [ "$service_is_masked" = 1 ]; }; then
            if [ "$service_is_masked" = 1 ] &&
               [ ! -L "$LATEST/transaction/$SERVICE_ITEM" ]; then
                _svc_src="$LATEST/transaction/$SERVICE_ITEM"
                if [ ! -f "$_svc_src" ] || [ -L "$_svc_src" ] ||
                   [ ! -r "$_svc_src" ] || [ ! -s "$_svc_src" ]; then
                    restore_failed "masked service source is not a readable regular file"
                    return 0
                fi
                _svc_hash_file="$LATEST/transaction/service.sha256"
                _svc_hash=""
                [ -f "$_svc_hash_file" ] && [ ! -L "$_svc_hash_file" ] &&
                    _svc_hash=$(cat "$_svc_hash_file" 2>/dev/null)
                case "$_svc_hash" in
                    *[!0-9a-f]*|'') restore_failed "masked service source has no verified hash"; return 0 ;;
                esac
                [ "${#_svc_hash}" -eq 64 ] || {
                    restore_failed "masked service source hash is malformed"; return 0; }
                _svc_actual=$("$SHA256SUM_BIN" "$_svc_src" 2>/dev/null | cut -d' ' -f1)
                [ "$_svc_actual" = "$_svc_hash" ] || {
                    restore_failed "masked service source hash mismatch"; return 0; }
                rm -f "$SERVICE" 2>/dev/null || {
                    restore_failed "cannot remove known /dev/null service mask"
                    return 0; }
            fi
            io_restore_entry "$LATEST/transaction/$SERVICE_ITEM" "$SERVICE" || \
                { log "FAILED to restore original wifi1b service"; service_restore_error=1; return 0; }
            log "restored pre-existing wifi1b service from transaction"
            if command -v systemctl >/dev/null 2>&1; then
                systemctl daemon-reload >> "$LOG" 2>&1 || true
                case "$SERVICE_ENABLED" in
                    enabled) systemctl enable wifi1b-daemon.service >> "$LOG" 2>&1 || {
                                 log "WARNING: could not re-enable pre-existing wifi1b service"
                                 service_restore_error=1; } ;;
                    enabled-runtime) systemctl enable --runtime wifi1b-daemon.service >> "$LOG" 2>&1 || {
                                 log "WARNING: could not re-enable(runtime) pre-existing wifi1b service"
                                 service_restore_error=1; } ;;
                    disabled) systemctl disable wifi1b-daemon.service >> "$LOG" 2>&1 || {
                                  log "WARNING: could not re-disable pre-existing wifi1b service"
                                  service_restore_error=1; } ;;
                    masked) systemctl mask wifi1b-daemon.service >> "$LOG" 2>&1 || {
                                log "WARNING: could not re-mask pre-existing wifi1b service"
                                service_restore_error=1; } ;;
                    masked-runtime) systemctl mask --runtime wifi1b-daemon.service >> "$LOG" 2>&1 || {
                                log "WARNING: could not re-mask(runtime) pre-existing wifi1b service"
                                service_restore_error=1; } ;;
                esac
                if [ "$SERVICE_ACTIVE" = 1 ]; then
                    systemctl start wifi1b-daemon.service >> "$LOG" 2>&1 || {
                        log "WARNING: could not re-start pre-existing wifi1b service"
                        service_restore_error=1; }
                else
                    systemctl stop wifi1b-daemon.service >> "$LOG" 2>&1 || {
                        log "WARNING: could not re-stop pre-existing wifi1b service"
                        service_restore_error=1; }
                fi
            fi
        else
            log "kept independently changed/foreign wifi1b service; no restore"
        fi
        return 0
    fi
    if [ -z "$SERVICE_EXISTED" ]; then
        # Legacy ledger without a service entry: remove only exact owned bytes.
        if [ "$service_is_owned" = 1 ]; then
            rm -f "$SERVICE" 2>/dev/null || restore_failed "cannot remove owned wifi1b service (legacy ledger)"
            command -v systemctl >/dev/null 2>&1 && \
                systemctl disable wifi1b-daemon.service >> "$LOG" 2>&1 || \
                restore_failed "cannot disable legacy owned wifi1b service"
            log "removed owned wifi1b service (legacy ledger)"
        else
            log "kept unknown wifi1b service: legacy ledger has no ownership entry"
        fi
        return 0
    fi
    # Originally absent: remove only our exact bytes, never a foreign file.
    if [ "$service_is_owned" = 1 ]; then
        rm -f "$SERVICE" 2>/dev/null || restore_failed "cannot remove owned wifi1b service"
        if command -v systemctl >/dev/null 2>&1; then
            systemctl disable wifi1b-daemon.service >> "$LOG" 2>&1 || {
                log "WARNING: could not disable removed wifi1b service"
                service_restore_error=1; }
            systemctl stop wifi1b-daemon.service >> "$LOG" 2>&1 || {
                log "WARNING: could not stop removed wifi1b service"
                service_restore_error=1; }
            systemctl daemon-reload >> "$LOG" 2>&1 || true
        fi
        log "removed owned wifi1b service"
    else
        log "kept foreign wifi1b service (originally absent)"
    fi
}
restore_service
MDNS_DROPIN="${WIFI1B_MDNS_DROPIN:-/etc/systemd/system/carplay_mdnsd.service.d/60-wifi1b-foreground.conf}"
if cmp -s "$MDNS_DROPIN" "$PAYLOAD/systemd/60-wifi1b-foreground.conf"; then
    # An identical override may have existed before installation. Preserve it
    # using the original restore point, not the latest reinstall transaction.
    while IFS='|' read -r item existed destination; do
        [ "$destination" = "$MDNS_DROPIN" ] || continue
        if [ "$existed" = 1 ]; then
            io_copy "$LATEST/transaction/$item" "$MDNS_DROPIN" || fail "cannot restore original mdns override"
        else
            io_remove "$MDNS_DROPIN" || fail "cannot remove owned mdns foreground override"
        fi
    done < "$LATEST/transaction/files"
fi
CONNMAN_DROPIN="${WIFI1B_CONNMAN_DROPIN:-/etc/systemd/system/connman.service.d/60-scourge-nodevice.conf}"
if cmp -s "$CONNMAN_DROPIN" "$PAYLOAD/systemd/60-connman-nodevice.conf"; then
    while IFS='|' read -r item existed destination; do
        [ "$destination" = "$CONNMAN_DROPIN" ] || continue
        if [ "$existed" = 1 ]; then
            io_copy "$LATEST/transaction/$item" "$CONNMAN_DROPIN" || fail "cannot restore original connman override"
        else
            io_remove "$CONNMAN_DROPIN" || fail "cannot remove owned connman override"
        fi
    done < "$LATEST/transaction/files"
    rmdir "$(dirname "$CONNMAN_DROPIN")" 2>/dev/null || true
    systemctl daemon-reload >> "$LOG" 2>&1 || true
fi

# Restore VBT only when Wifi1b was the last app to change it and WiFi is not
# installed.  With both apps installed, leave the shared flag alone.
if [ ! -f /rw_data/wifi/wifi_daemon ]; then
    vbt_changed=0
    restore_vbt() {
        identity=$1
        vbt=$2
        bak="$RW_DIR/backup/vbt-$identity.original"
        applied="$RW_DIR/backup/vbt-$identity.applied"
        [ -f "$vbt" ] && [ -f "$bak" ] && [ -f "$applied" ] || return 0
        runtime_cleanup_safe_dir_path "$(dirname "$vbt")" || {
            restore_failed "unsafe VBT parent: $vbt"; return 0; }
        runtime_cleanup_safe_file_path "$vbt" || {
            restore_failed "unsafe VBT target: $vbt"; return 0; }
        cmp -s "$vbt" "$applied" || { log "kept independently changed VBT: $vbt"; return 0; }
        _vbt_tmp="$vbt.wifi1b-restore.$$"
        if [ -e "$_vbt_tmp" ] || [ -L "$_vbt_tmp" ]; then
            restore_failed "unsafe VBT temporary path: $_vbt_tmp"
            return 0
        fi
        if ! cp -p "$bak" "$_vbt_tmp"; then
            restore_failed "cannot stage VBT restore: $vbt"
            rm -f "$_vbt_tmp" 2>/dev/null || true
            return 0
        fi
        if ! mv -f "$_vbt_tmp" "$vbt"; then
            restore_failed "cannot move VBT restore into place: $vbt"
            rm -f "$_vbt_tmp" 2>/dev/null || true
            return 0
        fi
        vbt_changed=1
    }
    restore_vbt app /app/data/vbt/VBTConfig.json
    restore_vbt rw /rw_data/app_data/vbt/VBTConfig.json
    [ "$vbt_changed" != 1 ] || \
        systemctl restart vbtd.service >> "$LOG" 2>&1 || \
        restore_failed "cannot restart vbtd after VBT restore"
fi

# Remove only the wifi1b: entries this installer recorded.
remove_wifi1b() {
    f="$1"
    if [ ! -e "$f" ] && [ ! -L "$f" ]; then return 0; fi
    target="$f"
    if [ -L "$f" ]; then
        target=$(readlink -f "$f" 2>/dev/null || true)
        case "$target" in
            "${INSTALL_IO_ROOT:-/}"/tmp/*|/tmp/*|"${INSTALL_IO_ROOT:-/}"/etc/*|/etc/*) : ;;
            *) restore_failed "account symlink has unapproved target: $f"; return 0 ;;
        esac
    fi
    [ -n "$target" ] && runtime_cleanup_safe_file_path "$target" &&
        [ -r "$target" ] && [ -w "$target" ] || {
        restore_failed "unreadable/unwritable account file: $f"; return 0; }
    current=$(grep '^wifi1b:' "$target" 2>/dev/null || true)
    marker="$RW_DIR/backup/accounts/$(basename "$f")"
    if [ -z "$current" ]; then
        if [ -f "$marker" ]; then
            rm -f "$marker" 2>/dev/null || restore_failed "cannot remove stale account marker: $marker"
        fi
        return 0
    fi
    if [ ! -f "$marker" ] || [ -L "$marker" ] || [ ! -r "$marker" ]; then
        restore_failed "no account ownership record for retained wifi1b entry: $f"
        return 0
    fi
    expected=$(cat "$marker" 2>/dev/null) || {
        restore_failed "cannot read account ownership marker: $marker"; return 0; }
    if [ "$current" != "$expected" ]; then
        restore_failed "account entry changed independently: $f"
        return 0
    fi
    # Stage beside the resolved account file, then use verified io_copy: this
    # follows the intended firmware symlink without replacing the link.
    tmp=$(mktemp "${target}.tmp.XXXXXX" 2>/dev/null) || {
        restore_failed "cannot stage account removal for $f"; return 0; }
    if grep -v '^wifi1b:' "$target" > "$tmp" 2>/dev/null; then
        case "$target" in
            *shadow*) chmod 600 "$tmp" 2>/dev/null || true ;;
            *) chmod 644 "$tmp" 2>/dev/null || true ;;
        esac
        if io_copy "$tmp" "$target"; then
            if [ -f "$marker" ]; then
                rm -f "$marker" 2>/dev/null || restore_failed "cannot remove account marker: $marker"
            fi
            log "removed owned wifi1b account entry from $f"
            rm -f "$tmp" 2>/dev/null || true
            return 0
        fi
    fi
    rm -f "$tmp" 2>/dev/null || true
    restore_failed "cannot rewrite account file safely: $f"
}
remove_wifi1b /etc/passwd
remove_wifi1b /etc/passwd.legacy
remove_wifi1b /etc/passwd.secure
remove_wifi1b /etc/shadow
remove_wifi1b /etc/shadow.Global

remove_owned_file() {
    if [ -e "$1" ] || [ -L "$1" ]; then
        rm -f "$1" 2>/dev/null || restore_failed "cannot remove runtime file: $1"
    fi
}
# Keep backups; remove runtime pieces and secrets.
for _f in "$RW_DIR/wifi1b_daemon" "$RW_DIR/status.json" "$RW_DIR/daemon.log" \
          "$RW_DIR/config.conf" "$RW_DIR/wifi.txt" "$RW_DIR/home.json" \
          "$RW_DIR/apply.result" "$RW_DIR/bt.state" "$RW_DIR/ssh.bound" \
          "$RW_DIR/wpa_supplicant.conf" "$RW_DIR/wpa_supplicant.log" \
          "$RW_DIR/wpa_supplicant.pid" "$RW_DIR/scan.json" \
          "$RW_DIR/dhcp.leases" "$RW_DIR/dhcpd.pid" "$RW_DIR/dhcpd.log" \
          "$RW_DIR/udhcpc.pid" "$RW_DIR/udhcpc.sh" "$RW_DIR/udhcpc.log" \
          "$RW_DIR/bin/dropbearmulti" "$RW_DIR/bin/dropbear" \
          "$RW_DIR/bin/dropbearkey" "$RW_DIR/bin/wifi1b_qmlpatch" \
          "$RW_DIR/ssh/dropbear.pid" \
          "$RW_DIR/ssh/dropbear_ed25519_host_key" \
          "$RW_DIR/ssh/dropbear_rsa_host_key"; do
    remove_owned_file "$_f"
done
auth_ledger=0
if [ -f "$LATEST/transaction/files" ] &&
   grep -Fq "|$RW_DIR/ssh/authorized_keys" "$LATEST/transaction/files"; then
    auth_ledger=1
fi
if [ "$auth_ledger" = 1 ]; then
    remove_owned_file "$RW_DIR/ssh/authorized_keys"
    remove_owned_file "$RW_DIR/home/.ssh/authorized_keys"
else
    log "kept authorized_keys: no transaction ownership record"
fi
rmdir "$RW_DIR/bin" "$RW_DIR/ssh" "$RW_DIR/home/.ssh" "$RW_DIR/home" 2>/dev/null || true

if [ -f "$LATEST/transaction/files" ]; then
    while IFS='|' read -r item existed destination; do
        [ "$existed" = 1 ] || continue
        case "$destination" in
            "$RW_DIR/ssh/authorized_keys"|"$RW_DIR/home/.ssh/authorized_keys"|"$QML_ROOT/wifi/"*|"$QML_ROOT/hello/"*|"$QML_ROOT/battery/"*)
                runtime_cleanup_safe_dir_path "$(dirname "$destination")" || {
                    restore_failed "unsafe original-file parent: $destination"; continue; }
                mkdir -p "$(dirname "$destination")" 2>/dev/null || {
                    restore_failed "cannot create original-file directory: $destination"; continue; }
                _src="$LATEST/transaction/$item"
                if [ -L "$_src" ]; then
                    io_restore_entry "$_src" "$destination" || \
                        restore_failed "cannot restore original symlink: $destination"
                else
                    runtime_cleanup_safe_file_path "$destination" || {
                        restore_failed "unsafe original-file leaf: $destination"; continue; }
                    io_restore_entry "$_src" "$destination" || \
                        restore_failed "cannot restore original owner file: $destination"
                fi
                ;;
        esac
    done < "$LATEST/transaction/files"
fi

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload 2>/dev/null || true
fi
sync

# Restart AppHomeScreen.
restart="not-attempted"
old_home_pid=$(pidof AppHomeScreen 2>/dev/null || true)
launcher_pid=""
for p in /proc/[0-9]*; do
    [ -r "$p/comm" ] || continue
    if [ "$(cat "$p/comm" 2>/dev/null)" = "AppLauncher" ]; then launcher_pid="${p#/proc/}"; break; fi
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
            restart="dbus-ok"; break
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
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        [ "$(cat "$p/comm" 2>/dev/null)" = "AppHomeScreen" ] || continue
        exe=$(readlink -f "$p/exe" 2>/dev/null || true)
        if [ -n "$home_exe" ] && [ "$exe" = "$home_exe" ] &&
           [ "$(basename "$exe")" = "AppHomeScreen" ]; then
            if kill -TERM "$(basename "$p")" 2>/dev/null; then
                killed_home=1
            else
                restore_failed "cannot signal owned AppHomeScreen"
            fi
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
if [ -n "$new_home_pid" ] && [ "$new_home_pid" != "$old_home_pid" ]; then restart="$restart verified=1"; else restart="$restart verified=0 REBOOT_REQUIRED"; fi
log "restart=$restart"
sync

final_status=OK
case "$restart" in *verified=0*) final_status=REBOOT_REQUIRED;; esac
if [ "${service_restore_error:-0}" = 1 ]; then
    final_status=FAIL
    log "WARNING: original service enablement/active state was not fully restored"
fi
if [ "${restore_errors:-0}" -gt 0 ]; then
    final_status=FAIL
    log "WARNING: $restore_errors owned cleanup step(s) failed; backups preserved"
fi
result "status=$final_status" "backup=$LATEST" "restart=$restart" \
       "cleanup_errors=$restore_errors" "log=$LOG"
log "=== Wifi1b restore done ==="
[ "$final_status" != FAIL ] || exit 1
exit 0
