# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#!/bin/sh
# APPLAUNCHER_SIGSTUB_REV=1
# applauncher_sigstub — disable AppLauncher's .sign verification on the
# Ioniq 5 Gen5W head unit (260128), deployed through the initial_root USB
# failsafe (run_stuff.sh at the stick root, run as root).
#
# What it does:
#   Patches two gate functions in the installed /app/bin/AppLauncher by
#   writing a patched copy to a new inode in the same directory and renaming
#   it over the target (permissions and SMACK label are preserved). This
#   avoids ETXTBSY while AppLauncher is running; the old process keeps the
#   old inode until the respawn.
#
#     B8 01 00 00 00 C3 = mov eax,1; ret  (return true = check passed)
#
#     FUN_00115b50  off 88912  readSection gate: libelf .sign extraction
#                               -> signCheck (SHA256 + RSA_verify vs
#                               /tmp/app_normal.key); its bool gates every
#                               verified QProcess::start
#     FUN_00112100  off 73984  navi (appId 0x1e) file CheckSign vs
#                               /tmp/navi_public.key
#
#   After patching it respawns AppLauncher in-session (nothing respawns
#   it: ApplicationLauncher.service is Type=oneshot) by capturing the
#   running launcher's environment, SIGTERM/SIGKILL-ing it and re-execing
#   /app/bin/AppLauncher 3 with that environment.  While the new launcher
#   starts, /rw_data/applaunch-config.json is temporarily replaced with an
#   empty group list so the respawn does not duplicate-spawn the boot
#   group; the original config is restored immediately afterwards.
#
#   It does NOT flash firmware, does not touch the updater, bootloader,
#   partitions or driving functions, and writes nothing outside
#   /app/bin/AppLauncher and /rw_data/applauncher_sigstub/ (plus the
#   result/log files on the USB stick).
#
# Notes:
#   - On-demand request.launch spawns already skip the check on the stock
#     binary (record flag +0x1c = 1, FUN_00123990); only startup-group /
#     preload spawns verify.  The stubs remove the check from that path.
#   - Idempotent: re-running reports already-patched and exits OK.
#   - The stock reference copy in backup/AppLauncher.stock is only a
#     fallback; byte-restore uses the embedded stock bytes.
#
# Restore:
#   Rename run_stuff_restore.sh to run_stuff.sh (the failsafe loader
#   md5-gates the active script) and plug the stick in again.
#
# Host/offline testing:
#   APPLAUNCHER_BIN=<copy> APPLAUNCHER_NO_RESPAWN=1 SIGSTUB_RW_DIR=<dir> \
#     sh scripts/run_stuff.sh
set -u
PATH=/bin:/sbin:/usr/bin:/usr/sbin
USB_DIR="${USB_DIR:-$(CDPATH= cd -- "$(dirname "$0")" && pwd)}"
F="${APPLAUNCHER_BIN:-/app/bin/AppLauncher}"
OUT="$USB_DIR/run_stuff_result.txt"
RW="${SIGSTUB_RW_DIR:-/rw_data/applauncher_sigstub}"
LOG="$RW/install.log"
EXPECTED_SIZE=407720
STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || echo unknown)
BACKUP="$RW/AppLauncher.$STAMP"
USB_BACKUP="$USB_DIR/AppLauncher.before-sigstub"
LAUNCHER_OUT="$RW/launcher.out"
ENV_FILE="$RW/launcher.env"
CONFIG=/rw_data/applaunch-config.json

mkdir -p "$RW" 2>/dev/null || RW="$USB_DIR"
mkdir -p "$RW" 2>/dev/null || true
LOG="$RW/install.log"
: > "$LOG" 2>/dev/null

log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }

result() {
    {
        echo "APPLAUNCHER_SIGSTUB"
        echo "time=$(date -u 2>/dev/null)"
        echo "usb=$USB_DIR"
        echo "target=$F"
        echo "uid=$(id -u 2>/dev/null)"
        for line in "$@"; do
            echo "$line"
        done
    } > "$OUT" 2>&1
    cp -f "$OUT" "$RW/result.txt" 2>/dev/null || true
}

fail() {
    log "FAIL: $*"
    result "status=FAIL" "error=$*"
    exit 1
}

# Replace dst atomically with src. The patched image is written to a new inode
# and renamed over the target, so the in-place write never hits the running
# AppLauncher's ETXTBSY protection. The running process keeps the old inode
# until the respawn; the path (and the next boot) sees the new one.
atomic_install() {
    src=$1
    dst=$2
    mode=$(stat -c%a "$dst" 2>/dev/null)
    owner=$(stat -c%u:%g "$dst" 2>/dev/null)
    label=$(getfattr -n security.SMACK64 --only-values "$dst" 2>/dev/null || true)
    [ -n "$mode" ] && chmod "$mode" "$src" 2>/dev/null || true
    [ -n "$owner" ] && chown "$owner" "$src" 2>/dev/null || true
    [ -n "$label" ] && setfattr -n security.SMACK64 -v "$label" "$src" 2>/dev/null || true
    mv -f "$src" "$dst" 2>>"$LOG" || { rm -f "$src"; return 1; }
    sync
    return 0
}

