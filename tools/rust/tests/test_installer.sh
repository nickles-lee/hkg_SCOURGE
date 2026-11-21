#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Offline test for the generic Rust app installer/restore. Uses a fake root
# tree and stub SMACK/systemd tools; no VM and no real head unit files.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
WORK=$(mktemp -d /tmp/rust_app_test.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

FAKE="$WORK/root"
USB="$WORK/usb"
STUB="$WORK/stub"
RW="$WORK/rw"
SVC="$WORK/etc/systemd/system/test-app.service"
mkdir -p "$FAKE" "$USB/payload/bin" "$USB/payload/systemd" "$USB/payload/lib" \
         "$USB/backup_current" "$STUB" "$RW" "$WORK/etc/systemd/system"
printf 'device %s ext4 rw 0 0\n' "$FAKE" > "$WORK/mounts"
export INSTALL_IO_ROOT="$FAKE" INSTALL_IO_MOUNTS="$WORK/mounts"

install -m 0755 "$HERE/assets/install_app.sh" "$USB/run_stuff.sh"
install -m 0755 "$HERE/assets/restore_app.sh" "$USB/run_stuff_restore.sh"
install -m 0644 "$ROOT/packages/common/install_io.sh" "$USB/payload/lib/install_io.sh"
cat > "$USB/payload/app.env" <<'EOF'
APP_NAME=test-app
APP_VERSION=0.1.0
APP_BIN=test-app
APP_SERVICE=test-app.service
APP_DATA_DIR=/rw_data/apps/test-app
APP_DESCRIPTION="test app"
EOF
cat > "$USB/payload/systemd/test-app.service" <<'EOF'
[Unit]
Description=test app
[Service]
ExecStart=/rw_data/apps/test-app/bin/test-app
[Install]
WantedBy=multi-user.target
EOF
cat > "$USB/payload/bin/test-app" <<'EOF'
#!/bin/sh
echo test-app
EOF
chmod 0755 "$USB/payload/bin/test-app"
sha256sum "$USB/payload/bin/test-app" | cut -d' ' -f1 > "$USB/payload/app.sha256"
(
    cd "$USB"
    find payload -type f -print0 | sort -z | xargs -0 md5sum > manifest.md5
)

# Stubs: SMACK and systemd are absent/changed on a host.
printf '#!/bin/sh\nprintf "%%s\\n" "$*" >> "$CHSMACK_LOG"\nexit 0\n' > "$STUB/chsmack"
printf '#!/bin/sh\nif [ "${EMPTY_LABEL:-0}" = 1 ]; then exit 0; fi\nif [ "${BAD_LABEL:-0}" = 1 ]; then echo Privileged; else echo _; fi\n' > "$STUB/getfattr"
printf '#!/bin/sh\nexit 0\n' > "$STUB/systemctl"
chmod +x "$STUB/chsmack" "$STUB/getfattr" "$STUB/systemctl"

run() {
    CHSMACK_LOG="$WORK/chsmack.log" \
    APP_PATH="$STUB:/usr/bin:/bin" \
    APP_RW_DIR="$RW" \
    APP_SERVICE_DIR="$WORK/etc/systemd/system" \
    APP_SMACK_SELF="${TEST_SMACK_SELF:-_}" \
    APP_NO_SYSTEMD="${TEST_NO_SYSTEMD:-1}" \
    APP_MIN_FREE_KB=1 \
    APP_TEST_UID=0 \
    USB_DIR="$USB" \
    sh "$1"
}

echo "== first install =="
run "$USB/run_stuff.sh"
cat "$USB/run_stuff_result.txt"
grep -q 'status=OK' "$USB/run_stuff_result.txt"
grep -q 'action=installed' "$USB/run_stuff_result.txt"
grep -q 'smack_check=ok' "$USB/run_stuff_result.txt"
test -x "$RW/bin/test-app"
test -f "$SVC"
grep -q -- "-e _ $RW/bin/test-app" "$WORK/chsmack.log"
echo "first install OK"

echo "== reinstall is idempotent =="
run "$USB/run_stuff.sh"
grep -q 'status=OK' "$USB/run_stuff_result.txt"
grep -q 'action=up-to-date' "$USB/run_stuff_result.txt"
test -x "$RW/bin/test-app"
echo "reinstall OK"

echo "== restore preserves data =="
printf 'keep me\n' > "$RW/user-data.txt"
run "$USB/run_stuff_restore.sh"
cat "$USB/run_stuff_result.txt"
grep -q 'status=OK' "$USB/run_stuff_result.txt"
grep -q 'action=restored' "$USB/run_stuff_result.txt"
grep -q 'data_preserved=1' "$USB/run_stuff_result.txt"
test ! -e "$RW/bin/test-app"
test ! -e "$SVC"
test -f "$RW/user-data.txt"
echo "restore OK"

echo "== purge removes app data =="
run "$USB/run_stuff.sh"
printf 'remove me\n' > "$RW/user-data.txt"
APP_PURGE=1 run "$USB/run_stuff_restore.sh"
grep -q 'status=OK' "$USB/run_stuff_result.txt"
grep -q 'data_preserved=0' "$USB/run_stuff_result.txt"
test ! -e "$RW"
echo "purge OK"

echo "== bad SMACK label is a hard stop =="
if BAD_LABEL=1 run "$USB/run_stuff.sh"; then
    echo "installer unexpectedly accepted a bad SMACK label" >&2
    exit 1
fi
grep -q 'status=FAIL' "$USB/run_stuff_result.txt"
grep -q 'exec label readback failed' "$USB/run_stuff_result.txt"
echo "SMACK gate OK"

echo "== empty SMACK readback is a hard stop =="
if EMPTY_LABEL=1 run "$USB/run_stuff.sh"; then
    echo "installer unexpectedly accepted an empty SMACK readback" >&2
    exit 1
fi
grep -q 'status=FAIL' "$USB/run_stuff_result.txt"
grep -q 'cannot read back SMACK64EXEC' "$USB/run_stuff_result.txt"
echo "SMACK readback gate OK"

echo "== bad manifest stops before writing =="
run "$USB/run_stuff.sh" >/dev/null
printf '\ncorrupt\n' >> "$USB/payload/app.env"
if run "$USB/run_stuff.sh"; then
    echo "installer unexpectedly accepted a corrupt manifest" >&2
    exit 1
fi
grep -q 'status=FAIL' "$USB/run_stuff_result.txt"
grep -q 'manifest verification failed' "$USB/run_stuff_result.txt"
echo "manifest gate OK"

echo "== all generic installer tests passed =="
