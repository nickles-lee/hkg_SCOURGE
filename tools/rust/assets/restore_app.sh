#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Generic SCOURGE Rust app/daemon restore. Generated into a USB package by
# tools/rust/scripts/package_app.sh; rename/copy it over run_stuff.sh to run.
# Removes the app binary and unit; app data is preserved unless APP_PURGE=1.
# TARGET: Gen5W_L 260128 / BusyBox ash.
set -u
PATH="${APP_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-/media/usb}"
PAYLOAD="$USB_DIR/payload"
RESULT="$USB_DIR/run_stuff_result.txt"
APP_SERVICE_DIR="${APP_SERVICE_DIR:-/etc/systemd/system}"
APP_RW_DIR="${APP_RW_DIR:-}"
LOG="/tmp/scourge_app_restore.log"

log() {
    echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null
}

result() {
    {
        echo "RUST_APP_RESTORE"
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

( cd "$USB_DIR" && md5sum -c manifest.md5 ) >> "$LOG" 2>&1 ||
    fail "manifest verification failed (do not restore)"

# shellcheck disable=SC1091
. "$PAYLOAD/app.env" || fail "cannot read payload/app.env"
for var in APP_NAME APP_BIN APP_SERVICE APP_DATA_DIR; do
    eval "value=\${$var:-}"
    [ -n "$value" ] || fail "payload/app.env is missing $var"
done
APP_RW_DIR="${APP_RW_DIR:-$APP_DATA_DIR}"

# shellcheck disable=SC1091
. "$PAYLOAD/lib/install_io.sh" || fail "cannot source install_io.sh"
io_init >> "$LOG" 2>&1 || fail "cannot initialise the root mount state"
trap 'io_restore_root || true' EXIT
io_mkdir "$APP_RW_DIR" || true
LOG="$APP_RW_DIR/restore.log"
: >> "$LOG" 2>/dev/null || true
log "RUST_APP_RESTORE $APP_NAME"

[ "${APP_TEST_UID:-$(id -u 2>/dev/null)}" = 0 ] || fail "restore must run as root"

have_systemd() {
    [ "${APP_NO_SYSTEMD:-0}" != 1 ] || return 1
    [ -d /run/systemd/system ] || return 1
    command -v systemctl >/dev/null 2>&1
}

UNIT_DST="$APP_SERVICE_DIR/$APP_SERVICE"
BIN="$APP_RW_DIR/bin/$APP_BIN"

if have_systemd; then
    systemctl stop "$APP_SERVICE" >> "$LOG" 2>&1 || true
    systemctl disable "$APP_SERVICE" >> "$LOG" 2>&1 || true
fi
if [ -f "$UNIT_DST" ]; then
    io_remove "$UNIT_DST" || fail "cannot remove $UNIT_DST"
fi
if [ -f "$BIN" ]; then
    io_remove "$BIN" || fail "cannot remove $BIN"
fi
rmdir "$APP_RW_DIR/bin" 2>/dev/null || true
if have_systemd; then
    systemctl daemon-reload >> "$LOG" 2>&1 || true
fi

DATA_PRESERVED=1
if [ "${APP_PURGE:-0}" = 1 ]; then
    case "$APP_RW_DIR" in
        /|/rw_data|/rw_data/) fail "refusing to purge $APP_RW_DIR" ;;
    esac
    io_ensure_rw || fail "cannot make the root writable for purge"
    rm -rf "$APP_RW_DIR" || fail "cannot purge $APP_RW_DIR"
    LOG=/tmp/scourge_app_restore.log
    DATA_PRESERVED=0
fi

log "result restored data_preserved=$DATA_PRESERVED"
result "status=OK" \
    "app=$APP_NAME" \
    "action=restored" \
    "service=$APP_SERVICE" \
    "data_dir=$APP_RW_DIR" \
    "data_preserved=$DATA_PRESERVED"
exit 0
