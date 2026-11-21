#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Mode V probe: install and exercise the host-built static-musl Rust sample
# daemon package. Research boot has no systemd, so the daemon is started
# directly; the unit file is installed and checked but not started by PID 1.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/app/bin

APP=scourge-statusd
BIN=/rw_data/apps/$APP/bin/$APP
SERVICE=/etc/systemd/system/$APP.service
STICK=/media/rust

fail() {
    echo "HARNESS_FAIL $1 ($2)"
    [ ! -f /tmp/rust-install.log ] || tail -60 /tmp/rust-install.log
    [ ! -f /tmp/rust-daemon.log ] || tail -30 /tmp/rust-daemon.log
    exit 1
}

mount_stick() {
    mkdir -p "$STICK"
    for dev in /dev/sd?; do
        [ -b "$dev" ] || continue
        mount -o rw "$dev" "$STICK" 2>/dev/null || continue
        if [ -f "$STICK/payload/app.env" ]; then
            PAY=$dev
            return 0
        fi
        umount "$STICK" 2>/dev/null || true
    done
    return 1
}

modprobe ehci-pci 2>/dev/null || true
modprobe usb-storage 2>/dev/null || true
modprobe sd_mod 2>/dev/null || true
ip link set lo up 2>/dev/null || ifconfig lo up 2>/dev/null || true
sleep 4

PAY=
mount_stick || { echo 'HARNESS_BLOCKED rust-payload missing'; exit 77; }

echo "== install =="
USB_DIR="$STICK" \
APP_SMACK_SELF=_ \
APP_NO_SYSTEMD=1 \
APP_PATH=/bin:/sbin:/usr/bin:/usr/sbin \
sh "$STICK/run_stuff.sh" >/tmp/rust-install.log 2>&1
INSTALL_RC=$?
cat "$STICK/run_stuff_result.txt" 2>/dev/null || true
[ "$INSTALL_RC" -eq 0 ] || fail rust-install "installer rc=$INSTALL_RC"
grep -q 'status=OK' "$STICK/run_stuff_result.txt" ||
    fail rust-install "installer did not report status=OK"
grep -q 'action=installed' "$STICK/run_stuff_result.txt" ||
    fail rust-install "unexpected install action"
grep -q "smack_check=ok" "$STICK/run_stuff_result.txt" ||
    fail rust-install "SMACK label readback was not ok"
[ -x "$BIN" ] || fail rust-install "missing $BIN"
[ -f "$SERVICE" ] || fail rust-install "missing $SERVICE"
echo 'HARNESS_CHECK rust-install PASS'

# The deployed app must not depend on the stick.
umount "$STICK" 2>/dev/null || fail rust-install "cannot unmount the stick"

echo "== static linking =="
if ldd "$BIN" 2>&1 | grep -Eqi 'not a dynamic executable|statically linked'; then
    :
else
    fail rust-static "ldd does not report a static binary"
fi
echo 'HARNESS_CHECK rust-static PASS'

echo "== daemon =="
rm -f "/rw_data/apps/$APP/status.json"
"$BIN" --listen 127.0.0.1:18088 --status "/rw_data/apps/$APP/status.json" \
    >/tmp/rust-daemon.log 2>&1 &
PID=$!
sleep 1
kill -0 "$PID" 2>/dev/null || fail rust-daemon "daemon exited early"
echo 'HARNESS_CHECK rust-daemon PASS'

echo "== HTTP status =="
RESP=$(printf 'GET /status HTTP/1.0\r\n\r\n' | socat - TCP:127.0.0.1:18088 2>/dev/null || true)
echo "$RESP"
printf '%s' "$RESP" | grep -q '"app":"scourge-statusd"' ||
    fail rust-status "status response missing app"
printf '%s' "$RESP" | grep -q '"state":"running"' ||
    fail rust-status "status response missing running state"
printf '%s' "$RESP" | grep -q '"static":true' ||
    fail rust-status "daemon does not report static=true"
grep -q 'libc\.so\|ld-linux' "/proc/$PID/maps" &&
    fail rust-status "daemon has a dynamic libc mapping"
echo 'HARNESS_CHECK rust-status PASS'

echo "== SMACK =="
LABEL=$(cat "/proc/$PID/attr/current" 2>/dev/null | tr -d '\r\n')
[ "$LABEL" = "_" ] || fail rust-smack "process label is '${LABEL:-none}', want '_'"
ATTR=$(getfattr -n security.SMACK64EXEC --only-values "$BIN" 2>/dev/null || true)
[ "$ATTR" = "_" ] || fail rust-smack "binary exec label is '${ATTR:-none}', want '_'"
echo 'HARNESS_CHECK rust-smack PASS'

echo "== SIGTERM shutdown =="
kill -TERM "$PID" 2>/dev/null || fail rust-shutdown "cannot signal the daemon"
wait "$PID"
SHUTDOWN_RC=$?
[ "$SHUTDOWN_RC" -eq 0 ] || fail rust-shutdown "daemon exited rc=$SHUTDOWN_RC"
grep -Eq '"state"[[:space:]]*:[[:space:]]*"stopped"' "/rw_data/apps/$APP/status.json" ||
    fail rust-shutdown "status mirror was not updated to stopped"
echo 'HARNESS_CHECK rust-shutdown PASS'

PAY=
mount_stick || fail rust-idempotent "lost the payload stick"
echo "== reinstall is idempotent =="
USB_DIR="$STICK" APP_SMACK_SELF=_ APP_NO_SYSTEMD=1 \
    sh "$STICK/run_stuff.sh" >/tmp/rust-install2.log 2>&1 ||
    fail rust-idempotent "second install failed"
cat "$STICK/run_stuff_result.txt"
grep -q 'status=OK' "$STICK/run_stuff_result.txt" ||
    fail rust-idempotent "second install did not report OK"
grep -q 'action=up-to-date' "$STICK/run_stuff_result.txt" ||
    fail rust-idempotent "second install was not a no-op"
echo 'HARNESS_CHECK rust-idempotent PASS'

echo "== restore preserves data =="
printf 'keep me\n' > "/rw_data/apps/$APP/user-data.txt"
USB_DIR="$STICK" APP_NO_SYSTEMD=1 \
    sh "$STICK/run_stuff_restore.sh" >/tmp/rust-restore.log 2>&1 ||
    fail rust-restore "restore failed"
cat "$STICK/run_stuff_result.txt"
grep -q 'status=OK' "$STICK/run_stuff_result.txt" ||
    fail rust-restore "restore did not report OK"
grep -q 'data_preserved=1' "$STICK/run_stuff_result.txt" ||
    fail rust-restore "restore did not preserve app data"
[ ! -e "$BIN" ] || fail rust-restore "binary still present"
[ ! -e "$SERVICE" ] || fail rust-restore "service unit still present"
[ -f "/rw_data/apps/$APP/user-data.txt" ] ||
    fail rust-restore "user data was removed"
echo 'HARNESS_CHECK rust-restore PASS'

umount "$STICK" 2>/dev/null || true
echo "== rust-app finished =="
