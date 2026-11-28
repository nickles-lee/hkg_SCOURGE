#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L AU 260319
# PROJECTION_PAYLOAD_RESTORE=1
# Projection+ uninstaller.  WIP: VM-tested only (restore not yet run on a car).
#
# Puts back, byte for byte, every file recorded in
# /rw_data/projection/backup/latest, removes every file Projection+ added
# (QML, icons, daemon, unit), stops and disables projection-daemon, clears
# the QML cache and restarts AppHomeScreen.  Works after an interrupted
# install: the installer records each file before it writes it.
# Keeps /rw_data/projection/{backup,tools,*.log} for post-mortem.
# Never touches /rw_data/hudnav.
#
#   SSH:  sh /rw_data/projection/tools/run_stuff_restore.sh
#         (or sh run_stuff_restore.sh --from-dir DIR from the uploaded payload)
#         Refuses, writing nothing, if a recorded file was changed by something
#         else after install; --force restores the originals anyway.
#   USB:  rename this file to run_stuff.sh at the stick root.
set -u
umask 022
PATH="${PROJECTION_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
R="${PROJECTION_FAKE_ROOT:-}"

USAGE="usage: sh run_stuff_restore.sh [--from-dir DIR] [--force]"
FROM_DIR=""
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --from-dir) [ $# -ge 2 ] || { echo "$USAGE"; exit 2; }; FROM_DIR=$2; shift 2 ;;
        --from-dir=*) FROM_DIR=${1#--from-dir=}; shift ;;
        --force) FORCE=1; shift ;;
        -h|--help) echo "$USAGE"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

SELF_DIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
if [ -n "$FROM_DIR" ]; then
    MODE=ssh; SRC=$(cd "$FROM_DIR" 2>/dev/null && pwd) || { echo "status=FAIL error=cannot enter $FROM_DIR"; exit 1; }
elif [ -n "${USB_DIR:-}" ]; then
    MODE=usb; SRC=$USB_DIR
    mount -o remount,rw "$USB_DIR" 2>/dev/null || true
else
    MODE=ssh; SRC=$SELF_DIR
fi

QML_ROOT="$R/app/share/AppHomeScreen"
RW_DIR="$R/rw_data/projection"
UNIT=projection-daemon.service
SERVICE="$R/etc/systemd/system/$UNIT"
LATEST="$RW_DIR/backup/latest"
LOG="$RW_DIR/restore.log"
OUT="$RW_DIR/restore-result.txt"
[ "$MODE" = usb ] && OUT="$USB_DIR/run_stuff_result.txt"

mkdir -p "$RW_DIR" 2>/dev/null
: >> "$LOG" 2>/dev/null || LOG=/tmp/projection_restore.log
log() {
    echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null
    echo "$*"
}
result() {
    {
        echo "PROJECTION_RESTORE"
        echo "time=$(date -u 2>/dev/null)"
        echo "mode=$MODE"
        for line in "$@"; do echo "$line"; done
        echo "log=$LOG"
    } > "$OUT" 2>/dev/null
    [ "$OUT" = "$RW_DIR/restore-result.txt" ] || cp -f "$OUT" "$RW_DIR/restore-result.txt" 2>/dev/null || true
    echo "---- result ----"
    cat "$OUT" 2>/dev/null || for line in "$@"; do echo "$line"; done
}
fail() { log "FAIL: $*"; result "status=FAIL" "error=$*"; exit 1; }

log "=== projection restore start mode=$MODE ==="

MD5SUM_BIN=md5sum
command -v md5sum >/dev/null 2>&1 || MD5SUM_BIN=md5sum.coreutils
SHA256SUM_BIN=sha256sum
command -v sha256sum >/dev/null 2>&1 || SHA256SUM_BIN=sha256sum.coreutils
export MD5SUM_BIN SHA256SUM_BIN

if [ "$(id -u 2>/dev/null)" != 0 ] && [ "${PROJECTION_ALLOW_NONROOT:-0}" != 1 ]; then
    fail "must run as root"
fi

