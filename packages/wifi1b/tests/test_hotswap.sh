#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Fixture-only tests for scripts/hotswap_daemon.sh.
# No host service, radio, network or real /rw_data path is touched.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SCRIPT="$HERE/scripts/hotswap_daemon.sh"
DAEMON="$HERE/src/wifi1b_daemon"
[ -x "$DAEMON" ] || { echo "missing $DAEMON (run make payload first)" >&2; exit 1; }
TMP=$(mktemp -d /tmp/opencode/wifi1b-hotswap.XXXXXX)
trap 'rm -rf "$TMP"' EXIT INT TERM HUP

make_stubs() {
    bin=$1
    mkdir -p "$bin"
    cat > "$bin/systemctl" <<'EOS'
#!/bin/sh
echo "$*" >> "$STATE/systemctl.log"
case "$1" in
    stop) rm -f "$STATE/running" "$STATE/api" "$STATE/listener"; exit 0 ;;
    start)
        touch "$STATE/running"
        [ -f "$STATE/fail_start" ] && exit 1
        [ -f "$STATE/no_api" ] || touch "$STATE/api"
        [ -f "$STATE/no_listener" ] || touch "$STATE/listener"
        exit 0 ;;
esac
exit 0
EOS
    cat > "$bin/socat" <<'EOS'
#!/bin/sh
[ -f "$STATE/api" ] || exit 1
printf 'HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}'
EOS
    cat > "$bin/netstat" <<'EOS'
#!/bin/sh
if [ -f "$STATE/listener" ]; then
    echo "tcp 0 0 0.0.0.0:${WIFI1B_HOTSWAP_SSH_PORT:-2224} 0.0.0.0:* LISTEN"
fi
EOS
    cat > "$bin/pidof" <<'EOS'
#!/bin/sh
exit 1
EOS
    cat > "$bin/getfattr" <<'EOS'
#!/bin/sh
echo "$*" >> "$STATE/getfattr.log"
case "$*" in
    *SMACK64EXEC*) echo Privileged ;;
    *SMACK64*) echo _ ;;
esac
exit 0
EOS
    cat > "$bin/chsmack" <<'EOS'
