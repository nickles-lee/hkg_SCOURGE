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
PATH=${WIFI1B_HOTSWAP_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}

APP=${WIFI1B_HOTSWAP_APP:-/rw_data/wifi1b}
SERVICE=${WIFI1B_HOTSWAP_SERVICE:-wifi1b-daemon.service}
SYSTEMCTL=${WIFI1B_HOTSWAP_SYSTEMCTL:-systemctl}
API_PORT=${WIFI1B_HOTSWAP_API_PORT:-8093}
SSH_PORT=${WIFI1B_HOTSWAP_SSH_PORT:-2224}
START_DELAY=${WIFI1B_HOTSWAP_START_DELAY:-2}
VERIFY_WAIT=${WIFI1B_HOTSWAP_VERIFY_WAIT:-120}
RECOVERY_WAIT=${WIFI1B_HOTSWAP_RECOVERY_WAIT:-30}
LIVE="$APP/wifi1b_daemon"
NEW="$APP/wifi1b_daemon.new"
NEW_SHA="$NEW.sha256"
BACKUP_DIR="$APP/backup"
LOG="$APP/hotswap.log"
RESULT="$APP/hotswap.result"
LOCK="$APP/hotswap.lock"
CFG="$APP/config.conf"

state=pre                 # pre -> stopping -> stopped -> installing -> installed -> committed
reason=""
backup=""
tmp=""
label64=""
labelexec=""
api=no
listener=no
want_ssh=1
build=""
rollback="not-needed"

mkdir -p "$BACKUP_DIR" 2>/dev/null || true
exec >> "$LOG" 2>&1
log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*"; }

write_result() {
    ok=$1
    rb=$2
    why=$3
    tmp_result="$RESULT.tmp.$$"
    {
        echo "ok=$ok"
        echo "rollback=$rb"
        echo "reason=$why"
        echo "listener=$listener"
        echo "ssh_enabled=$want_ssh"
        echo "api=$api"
        [ -z "$build" ] || echo "build_id=$build"
        [ -z "$backup" ] || echo "backup=$backup"
        echo "time=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$tmp_result" 2>/dev/null && mv -f "$tmp_result" "$RESULT" 2>/dev/null
    rm -f "$tmp_result" 2>/dev/null || true
}

probe_api() {
    if command -v socat >/dev/null 2>&1; then
        printf 'GET /status HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n' |
            socat -T 2 - TCP:127.0.0.1:"$API_PORT" 2>/dev/null | grep -q '^HTTP/'
    elif command -v wget >/dev/null 2>&1; then
        wget -q -T 2 -O - "http://127.0.0.1:$API_PORT/status" 2>/dev/null |
            grep -q '"ok":'
    elif command -v curl >/dev/null 2>&1; then
        curl -fsS --max-time 2 "http://127.0.0.1:$API_PORT/status" 2>/dev/null |
            grep -q '"ok":'
    else
        return 2
    fi
}

probe_listener() {
    if command -v netstat >/dev/null 2>&1; then
        netstat -ltn 2>/dev/null | grep -q ":$SSH_PORT "
    elif command -v ss >/dev/null 2>&1; then
        ss -ltn 2>/dev/null | grep -q ":$SSH_PORT "
    else
        return 2
    fi
}

record_labels() {
    if command -v getfattr >/dev/null 2>&1; then
        label64=$(getfattr -n security.SMACK64 --only-values "$LIVE" 2>/dev/null |
                  tr -d '\000\n')
        labelexec=$(getfattr -n security.SMACK64EXEC --only-values "$LIVE" 2>/dev/null |
                    tr -d '\000\n')
    fi
}

apply_labels() {
    target=$1
    if command -v chsmack >/dev/null 2>&1; then
        [ -z "$label64" ] || chsmack -a "$label64" "$target" 2>/dev/null || true
        [ -z "$labelexec" ] || chsmack -e "$labelexec" "$target" 2>/dev/null || true
    elif command -v setfattr >/dev/null 2>&1; then
        [ -z "$label64" ] ||
            setfattr -n security.SMACK64 -v "$label64" "$target" 2>/dev/null || true
        [ -z "$labelexec" ] ||
            setfattr -n security.SMACK64EXEC -v "$labelexec" "$target" 2>/dev/null || true
    fi
}

start_service() {
    "$SYSTEMCTL" start "$SERVICE" >/dev/null 2>&1
}

recover() {
    [ "$state" = committed ] && return 0
    log "recovery: state=$state reason=$reason"
    case "$state" in
        installing)
            [ -z "$tmp" ] || rm -f "$tmp"
            rollback="service-restarted"
            start_service || log "recovery: restart failed"
            ;;
        installed)
            if [ -n "$backup" ] && [ -f "$backup" ]; then
                rt="$LIVE.hotswap.restore.$$"
                if cp -f "$backup" "$rt" 2>/dev/null; then
                    chmod 755 "$rt" 2>/dev/null || true
                    chown 0:0 "$rt" 2>/dev/null || true
                    apply_labels "$rt"
                    sync
                    if mv -f "$rt" "$LIVE" 2>/dev/null; then
                        rollback="restored"
                    else
                        rm -f "$rt" 2>/dev/null || true
                        rollback="restore-failed"
                    fi
                else
                    rm -f "$rt" 2>/dev/null || true
                    rollback="restore-failed"
                fi
            else
                rollback="restore-failed"
            fi
            start_service || log "recovery: restart failed"
            ;;
        stopped|stopping)
            rollback="service-restarted"
            start_service || log "recovery: restart failed"
            ;;
        pre)
            rollback="not-needed"
            ;;
    esac
    # Best effort: give the restored daemon a short window to answer.
    i=0
    while [ "$i" -lt "$RECOVERY_WAIT" ]; do
        if probe_api >/dev/null 2>&1; then api=yes; break; fi
        if [ "$want_ssh" = 1 ] && probe_listener >/dev/null 2>&1; then
            listener=yes
        fi
        i=$((i + 1))
        sleep 1
    done
    write_result 0 "$rollback" "$reason"
    state=committed
}

