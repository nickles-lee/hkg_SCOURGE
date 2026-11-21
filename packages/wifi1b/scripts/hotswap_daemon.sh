#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Owner-run daemon-only update over SSH.
#
# Stopping wifi1b-daemon.service also stops its dropbear children, so a
# foreground update running inside that SSH session would be killed halfway.
# Launch this script detached (systemd-run --no-block, or setsid) and read
# hotswap.result / hotswap.log after reconnecting.
set -u
PATH=/bin:/sbin:/usr/bin:/usr/sbin

APP=/rw_data/wifi1b
LIVE="$APP/wifi1b_daemon"
NEW="$APP/wifi1b_daemon.new"
NEW_SHA="$NEW.sha256"
SERVICE=wifi1b-daemon.service
BACKUP_DIR="$APP/backup"
LOG="$APP/hotswap.log"
RESULT="$APP/hotswap.result"
LOCK="$APP/hotswap.lock"
CFG="$APP/config.conf"
PORT=2224

mkdir -p "$BACKUP_DIR" 2>/dev/null || true
exec >> "$LOG" 2>&1
log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*"; }
log "=== hot-swap start pid=$$ ==="

exec 9>"$LOCK" 2>/dev/null || true
if command -v flock >/dev/null 2>&1; then
    flock -n 9 || { log "another hot-swap holds $LOCK"; exit 1; }
fi

fail() {
    log "FAIL: $*"
    {
        echo "ok=0"
        echo "rollback=not-needed"
        echo "reason=$*"
        echo "time=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$RESULT"
    exit 1
}

rm -f "$RESULT" 2>/dev/null || true
[ -f "$NEW" ] || fail "missing $NEW"
[ -f "$NEW_SHA" ] || fail "missing $NEW_SHA"
[ -f "$LIVE" ] || fail "missing live binary $LIVE"
[ -x "$NEW" ] || fail "$NEW is not executable"

want=$(cat "$NEW_SHA" 2>/dev/null | tr -d ' \t\r\n' | cut -c1-64)
[ -n "$want" ] || fail "empty $NEW_SHA"
got=$(sha256sum "$NEW" 2>/dev/null | cut -d' ' -f1)
[ "$got" = "$want" ] || fail "staged binary hash mismatch: got $got want $want"

if command -v file >/dev/null 2>&1; then
    file "$NEW" | grep -q 'ELF 64-bit LSB.*x86-64' || fail "$NEW is not an x86-64 ELF"
    file "$NEW" | grep -q 'statically linked' || fail "$NEW is not statically linked"
fi

if [ "$(sha256sum "$LIVE" 2>/dev/null | cut -d' ' -f1)" = "$want" ]; then
    log "live binary already matches staged hash; no restart"
    {
        echo "ok=1"
        echo "rollback=not-needed"
        echo "reason=already-current"
        echo "time=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$RESULT"
    exit 0
fi

ssh_en=$(sed -n 's/^ssh=//p' "$CFG" 2>/dev/null | tail -n 1)
want_ssh=1
[ "$ssh_en" = "1" ] || want_ssh=0
log "ssh_enabled=$want_ssh"

# Give the launching SSH command time to return before the daemon stop drops
# its connection and kills every child of that session.
sleep 2

stamp=$(date -u +%Y%m%dT%H%M%SZ)
backup="$BACKUP_DIR/wifi1b_daemon.pre-hotswap-$stamp"

log "stopping $SERVICE"
systemctl stop "$SERVICE" >/dev/null 2>&1 || true
i=0
while [ "$i" -lt 30 ]; do
    pid=""
    if command -v pidof >/dev/null 2>&1; then
        pid=$(pidof wifi1b_daemon 2>/dev/null || true)
    else
        pid=$(ps w 2>/dev/null | grep '[w]ifi1b_daemon' | awk '{print $1}' | head -n 1)
    fi
    [ -z "$pid" ] && break
    i=$((i + 1))
    sleep 1
done
[ -z "$pid" ] || log "warning: daemon pid $pid still present after stop"

cp -p "$LIVE" "$backup" || fail "cannot back up $LIVE"
log "backup: $backup"

cp -f "$NEW" "$LIVE" || fail "cannot install staged binary"
chmod 755 "$LIVE" 2>/dev/null || true
chown 0:0 "$LIVE" 2>/dev/null || true
if command -v chsmack >/dev/null 2>&1; then
    chsmack -a _ "$LIVE" 2>/dev/null || true
fi
sync

log "starting $SERVICE"
if ! systemctl start "$SERVICE" >/dev/null 2>&1; then
    log "start failed; restoring backup"
    cp -f "$backup" "$LIVE" 2>/dev/null || true
    chmod 755 "$LIVE" 2>/dev/null || true
    chown 0:0 "$LIVE" 2>/dev/null || true
    sync
    systemctl start "$SERVICE" >/dev/null 2>&1 || true
    {
        echo "ok=0"
        echo "rollback=restored"
        echo "reason=start-failed"
        echo "backup=$backup"
        echo "time=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$RESULT"
    exit 1
fi

api=no
listener=no
i=0
while [ "$i" -lt 120 ]; do
    if command -v socat >/dev/null 2>&1 &&
       printf 'GET /status HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n' |
           socat -T 2 - TCP:127.0.0.1:8093 2>/dev/null | grep -q '^HTTP/'; then
        api=yes
    fi
    if netstat -ltn 2>/dev/null | grep -q ":$PORT "; then
        listener=yes
        break
    fi
    if [ "$want_ssh" = 0 ] && [ "$api" = yes ]; then
        break
    fi
    i=$((i + 1))
    sleep 1
done

build=$(sed -n 's/.*"build_id":"\([^"]*\)".*/\1/p' "$APP/status.json" 2>/dev/null | head -n 1)
log "api=$api listener=$listener ssh_enabled=$want_ssh build_id=$build"

if [ "$want_ssh" = 1 ] && [ "$listener" != yes ]; then
    log "listener did not appear; restoring backup"
    systemctl stop "$SERVICE" >/dev/null 2>&1 || true
    cp -f "$backup" "$LIVE" 2>/dev/null || true
    chmod 755 "$LIVE" 2>/dev/null || true
    chown 0:0 "$LIVE" 2>/dev/null || true
    sync
    systemctl start "$SERVICE" >/dev/null 2>&1 || true
    {
        echo "ok=0"
        echo "rollback=restored"
        echo "reason=no-listener"
        echo "backup=$backup"
        echo "time=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$RESULT"
    exit 1
fi

{
    echo "ok=1"
    echo "rollback=not-needed"
    echo "listener=$listener"
    echo "ssh_enabled=$want_ssh"
    echo "build_id=$build"
    echo "backup=$backup"
    echo "time=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$RESULT"
log "=== hot-swap OK ==="
exit 0
