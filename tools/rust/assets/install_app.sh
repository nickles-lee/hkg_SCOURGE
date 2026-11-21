#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Generic SCOURGE Rust app/daemon installer. Generated into a USB package by
# tools/rust/scripts/package_app.sh; payload/app.env supplies the app fields.
# TARGET: Gen5W_L 260128 / BusyBox ash. Runs as root from the initial_root
# failsafe (docs/06). Never patches stock binaries, the updater or partitions.
set -u
PATH="${APP_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
RESULT="$USB_DIR/run_stuff_result.txt"
APP_SERVICE_DIR="${APP_SERVICE_DIR:-/etc/systemd/system}"
APP_RW_DIR="${APP_RW_DIR:-}"
SMACK_SELF="${APP_SMACK_SELF:-}"
SMACK_CHECK="not-run"
ACTION=""
START="not-run"
LOG="/tmp/scourge_app_install.log"

log() {
    echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null
}

result() {
    {
        echo "RUST_APP_PAYLOAD"
        echo "time=$(date -u 2>/dev/null)"
        echo "usb=$USB_DIR"
        echo "uid=$(id -u 2>/dev/null)"
        echo "$@"
    } > "$RESULT" 2>&1
    [ -z "${APP_RW_DIR:-}" ] || [ ! -d "$APP_RW_DIR" ] || \
        cp -f "$RESULT" "$APP_RW_DIR/result.txt" 2>/dev/null || true
}

fail() {
    log "FAIL: $*"
    result "status=FAIL" "error=$*"
    exit 1
}

[ -r "$PAYLOAD/app.env" ] || fail "missing payload/app.env"
[ -r "$PAYLOAD/lib/install_io.sh" ] || fail "missing payload/lib/install_io.sh"

log "== manifest verification =="
( cd "$USB_DIR" && md5sum -c manifest.md5 ) >> "$LOG" 2>&1 ||
    fail "manifest verification failed (do not install)"

# shellcheck disable=SC1091
. "$PAYLOAD/app.env" || fail "cannot read payload/app.env"
for var in APP_NAME APP_VERSION APP_BIN APP_SERVICE APP_DATA_DIR; do
    eval "value=\${$var:-}"
    [ -n "$value" ] || fail "payload/app.env is missing $var"