on_exit() {
    rc=$?
    trap - EXIT INT TERM HUP
    if [ "$state" != committed ]; then
        [ -n "$reason" ] || reason="interrupted"
        recover
    fi
    exit "$rc"
}

fail() {
    reason=$*
    log "FAIL: $reason"
    exit 1
}

trap on_exit EXIT
trap 'reason=interrupted; exit 130' INT
trap 'reason=terminated; exit 143' TERM
trap 'reason=hangup; exit 129' HUP

log "=== hot-swap start pid=$$ ==="

exec 9>"$LOCK" 2>/dev/null || true
if command -v flock >/dev/null 2>&1; then
    flock -n 9 || { log "another hot-swap holds $LOCK"; exit 1; }
fi

rm -f "$RESULT" 2>/dev/null || true
[ -f "$NEW" ] || fail "missing $NEW"
[ -f "$NEW_SHA" ] || fail "missing $NEW_SHA"
[ -f "$LIVE" ] || fail "missing live binary $LIVE"
[ -x "$NEW" ] || fail "$NEW is not executable"

want=$(cat "$NEW_SHA" 2>/dev/null | tr -d ' \t\r\n' | cut -c1-64)
[ -n "$want" ] || fail "empty $NEW_SHA"
got=$(sha256sum "$NEW" 2>/dev/null | cut -d' ' -f1)
[ "$got" = "$want" ] || fail "staged binary hash mismatch: got $got want $want"