#!/bin/sh
echo "$*" >> "$STATE/chsmack.log"
exit 0
EOS
    chmod 755 "$bin"/*
}

new_case() {
    name=$1
    ssh_en=${2:-1}
    CASE="$TMP/$name"
    rm -rf "$CASE"
    mkdir -p "$CASE/app/backup" "$CASE/bin" "$CASE/state"
    : > "$CASE/state/systemctl.log"
    : > "$CASE/state/getfattr.log"
    : > "$CASE/state/chsmack.log"
    cp "$DAEMON" "$CASE/app/wifi1b_daemon.new"
    cp /bin/true "$CASE/app/wifi1b_daemon"
    sha256sum "$CASE/app/wifi1b_daemon.new" | cut -d' ' -f1 \
        > "$CASE/app/wifi1b_daemon.new.sha256"
    printf 'ssh=%s\n' "$ssh_en" > "$CASE/app/config.conf"
    make_stubs "$CASE/bin"
}

run_case() {
    CASE_NAME=$1
    use_path=${2:-$CASE/bin:/usr/bin:/bin}
    env STATE="$CASE/state" \
        WIFI1B_HOTSWAP_APP="$CASE/app" \
        WIFI1B_HOTSWAP_SERVICE=test-wifi1b.service \
        WIFI1B_HOTSWAP_SYSTEMCTL="$CASE/bin/systemctl" \
        WIFI1B_HOTSWAP_API_PORT=18093 \
        WIFI1B_HOTSWAP_SSH_PORT=18224 \
        WIFI1B_HOTSWAP_START_DELAY=0 \
        WIFI1B_HOTSWAP_VERIFY_WAIT=3 \
        WIFI1B_HOTSWAP_RECOVERY_WAIT=2 \
        WIFI1B_HOTSWAP_PATH="$use_path" \
        sh "$SCRIPT" >/dev/null 2>&1 || true
    RESULT="$CASE/app/hotswap.result"
    [ -f "$RESULT" ] || { echo "FAIL: $CASE_NAME wrote no result"; exit 1; }
}

expect() {
    name=$1
    shift
    "$@" || { echo "FAIL: $name"; echo "--- result"; cat "$RESULT"; exit 1; }
}

# A. Normal update, API and listener verified.
new_case success 1
run_case success
expect 'success ok=1' grep -qx 'ok=1' "$RESULT"
expect 'success rollback=not-needed' grep -qx 'rollback=not-needed' "$RESULT"
expect 'success listener=yes' grep -qx 'listener=yes' "$RESULT"
[ "$(sha256sum "$CASE/app/wifi1b_daemon" | cut -d' ' -f1)" = \
  "$(cut -d' ' -f1 "$CASE/app/wifi1b_daemon.new.sha256")" ]
ls "$CASE/app/backup"/wifi1b_daemon.pre-hotswap-* >/dev/null

# B. Already current with a live API; no restart.
new_case current 1
cp "$CASE/app/wifi1b_daemon.new" "$CASE/app/wifi1b_daemon"
touch "$CASE/state/api"
run_case current
expect 'current ok=1' grep -qx 'ok=1' "$RESULT"
expect 'current reason' grep -qx 'reason=already-current' "$RESULT"
! grep -q 'stop' "$CASE/state/systemctl.log"

# C. Already current but API down must not report success.
new_case currentdown 1
cp "$CASE/app/wifi1b_daemon.new" "$CASE/app/wifi1b_daemon"
run_case currentdown
expect 'currentdown ok=0' grep -qx 'ok=0' "$RESULT"
expect 'currentdown reason' grep -q 'already-current binary but the daemon API' "$RESULT"
! grep -q 'stop' "$CASE/state/systemctl.log"

# D. Backup failure happens before the unit is stopped.
new_case backupfail 1
chmod 500 "$CASE/app/backup"
run_case backupfail
expect 'backupfail ok=0' grep -qx 'ok=0' "$RESULT"
expect 'backupfail not-needed' grep -qx 'rollback=not-needed' "$RESULT"
! grep -q 'stop' "$CASE/state/systemctl.log"
chmod 700 "$CASE/app/backup"

# E. Install failure after stop restarts the unchanged old daemon.
new_case installfail 1
old_sha=$(sha256sum "$CASE/app/wifi1b_daemon" | cut -d' ' -f1)
cat > "$CASE/bin/cp" <<'EOS'
#!/bin/sh
case "$*" in
    *wifi1b_daemon.new*wifi1b_daemon.hotswap*) exit 1 ;;
esac
exec /usr/bin/cp "$@"
EOS
chmod 755 "$CASE/bin/cp"
run_case installfail
expect 'installfail ok=0' grep -qx 'ok=0' "$RESULT"
expect 'installfail restarted' grep -qx 'rollback=service-restarted' "$RESULT"
grep -q 'start' "$CASE/state/systemctl.log"
[ "$(sha256sum "$CASE/app/wifi1b_daemon" | cut -d' ' -f1)" = "$old_sha" ]

# F. Missing API after restart restores the backup and its recorded labels.
new_case noapi 1
old_sha=$(sha256sum "$CASE/app/wifi1b_daemon" | cut -d' ' -f1)
touch "$CASE/state/no_api"
run_case noapi
expect 'noapi ok=0' grep -qx 'ok=0' "$RESULT"
expect 'noapi restored' grep -qx 'rollback=restored' "$RESULT"
[ "$(sha256sum "$CASE/app/wifi1b_daemon" | cut -d' ' -f1)" = "$old_sha" ]
grep -q -- '-a _' "$CASE/state/chsmack.log"
grep -q -- '-e Privileged' "$CASE/state/chsmack.log"

# G. ssh=0 still requires a verified API response.
new_case noapisshoff 0
touch "$CASE/state/no_api"
run_case noapisshoff
expect 'ssh0 ok=0' grep -qx 'ok=0' "$RESULT"
expect 'ssh0 ssh_enabled=0' grep -qx 'ssh_enabled=0' "$RESULT"
expect 'ssh0 restored' grep -qx 'rollback=restored' "$RESULT"

# H. Missing probe tool fails closed before the unit is stopped.
new_case noprobe 1
mkdir -p "$CASE/minbin"
make_stubs "$CASE/minbin"
rm -f "$CASE/minbin/socat"
for t in date cat tr cut sed grep head tail dd od sha256sum cp mv chmod chown \
         sync rm mkdir sleep ps awk flock; do
    [ -e "$CASE/minbin/$t" ] || ln -s "$(command -v "$t")" "$CASE/minbin/$t"
done
run_case noprobe "$CASE/minbin"
expect 'noprobe ok=0' grep -qx 'ok=0' "$RESULT"
expect 'noprobe reason' grep -q 'no HTTP probe tool' "$RESULT"
! grep -q 'stop' "$CASE/state/systemctl.log"

echo "PASS: hot-swap success, already-current, backup/install failure, no-API rollback, ssh=0 and missing-probe gates"