mount -o remount,rw "$USB_DIR" 2>/dev/null || true
log "=== applauncher_sigstub start usb=$USB_DIR target=$F ==="

[ -f "$F" ] || fail "missing $F"
SZ=$(stat -c%s "$F" 2>/dev/null)
[ "$SZ" = "$EXPECTED_SIZE" ] || fail "unexpected size $SZ (want $EXPECTED_SIZE)"
REAL_F="$F"

mkdir -p "$RW" 2>/dev/null || true
cp -f "$REAL_F" "$BACKUP" 2>/dev/null || fail "backup to $BACKUP failed"
cp -f "$REAL_F" "$USB_BACKUP" 2>/dev/null || true
MD5_BEFORE=$(md5sum "$REAL_F" 2>/dev/null | cut -d' ' -f1)
log "backup=$BACKUP md5_before=$MD5_BEFORE"

# ---- patch ------------------------------------------------------------
was_ro=0
grep -q ' / .* ro[, ]' /proc/mounts && was_ro=1
[ "$was_ro" = 1 ] && mount -o remount,rw / 2>/dev/null

# Work on a copy in the same directory so the final rename is atomic.
WORK="$REAL_F.sigstub.$$"
rm -f "$WORK" 2>/dev/null || true
cp -f "$REAL_F" "$WORK" 2>/dev/null || {
    [ "$was_ro" = 1 ] && mount -o remount,ro / 2>/dev/null
    fail "cannot create work copy $WORK"
}
F="$WORK"

patch_one() {
    off=$1
    name=$2
    expect=$3
    cur=$(dd if="$F" bs=1 skip="$off" count=8 2>/dev/null | od -An -tx1 | tr -d ' \n')
    case "$cur" in
        b801000000c3*)
            echo "$name=already (b801000000c3)" >> "$LOG"
            return 0
            ;;
        "$expect")
            printf '\270\001\000\000\000\303' | dd of="$F" bs=1 seek="$off" conv=notrunc 2>/dev/null
            after=$(dd if="$F" bs=1 skip="$off" count=8 2>/dev/null | od -An -tx1 | tr -d ' \n')
            echo "$name=patched $cur -> $after" >> "$LOG"
            ;;
        *)
            echo "$name=UNEXPECTED $cur (want $expect)" >> "$LOG"
            return 1
            ;;
    esac
}

rc=0
patch_one 88912 FUN_00115b50 4157be1000000041 || rc=1
patch_one 73984 FUN_00112100 4157415641554154 || rc=1
sync

# Verify the patched bytes before touching the running process.
verify=$(dd if="$F" bs=1 skip=88912 count=6 2>/dev/null | od -An -tx1 | tr -d ' \n')
verify2=$(dd if="$F" bs=1 skip=73984 count=6 2>/dev/null | od -An -tx1 | tr -d ' \n')
if [ "$rc" != 0 ] || [ "$verify" != b801000000c3 ] || [ "$verify2" != b801000000c3 ]; then
    log "verify failed (15b50=$verify 12100=$verify2); original binary untouched"
    rm -f "$WORK" 2>/dev/null || true
    [ "$was_ro" = 1 ] && mount -o remount,ro / 2>/dev/null
    fail "patch verify failed; original binary left unchanged"
fi

MD5_AFTER=$(md5sum "$WORK" 2>/dev/null | cut -d' ' -f1)
atomic_install "$WORK" "$REAL_F" || {
    [ "$was_ro" = 1 ] && mount -o remount,ro / 2>/dev/null
    fail "atomic install failed; original binary left unchanged"
}
F="$REAL_F"
[ "$was_ro" = 1 ] && mount -o remount,ro / 2>/dev/null
MD5_AFTER=$(md5sum "$F" 2>/dev/null | cut -d' ' -f1)
log "patch verified md5_after=$MD5_AFTER"

# ---- respawn ----------------------------------------------------------
restart="skipped"
launcher_pid=""
for p in /proc/[0-9]*; do
    [ -r "$p/comm" ] || continue
    if [ "$(cat "$p/comm" 2>/dev/null)" = "AppLauncher" ]; then
        launcher_pid="${p#/proc/}"
        break
    fi
done

if [ "${APPLAUNCHER_NO_RESPAWN:-0}" = 1 ]; then
    restart="skipped (APPLAUNCHER_NO_RESPAWN=1)"
elif [ -z "$launcher_pid" ]; then
    restart="not-running (patched binary starts at next boot)"
