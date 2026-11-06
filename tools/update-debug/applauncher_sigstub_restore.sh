# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#!/bin/sh
# APPLAUNCHER_SIGSTUB_RESTORE_REV=1
# Restore the stock AppLauncher signature-check bytes installed by
# applauncher_sigstub.  Rename this file to run_stuff.sh on the stick (the
# failsafe loader md5-gates the active script) and plug it in.
#
# It writes the original 6 bytes back at both offsets (guarded), respawns
# AppLauncher like the installer does, and writes run_stuff_result.txt.
set -u
PATH=/bin:/sbin:/usr/bin:/usr/sbin
USB_DIR="${USB_DIR:-$(CDPATH= cd -- "$(dirname "$0")" && pwd)}"
F="${APPLAUNCHER_BIN:-/app/bin/AppLauncher}"
OUT="$USB_DIR/run_stuff_result.txt"
RW="${SIGSTUB_RW_DIR:-/rw_data/applauncher_sigstub}"
EXPECTED_SIZE=407720
STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || echo unknown)
BACKUP="$RW/AppLauncher.restore.$STAMP"
LAUNCHER_OUT="$RW/launcher.out"
ENV_FILE="$RW/launcher.env"
CONFIG=/rw_data/applaunch-config.json

mkdir -p "$RW" 2>/dev/null || RW="$USB_DIR"
LOG="$RW/restore.log"
: > "$LOG" 2>/dev/null

log() { echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }

result() {
    {
        echo "APPLAUNCHER_SIGSTUB_RESTORE"
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

# Atomic replacement (see the installer): write a new inode and rename it over
# the target so a running AppLauncher does not cause ETXTBSY.
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
log "=== applauncher_sigstub restore start target=$F ==="

[ -f "$F" ] || fail "missing $F"
SZ=$(stat -c%s "$F" 2>/dev/null)
[ "$SZ" = "$EXPECTED_SIZE" ] || fail "unexpected size $SZ (want $EXPECTED_SIZE)"
REAL_F="$F"
cp -f "$REAL_F" "$BACKUP" 2>/dev/null || true
MD5_BEFORE=$(md5sum "$REAL_F" 2>/dev/null | cut -d' ' -f1)

was_ro=0
grep -q ' / .* ro[, ]' /proc/mounts && was_ro=1
[ "$was_ro" = 1 ] && mount -o remount,rw / 2>/dev/null

WORK="$REAL_F.sigstub_restore.$$"
rm -f "$WORK" 2>/dev/null || true
cp -f "$REAL_F" "$WORK" 2>/dev/null || {
    [ "$was_ro" = 1 ] && mount -o remount,ro / 2>/dev/null
    fail "cannot create work copy $WORK"
}
F="$WORK"

# 41 57 be 10 00 00 = \101\127\276\020\000\000
# 41 57 41 56 41 55 = \101\127\101\126\101\125
restore_one() {
    off=$1
    name=$2
    stock=$3
    cur=$(dd if="$F" bs=1 skip="$off" count=8 2>/dev/null | od -An -tx1 | tr -d ' \n')
    case "$cur" in
        "$stock")
            echo "$name=already stock" >> "$LOG"
            return 0
            ;;
        b801000000c3*)
            printf "$4" | dd of="$F" bs=1 seek="$off" conv=notrunc 2>/dev/null
            after=$(dd if="$F" bs=1 skip="$off" count=8 2>/dev/null | od -An -tx1 | tr -d ' \n')
            echo "$name=restored $cur -> $after" >> "$LOG"
            ;;
        *)
            echo "$name=UNEXPECTED $cur (want stub b801000000c3 or stock $stock)" >> "$LOG"
            return 1
            ;;
    esac
}

rc=0
restore_one 88912 FUN_00115b50 4157be1000000041 '\101\127\276\020\000\000' || rc=1
restore_one 73984 FUN_00112100 4157415641554154 '\101\127\101\126\101\125' || rc=1
sync