# Libraries: from a verified payload when one is given, else the copies the
# installer placed in /rw_data/projection/tools before its first write.
LIB=""
for cand in "$SRC/payload/tools" "$SELF_DIR" "$RW_DIR/tools"; do
    if [ -f "$cand/install_io.sh" ] && [ -f "$cand/projection_txn.sh" ]; then
        LIB=$cand; break
    fi
done
[ -n "$LIB" ] || fail "install_io.sh/projection_txn.sh not found (looked in $SRC/payload/tools, $SELF_DIR, $RW_DIR/tools)"
if [ "$LIB" = "$SRC/payload/tools" ] && [ -f "$SRC/manifest.txt" ]; then
    for t in install_io.sh projection_txn.sh; do
        want=$(grep "^tools/$t " "$SRC/manifest.txt" | head -n 1 | cut -d' ' -f3)
        got=$("$SHA256SUM_BIN" "$LIB/$t" 2>/dev/null | cut -d' ' -f1)
        [ -n "$want" ] && [ "$want" = "$got" ] || fail "library $t does not match manifest.txt"
    done
fi
log "libraries from $LIB"
. "$LIB/install_io.sh"
. "$LIB/projection_txn.sh"

[ -d "$LATEST" ] || fail "no $LATEST: nothing recorded to restore"
BK=$(cd "$LATEST" && pwd -P)
log "restoring from $BK (state $(txn_state "$BK"))"

# Gate: every recorded file must still be the original, absent, or a payload
# we wrote. Anything else was changed after install (e.g. another package
# reinstalled over a layout); restoring would overwrite that, so refuse with
# nothing written unless --force.
if ! txn_conflicts "$BK" > "$RW_DIR/restore-conflicts.txt" 2>/dev/null; then
    while IFS= read -r c; do log "changed since install: $c"; done < "$RW_DIR/restore-conflicts.txt"
    if [ "$FORCE" = 1 ]; then
        log "WARNING: --force: overwriting files changed since install"
    else
        fail "files changed since Projection+ was installed (see $RW_DIR/restore-conflicts.txt); nothing restored. Re-run with --force to put the pre-install originals back anyway."
    fi
fi

io_init || fail "cannot identify root mount"
trap 'io_restore_root || true' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
io_settle_root || fail "root filesystem is not stably writable"

if command -v systemctl >/dev/null 2>&1; then
    systemctl stop "$UNIT" >> "$LOG" 2>&1 || true
    systemctl disable "$UNIT" >> "$LOG" 2>&1 || true
fi
if [ -z "$R" ]; then
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        case "$(cat "$p/comm" 2>/dev/null)" in
            projection_daem*) kill -TERM "${p#/proc/}" 2>/dev/null || true ;;
        esac
    done
fi

restore_status=ok
txn_restore "$BK" || restore_status=incomplete
# Belt and braces: the unit and its enablement link must not survive.
for f in "$SERVICE" "$R/etc/systemd/system/multi-user.target.wants/$UNIT"; do
    if [ -e "$f" ] || [ -L "$f" ]; then io_remove "$f" || restore_status=incomplete; fi
done
rm -f "$RW_DIR/projection_daemon" "$RW_DIR/daemon.log" "$RW_DIR/installed.manifest" 2>/dev/null || true
[ ! -e "$RW_DIR/projection_daemon" ] || restore_status=incomplete

if [ "$restore_status" = ok ]; then
    txn_set_state "$BK" restored || true
fi
command -v systemctl >/dev/null 2>&1 && systemctl daemon-reload >> "$LOG" 2>&1 || true

rm -rf "$R/root/.cache/AppHomeScreen" "$R/root/.cache/qtshadercache" \
       "$R/tmp/qmlcache" "$R/tmp/qt-qml-cache" 2>/dev/null || true
sync

io_restore_root || log "WARNING: cannot restore root mount state"
restart_homescreen
log "restart=$restart"
sync

status=OK
[ "$restore_status" = ok ] || status=FAIL
result "status=$status" "restored_from=$BK" "restore=$restore_status" "restart=$restart"
[ "$restart" = reboot-required ] && echo "NEXT: reboot the head unit (Reboot tile) to load the restored home screen."
log "=== projection restore done status=$status ==="
[ "$status" = OK ] || exit 1
exit 0