else
    # Capture the running launcher's environment.
    : > "$ENV_FILE" 2>/dev/null
    tr '\0' '\n' < "/proc/$launcher_pid/environ" > "$RW/launcher.environ" 2>/dev/null || true
    for var in DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR WAYLAND_DISPLAY \
               QT_QPA_PLATFORM LD_LIBRARY_PATH LD_PRELOAD QV4_MM_MAX_CHUNK_SIZE \
               QV4_NO_SSA QV4_FORCE_INTERPRETER; do
        val=$(grep "^$var=" "$RW/launcher.environ" 2>/dev/null | head -n 1 | cut -d= -f2-)
        [ -n "$val" ] && printf '%s=%s\n' "$var" "$val" >> "$ENV_FILE"
    done
    log "captured $(wc -l < "$ENV_FILE" 2>/dev/null || echo 0) env vars from pid $launcher_pid"

    # Temporarily empty the startup groups so the respawned launcher does
    # not duplicate-spawn the boot group while the old instances are still
    # alive (they are orphaned, not killed, when the launcher dies).
    config_saved=0
    config_existed=0
    if [ -f "$CONFIG" ]; then
        config_existed=1
        if cp -f "$CONFIG" "$RW/applaunch-config.before-respawn" 2>/dev/null; then
            config_saved=1
        fi
    fi
    printf '{}\n' > "$CONFIG" 2>/dev/null || true

    kill -TERM "$launcher_pid" 2>/dev/null || true
    i=0
    while [ -d "/proc/$launcher_pid" ] && [ "$i" -lt 10 ]; do sleep 1; i=$((i + 1)); done
    if [ -d "/proc/$launcher_pid" ]; then
        log "SIGTERM did not exit in ${i}s; SIGKILL"
        kill -KILL "$launcher_pid" 2>/dev/null || true
        i=0
        while [ -d "/proc/$launcher_pid" ] && [ "$i" -lt 5 ]; do sleep 1; i=$((i + 1)); done
    fi
    log "old launcher gone (pid $launcher_pid)"

    : > "$LAUNCHER_OUT" 2>/dev/null || true
    (
        cd /app/bin || exit 1
        while IFS= read -r line; do
            export "$line"
        done < "$ENV_FILE"
        exec /app/bin/AppLauncher 3
    ) >> "$LAUNCHER_OUT" 2>&1 &
    log "respawned /app/bin/AppLauncher 3"

    bus_ok=0
    if command -v dbus-send >/dev/null 2>&1; then
        sess=$(grep '^DBUS_SESSION_BUS_ADDRESS=' "$ENV_FILE" 2>/dev/null | head -n 1 | cut -d= -f2-)
        if [ -n "$sess" ]; then
            i=0
            while [ "$i" -lt 15 ]; do
                if DBUS_SESSION_BUS_ADDRESS="$sess" dbus-send --session --print-reply \
                        --dest=org.freedesktop.DBus / org.freedesktop.DBus.NameHasOwner \
                        string:com.lge.avn.applauncher 2>/dev/null | grep -q 'boolean true'; then
                    bus_ok=1
                    break
                fi
                sleep 1
                i=$((i + 1))
            done
        fi
    fi

    # Restore the startup-group config in every outcome.
    if [ "$config_existed" = 1 ]; then
        [ "$config_saved" = 1 ] && cp -f "$RW/applaunch-config.before-respawn" "$CONFIG" 2>/dev/null || true
    else
        rm -f "$CONFIG" 2>/dev/null || true
    fi

    if [ "$bus_ok" = 1 ]; then
        restart="respawned (bus verified)"
    elif command -v dbus-send >/dev/null 2>&1; then
        restart="respawned (bus NOT re-owned) REBOOT_REQUIRED"
        log "respawn did not re-own com.lge.avn.applauncher"
    else
        restart="respawned (bus verify unavailable) REBOOT_REQUIRED if UI missing"
    fi
    log "restart=$restart"
fi

{
    echo "APPLAUNCHER_SIGSTUB"
    echo "time=$(date -u 2>/dev/null)"
    echo "usb=$USB_DIR"
    echo "target=$F"
    echo "uid=$(id -u 2>/dev/null)"
    echo "size=$SZ"
    echo "md5_before=$MD5_BEFORE"
    echo "md5_after=$MD5_AFTER"
    echo "backup=$BACKUP"
    echo "usb_backup=$USB_BACKUP"
    b=$(dd if="$F" bs=1 skip=88912 count=6 2>/dev/null | od -An -tx1 | tr -d ' \n')
    echo "FUN_00115b50=$b (want b801000000c3)"
    b=$(dd if="$F" bs=1 skip=73984 count=6 2>/dev/null | od -An -tx1 | tr -d ' \n')
    echo "FUN_00112100=$b (want b801000000c3)"
    echo "launcher_pid_before=${launcher_pid:-none}"
    echo "restart=$restart"
    echo "log=$LOG"
    echo "status=OK"
} > "$OUT" 2>&1
cp -f "$OUT" "$RW/result.txt" 2>/dev/null || true
cp -f "$LOG" "$USB_DIR/applauncher_sigstub.log" 2>/dev/null || true
log "=== applauncher_sigstub done restart=$restart ==="
sync
exit 0