verify=$(dd if="$F" bs=1 skip=88912 count=6 2>/dev/null | od -An -tx1 | tr -d ' \n')
verify2=$(dd if="$F" bs=1 skip=73984 count=6 2>/dev/null | od -An -tx1 | tr -d ' \n')
if [ "$rc" != 0 ] || [ "$verify" != 4157be100000 ] || [ "$verify2" != 415741564155 ]; then
    log "verify failed (15b50=$verify 12100=$verify2); original binary untouched"
    rm -f "$WORK" 2>/dev/null || true
    [ "$was_ro" = 1 ] && mount -o remount,ro / 2>/dev/null
    fail "restore verify failed; original binary left unchanged"
fi

atomic_install "$WORK" "$REAL_F" || {
    [ "$was_ro" = 1 ] && mount -o remount,ro / 2>/dev/null
    fail "atomic restore failed; original binary left unchanged"
}
F="$REAL_F"
[ "$was_ro" = 1 ] && mount -o remount,ro / 2>/dev/null
MD5_AFTER=$(md5sum "$F" 2>/dev/null | cut -d' ' -f1)
log "restore verified md5_after=$MD5_AFTER"

# ---- respawn (same procedure as the installer) -------------------------
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
    restart="not-running (stock binary starts at next boot)"
else
    : > "$ENV_FILE" 2>/dev/null
    tr '\0' '\n' < "/proc/$launcher_pid/environ" > "$RW/launcher.environ" 2>/dev/null || true
    for var in DBUS_SESSION_BUS_ADDRESS XDG_RUNTIME_DIR WAYLAND_DISPLAY \
               QT_QPA_PLATFORM LD_LIBRARY_PATH LD_PRELOAD QV4_MM_MAX_CHUNK_SIZE \
               QV4_NO_SSA QV4_FORCE_INTERPRETER; do
        val=$(grep "^$var=" "$RW/launcher.environ" 2>/dev/null | head -n 1 | cut -d= -f2-)
        [ -n "$val" ] && printf '%s=%s\n' "$var" "$val" >> "$ENV_FILE"
    done

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
        kill -KILL "$launcher_pid" 2>/dev/null || true
        i=0
        while [ -d "/proc/$launcher_pid" ] && [ "$i" -lt 5 ]; do sleep 1; i=$((i + 1)); done
    fi

    : > "$LAUNCHER_OUT" 2>/dev/null || true
    (
        cd /app/bin || exit 1
        while IFS= read -r line; do
            export "$line"
        done < "$ENV_FILE"
        exec /app/bin/AppLauncher 3
    ) >> "$LAUNCHER_OUT" 2>&1 &

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

    if [ "$config_existed" = 1 ]; then
        [ "$config_saved" = 1 ] && cp -f "$RW/applaunch-config.before-respawn" "$CONFIG" 2>/dev/null || true
    else
        rm -f "$CONFIG" 2>/dev/null || true
    fi

    if [ "$bus_ok" = 1 ]; then
        restart="respawned (bus verified)"
    elif command -v dbus-send >/dev/null 2>&1; then
        restart="respawned (bus NOT re-owned) REBOOT_REQUIRED"
    else
        restart="respawned (bus verify unavailable) REBOOT_REQUIRED if UI missing"
    fi
    log "restart=$restart"
fi

{
    echo "APPLAUNCHER_SIGSTUB_RESTORE"
    echo "time=$(date -u 2>/dev/null)"
    echo "usb=$USB_DIR"
    echo "target=$F"
    echo "uid=$(id -u 2>/dev/null)"
    echo "size=$SZ"
    echo "md5_before=$MD5_BEFORE"
    echo "md5_after=$MD5_AFTER"
    echo "backup=$BACKUP"
    b=$(dd if="$F" bs=1 skip=88912 count=6 2>/dev/null | od -An -tx1 | tr -d ' \n')
    echo "FUN_00115b50=$b (want 4157be100000)"
    b=$(dd if="$F" bs=1 skip=73984 count=6 2>/dev/null | od -An -tx1 | tr -d ' \n')
    echo "FUN_00112100=$b (want 415741564155)"
    echo "launcher_pid_before=${launcher_pid:-none}"
    echo "restart=$restart"
    echo "log=$LOG"
    echo "status=OK"
} > "$OUT" 2>&1
cp -f "$OUT" "$RW/result.txt" 2>/dev/null || true
sync
exit 0