done
case "$APP_DATA_DIR" in
    /*) ;;
    *) fail "APP_DATA_DIR is not absolute: $APP_DATA_DIR" ;;
esac
APP_RW_DIR="${APP_RW_DIR:-$APP_DATA_DIR}"

# shellcheck disable=SC1091
. "$PAYLOAD/lib/install_io.sh" || fail "cannot source install_io.sh"
io_init >> "$LOG" 2>&1 || fail "cannot initialise the root mount state"
trap 'io_restore_root || true' EXIT
io_mkdir "$APP_RW_DIR" || fail "cannot create $APP_RW_DIR"
LOG="$APP_RW_DIR/install.log"
: >> "$LOG" 2>/dev/null || true
log "RUST_APP_PAYLOAD install $APP_NAME $APP_VERSION"
log "USB_DIR=$USB_DIR APP_RW_DIR=$APP_RW_DIR APP_SERVICE_DIR=$APP_SERVICE_DIR"

[ "${APP_TEST_UID:-$(id -u 2>/dev/null)}" = 0 ] || fail "installer must run as root"
command -v md5sum >/dev/null 2>&1 || fail "md5sum is missing"
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum is missing"

free_kb=$(df -Pk "$APP_RW_DIR" 2>/dev/null | awk 'NR==2 {print $4}')
[ -n "${free_kb:-}" ] || fail "cannot determine free space for $APP_RW_DIR"
[ "$free_kb" -ge "${APP_MIN_FREE_KB:-8192}" ] ||
    fail "not enough free space in $APP_RW_DIR (${free_kb} KiB free, need ${APP_MIN_FREE_KB:-8192} KiB)"

smack_init() {
    if [ -n "${APP_SMACK_SELF:-}" ]; then
        SMACK_SELF=$APP_SMACK_SELF
        log "SMACK: using override label '$SMACK_SELF'"
        return 0
    fi
    grep -q ' smackfs ' /proc/mounts 2>/dev/null ||
        fail "smackfs is not mounted (SMACK is required)"
    SMACK_SELF=$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')
    [ -n "$SMACK_SELF" ] || fail "cannot read /proc/self/attr/current"
    log "SMACK: installer label '$SMACK_SELF'"
}

set_exec_label() {
    _bin=$1
    command -v chsmack >/dev/null 2>&1 || fail "chsmack missing (SMACK is required)"
    command -v getfattr >/dev/null 2>&1 ||
        fail "getfattr missing (SMACK readback is required)"
    chsmack -e "$SMACK_SELF" "$_bin" >> "$LOG" 2>&1 ||
        fail "exec label failed: $_bin"
    _actual=$(getfattr -n security.SMACK64EXEC --only-values "$_bin" 2>/dev/null || true)
    [ -n "$_actual" ] ||
        fail "cannot read back SMACK64EXEC for $_bin"
    [ "$_actual" = "$SMACK_SELF" ] ||
        fail "exec label readback failed: $_bin (expected '$SMACK_SELF', got '$_actual')"
    SMACK_CHECK="ok"
    log "exec label readback: $_bin = $_actual"
}

have_systemd() {
    [ "${APP_NO_SYSTEMD:-0}" != 1 ] || return 1
    [ -d /run/systemd/system ] || return 1
    command -v systemctl >/dev/null 2>&1
}

smack_init

UNIT_SRC="$PAYLOAD/systemd/$APP_SERVICE"
UNIT_DST="$APP_SERVICE_DIR/$APP_SERVICE"
[ -f "$UNIT_SRC" ] || fail "missing payload/systemd/$APP_SERVICE"
io_mkdir "$APP_RW_DIR/bin" || fail "cannot create $APP_RW_DIR/bin"

NEW_SHA=$(tr -d '\r\n' < "$PAYLOAD/app.sha256" 2>/dev/null)
[ -n "$NEW_SHA" ] || NEW_SHA=$(sha256sum "$PAYLOAD/bin/$APP_BIN" 2>/dev/null | cut -d' ' -f1)
[ -n "$NEW_SHA" ] || fail "cannot read payload/app.sha256"
OLD_SHA=$(sha256sum "$APP_RW_DIR/bin/$APP_BIN" 2>/dev/null | cut -d' ' -f1)
SAME_BIN=0
[ "$OLD_SHA" = "$NEW_SHA" ] && SAME_BIN=1
SAME_UNIT=0
cmp -s "$UNIT_SRC" "$UNIT_DST" 2>/dev/null && SAME_UNIT=1

if [ "$SAME_BIN" = 1 ] && [ "$SAME_UNIT" = 1 ]; then
    ACTION=up-to-date
    START=already-installed
    log "binary and unit already up to date"
else
    if [ -n "$OLD_SHA" ]; then
        ACTION=upgraded
    else
        ACTION=installed
    fi
    if have_systemd; then
        systemctl stop "$APP_SERVICE" >> "$LOG" 2>&1 || true
    fi
    io_copy "$PAYLOAD/bin/$APP_BIN" "$APP_RW_DIR/bin/$APP_BIN" ||
        fail "binary copy failed"
    chmod 0755 "$APP_RW_DIR/bin/$APP_BIN" 2>/dev/null ||
        fail "cannot chmod $APP_RW_DIR/bin/$APP_BIN"
    actual=$(sha256sum "$APP_RW_DIR/bin/$APP_BIN" 2>/dev/null | cut -d' ' -f1)
    [ "$actual" = "$NEW_SHA" ] ||
        fail "installed binary hash mismatch (got ${actual:-none}, want $NEW_SHA)"

    if [ -f "$UNIT_DST" ] && [ "$SAME_UNIT" = 0 ]; then
        stamp=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || echo unknown)
        backup="$APP_RW_DIR/backup/$stamp"
        io_mkdir "$backup" || fail "cannot create unit backup directory"
        io_copy "$UNIT_DST" "$backup/$APP_SERVICE" || fail "unit backup failed"
        cp -p "$UNIT_DST" "$USB_DIR/backup_current/$APP_SERVICE" 2>/dev/null ||
            log "WARNING: could not copy the old unit to the stick backup_current/"
        log "backed up old unit to $backup/$APP_SERVICE"
    fi
    io_mkdir "$APP_SERVICE_DIR" || fail "cannot create $APP_SERVICE_DIR"
    io_copy "$UNIT_SRC" "$UNIT_DST" || fail "service install failed"
    chmod 0644 "$UNIT_DST" 2>/dev/null || fail "cannot chmod $UNIT_DST"
fi

set_exec_label "$APP_RW_DIR/bin/$APP_BIN"

if have_systemd; then
    systemctl daemon-reload >> "$LOG" 2>&1 || fail "systemctl daemon-reload failed"
    systemctl enable "$APP_SERVICE" >> "$LOG" 2>&1 || fail "systemctl enable failed"
    if [ "$ACTION" = up-to-date ] && systemctl is-active --quiet "$APP_SERVICE"; then
        START=already-running
    else
        systemctl restart "$APP_SERVICE" >> "$LOG" 2>&1 || fail "service start failed"
        START=started
    fi
else
    START="skipped:no-systemd"
fi

log "result action=$ACTION start=$START smack=$SMACK_SELF/$SMACK_CHECK"
result "status=OK" \
    "app=$APP_NAME" \
    "version=$APP_VERSION" \
    "action=$ACTION" \
    "binary=$APP_RW_DIR/bin/$APP_BIN" \
    "sha256=$NEW_SHA" \
    "service=$APP_SERVICE" \
    "start=$START" \
    "smack_self=$SMACK_SELF" \
    "smack_check=$SMACK_CHECK"
exit 0