new_magic=$(dd if="$NEW" bs=1 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
[ "$new_magic" = "7f454c46" ] || fail "$NEW is not an ELF file"
new_machine=$(dd if="$NEW" bs=1 skip=18 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')
[ "$new_machine" = "3e00" ] || fail "$NEW is not an x86-64 ELF"
if command -v file >/dev/null 2>&1; then
    file "$NEW" | grep -q 'statically linked' || fail "$NEW is not statically linked"
fi

ssh_en=$(sed -n 's/^ssh=//p' "$CFG" 2>/dev/null | tail -n 1)
want_ssh=1
[ "$ssh_en" = "1" ] || want_ssh=0
log "ssh_enabled=$want_ssh"

# Fail before touching the daemon if success could not be verified.
probe_api >/dev/null 2>&1
[ $? -ne 2 ] || fail "no HTTP probe tool (socat/wget/curl)"
if [ "$want_ssh" = 1 ]; then
    probe_listener >/dev/null 2>&1
    [ $? -ne 2 ] || fail "no listener probe tool (netstat/ss)"
fi

if [ "$(sha256sum "$LIVE" 2>/dev/null | cut -d' ' -f1)" = "$want" ]; then
    api=no
    i=0
    while [ "$i" -lt 5 ]; do
        if probe_api >/dev/null 2>&1; then api=yes; break; fi
        i=$((i + 1))
        sleep 1
    done
    if [ "$want_ssh" = 1 ] && probe_listener >/dev/null 2>&1; then listener=yes; fi
    build=$(sed -n 's/.*"build_id":"\([^"]*\)".*/\1/p' "$APP/status.json" 2>/dev/null | head -n 1)
    if [ "$api" = yes ]; then
        log "live binary already matches staged hash and API answers; no restart"
        write_result 1 not-needed already-current
        state=committed
        exit 0
    fi
    fail "already-current binary but the daemon API is unreachable"
fi

# Give the launching SSH command time to return before the daemon stop drops
# its connection and kills every child of that session.
sleep "$START_DELAY"

stamp=$(date -u +%Y%m%dT%H%M%SZ)
backup="$BACKUP_DIR/wifi1b_daemon.pre-hotswap-$stamp"
[ ! -e "$backup" ] || backup="$backup.$$"
# Back up while the daemon is still running: a failed backup must not leave
# the unit stopped.
cp -p "$LIVE" "$backup" || fail "cannot back up $LIVE"
cmp -s "$LIVE" "$backup" || fail "backup readback mismatch: $backup"
log "backup: $backup"
record_labels
log "labels: SMACK64=${label64:-<none>} SMACK64EXEC=${labelexec:-<none>}"

state=stopping
log "stopping $SERVICE"
if ! "$SYSTEMCTL" stop "$SERVICE" >/dev/null 2>&1; then
    # Leave state=stopping so recovery reasserts the unit even if the stop
    # partly succeeded before returning an error.
    fail "cannot stop $SERVICE"
fi
state=stopped
i=0
pid=""
while [ "$i" -lt 30 ]; do
    if command -v pidof >/dev/null 2>&1; then
        pid=$(pidof wifi1b_daemon 2>/dev/null || true)
    else
        pid=$(ps w 2>/dev/null | grep '[w]ifi1b_daemon' | awk '{print $1}' | head -n 1)
    fi
    [ -z "$pid" ] && break
    i=$((i + 1))
    sleep 1
done
if [ -n "$pid" ]; then
    # Do not replace a binary that some process may still be executing.
    fail "daemon pid $pid still present after stop; not replacing"
fi

state=installing
tmp="$LIVE.hotswap.$$"
cp -f "$NEW" "$tmp" || fail "cannot stage the new binary"
cmp -s "$NEW" "$tmp" || fail "staged binary readback mismatch"
chmod 755 "$tmp" 2>/dev/null || true
chown 0:0 "$tmp" 2>/dev/null || true
apply_labels "$tmp"
sync
mv -f "$tmp" "$LIVE" || fail "cannot replace the live binary"
state=installed
log "installed: $LIVE"
sync

log "starting $SERVICE"
if ! start_service; then
    fail "start failed"
fi

i=0
while [ "$i" -lt "$VERIFY_WAIT" ]; do
    if probe_api >/dev/null 2>&1; then api=yes; fi
    if [ "$want_ssh" = 1 ] && probe_listener >/dev/null 2>&1; then listener=yes; fi
    if [ "$api" = yes ] && { [ "$want_ssh" = 0 ] || [ "$listener" = yes ]; }; then
        break
    fi
    i=$((i + 1))
    sleep 1
done

build=$(sed -n 's/.*"build_id":"\([^"]*\)".*/\1/p' "$APP/status.json" 2>/dev/null | head -n 1)
log "api=$api listener=$listener ssh_enabled=$want_ssh build_id=$build"

if [ "$api" != yes ]; then
    fail "daemon API did not answer after restart"
fi
if [ "$want_ssh" = 1 ] && [ "$listener" != yes ]; then
    fail "SSH listener did not appear after restart"
fi

write_result 1 not-needed ok
state=committed
log "=== hot-swap OK ==="
exit 0
