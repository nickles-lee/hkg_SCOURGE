#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# both_copies_v1 — patch BOTH AppUpgrade engine copies + install an update
# watcher + collect diagnostics.
#
# Stages:
#   1. installed /app/bin/AppUpgrade: verify/apply the five engine stubs
#      (no-op if already the five-engine-stub binary, md5 55e522e3...)
#   2. update drive: verify/swap the wrapper + patch .ver row 341142
#      (no-op if already the patched wrapper, md5 f730490b...)
#   3. install + start the update watcher: logs ps transitions and captures
#      every distinct /tmp/AppUpgrade to /rw_data during the update attempt
#      (45-minute window, pid-file dedupe)
#   4. collect diagnostics to the stick (snapshot, engine copies, AppUpgrade
#      logs, DLT tar, watcher log)
#
# Run it BEFORE the update attempt (patch verify + watcher start), and run it
# AGAIN after the attempt (idempotent re-verify + collects the watcher log
# and the post-update state).
#
# Optional marker file next to this script:
#   REVERT   restore the drive's stock wrapper + .ver from
#            wrapper_swap_backup/ (created by the swap stage)
#
# Payload files (same directory as this script):
#   AppUpgrade.patched.te2   the patched wrapper (10,803,676 B)
#
# Nothing outside /app/bin/AppUpgrade, the detected update-drive mount,
# /rw_data, and the payload stick is written.
#
# Test hooks: APPUPGRADE_OVERRIDE, DRIVE_OVERRIDE, RW_OVERRIDE.

set -u
PATH=/bin:/sbin:/usr/bin:/usr/sbin
USB_DIR="${USB_DIR:-$(dirname "$0")}"
RESULT="$USB_DIR/both_copies_result.txt"
RW="${RW_OVERRIDE:-/rw_data/both_copies}"
LOG="$RW/log"
STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || echo unknown)

APPUPGRADE="${APPUPGRADE_OVERRIDE:-/app/bin/AppUpgrade}"
INSTALLED_SIZE=10787256
INSTALLED_PATCHED_MD5=55e522e37e85b9a7c599f9f17fc1a580

PATCHED_WRAPPER="$USB_DIR/AppUpgrade.patched.te2"
PATCHED_WRAPPER_MD5=f730490b9a9f6d3499b9ba59070d63e8
STOCK_WRAPPER_MD5=15a33d05b52ffa3d2ada5d44b4f37253
WRAPPER_SIZE=10803676
ROW_STOCK="IONIQ5_EU|AppUpgrade|341142|609028602|10803676|1"
ROW_PATCHED="IONIQ5_EU|AppUpgrade|341142|414600538|10803676|1"
ROW_KEY='IONIQ5_EU|AppUpgrade|341142|'

mkdir -p "$RW" 2>/dev/null || true
: > "$LOG" 2>/dev/null
log(){ echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }
md5_of(){ md5sum "$1" 2>/dev/null | cut -d' ' -f1; }

finish(){
    {
        echo "BOTH_COPIES_V1"
        echo "time=$(date -u 2>/dev/null)"
        echo "usb=$USB_DIR"
        echo "uid=$(id -u 2>/dev/null)"
        echo "uname=$(uname -a 2>/dev/null)"
        cat "$LOG" 2>/dev/null
        echo "status=$1"
    } > "$RESULT" 2>&1
    cp -f "$RESULT" "$RW/result.txt" 2>/dev/null
    sync
    exit 0
}

# ============================================================
# Stage 1: installed /app/bin/AppUpgrade — five engine stubs (SCOMO four + FUMO)
# ============================================================
stage_installed(){
    [ -f "$APPUPGRADE" ] || { log "installed: missing $APPUPGRADE"; return 1; }
    SZ=$(stat -c%s "$APPUPGRADE" 2>/dev/null)
    [ "$SZ" = "$INSTALLED_SIZE" ] \
        || { log "installed: unexpected size $SZ"; return 1; }
    M=$(md5_of "$APPUPGRADE")
    log "installed: md5=$M"
    if [ "$M" = "$INSTALLED_PATCHED_MD5" ]; then
        log "installed: already five-engine-stub"
        return 0
    fi

    # backup once before patching
    mkdir -p "$RW" 2>/dev/null
    cp -f "$APPUPGRADE" "$RW/AppUpgrade.before.$STAMP" 2>/dev/null

    was_ro=0
    grep -q ' / .* ro[, ]' /proc/mounts && was_ro=1
    [ "$was_ro" = 1 ] && mount -o remount,rw / 2>/dev/null

    rc=0
    patch_one(){
        off=$1; name=$2; expect=$3
        cur=$(dd if="$APPUPGRADE" bs=1 skip="$off" count=8 2>/dev/null \
              | od -An -tx1 | tr -d ' \n')
        case "$cur" in
            31c0c3*) log "installed: $name already (31c0c3)"; return 0 ;;
            "$expect")
                printf '\061\300\303' \
                    | dd of="$APPUPGRADE" bs=1 seek="$off" conv=notrunc 2>/dev/null
                log "installed: $name patched $cur -> 31c0c3"
                ;;
            *)  log "installed: $name UNEXPECTED $cur"; return 1 ;;
        esac
    }
    patch_one 2086500 FUN_002f5a00 415455534883ec50 || rc=1
    patch_one 2086852 FUN_002f5b60 4883ec1864488b04 || rc=1
    patch_one 2086916 FUN_002f5ba0 554889f531f65348 || rc=1
    patch_one 2087076 FUN_002f5c40 554889f5534883ec || rc=1
    patch_one 7346024 FUN_0028f5a0 415455534883ec50 || rc=1
    sync
    [ "$was_ro" = 1 ] && mount -o remount,ro / 2>/dev/null
    [ "$rc" = 0 ] || { log "installed: patch failed"; return 1; }

    M=$(md5_of "$APPUPGRADE")
    if [ "$M" = "$INSTALLED_PATCHED_MD5" ]; then
        log "installed: patched to five-engine-stub (md5=$M)"
        return 0
    fi
    log "installed: md5 mismatch after patch: $M"
    return 1
}

# ============================================================
# Stage 2: update drive — wrapper swap + .ver row 341142
# ============================================================
find_drive(){
    if [ -n "${DRIVE_OVERRIDE:-}" ] && [ -d "$DRIVE_OVERRIDE" ]; then
        echo "$DRIVE_OVERRIDE"; return 0
    fi
    for d in /media/usb/* /media/usb/*/* /media/* /run/media/* \
             /run/media/*/* /mnt/*; do
        [ -d "$d" ] || continue
        [ "$d" = "$USB_DIR" ] && continue
        [ -f "$d/IONIQ5_EU.ver" ] && [ -f "$d/AppUpgrade" ] \
            && [ -f "$d/HU/images/mango-rootfs.tar.gz" ] || continue
        echo "$d"; return 0
    done
    return 1
}

stage_drive(){
    DRV=$(find_drive) || { log "drive: update drive not found"; return 2; }
    log "drive: $DRV"

    ROOT_ROW=$(grep 'mango-rootfs\.tar\.gz' "$DRV/IONIQ5_EU.ver" 2>/dev/null | head -1)
    case "$ROOT_ROW" in
      *"|mango-rootfs.tar.gz|"*"|879126179|1") log "drive: rootfs_row=$ROOT_ROW" ;;
      *) log "drive: unexpected rootfs row: $ROOT_ROW"; return 1 ;;
    esac

    # REVERT marker: restore stock wrapper + .ver from the backup
    if [ -f "$USB_DIR/REVERT" ]; then
        B="$USB_DIR/wrapper_swap_backup"
        [ -f "$B/AppUpgrade" ] && [ -f "$B/IONIQ5_EU.ver" ] \
            || { log "drive: REVERT: backup missing in $B"; return 1; }
        cp -f "$B/AppUpgrade" "$DRV/AppUpgrade" || { log "drive: REVERT wrapper failed"; return 1; }
        cp -f "$B/IONIQ5_EU.ver" "$DRV/IONIQ5_EU.ver" || { log "drive: REVERT .ver failed"; return 1; }
        sync
        [ "$(md5_of "$DRV/AppUpgrade")" = "$STOCK_WRAPPER_MD5" ] \
            || { log "drive: REVERT md5 mismatch"; return 1; }
        log "drive: reverted to stock wrapper"
        return 3
    fi

    WMD5=$(md5_of "$DRV/AppUpgrade")
    log "drive: wrapper_md5=$WMD5"
    case "$WMD5" in
      "$PATCHED_WRAPPER_MD5") WRAPPER_DONE=1 ;;
      "$STOCK_WRAPPER_MD5")   WRAPPER_DONE=0 ;;
      *) log "drive: wrapper md5 unknown: $WMD5"; return 1 ;;
    esac

    CUR_ROW=$(grep -F "$ROW_KEY" "$DRV/IONIQ5_EU.ver" 2>/dev/null | head -1)
    log "drive: row_before=$CUR_ROW"
    case "$CUR_ROW" in
      "$ROW_PATCHED") ROW_DONE=1 ;;
      "$ROW_STOCK")   ROW_DONE=0 ;;
      *) log "drive: unexpected AppUpgrade row: $CUR_ROW"; return 1 ;;
    esac

    [ "$WRAPPER_DONE" = 1 ] && [ "$ROW_DONE" = 1 ] \
        && { log "drive: already swapped"; return 0; }

    # carried wrapper integrity (only needed if we must swap)
    if [ "$WRAPPER_DONE" = 0 ]; then
        [ -f "$PATCHED_WRAPPER" ] || { log "drive: missing carried wrapper"; return 1; }
        [ "$(stat -c%s "$PATCHED_WRAPPER" 2>/dev/null)" = "$WRAPPER_SIZE" ] \
            || { log "drive: carried wrapper size wrong"; return 1; }
        [ "$(md5_of "$PATCHED_WRAPPER")" = "$PATCHED_WRAPPER_MD5" ] \
            || { log "drive: carried wrapper md5 mismatch"; return 1; }
    fi

    B="$USB_DIR/wrapper_swap_backup"
    mkdir -p "$B" 2>/dev/null
    cp -f "$DRV/AppUpgrade" "$B/AppUpgrade" 2>/dev/null
    cp -f "$DRV/AppUpgrade" "$B/AppUpgrade.$STAMP" 2>/dev/null
    cp -f "$DRV/IONIQ5_EU.ver" "$B/IONIQ5_EU.ver" 2>/dev/null
    cp -f "$DRV/IONIQ5_EU.ver" "$B/IONIQ5_EU.ver.$STAMP" 2>/dev/null
    log "drive: backup=$B"

    if [ "$WRAPPER_DONE" = 0 ]; then
        cp -f "$PATCHED_WRAPPER" "$DRV/AppUpgrade" || { log "drive: wrapper copy failed"; return 1; }
        sync
        [ "$(md5_of "$DRV/AppUpgrade")" = "$PATCHED_WRAPPER_MD5" ] \
            || { log "drive: wrapper verify after copy failed"; return 1; }
        log "drive: wrapper_swapped"
    fi

    if [ "$ROW_DONE" = 0 ]; then
        TMPV="$DRV/IONIQ5_EU.ver.new"
        : > "$TMPV" 2>/dev/null || { log "drive: cannot create temp .ver"; return 1; }
        while IFS= read -r line || [ -n "$line" ]; do
            case "$line" in
                "$ROW_KEY"*) echo "$ROW_PATCHED" >> "$TMPV" ;;
                *)           echo "$line"        >> "$TMPV" ;;
            esac
        done < "$DRV/IONIQ5_EU.ver"
        NEW_ROW=$(grep -F "$ROW_KEY" "$TMPV" 2>/dev/null | head -1)
        NEW_ROOT=$(grep 'mango-rootfs\.tar\.gz' "$TMPV" 2>/dev/null | head -1)
        [ "$NEW_ROW" = "$ROW_PATCHED" ] || { rm -f "$TMPV"; log "drive: row patch verify failed"; return 1; }
        [ "$NEW_ROOT" = "$ROOT_ROW" ]   || { rm -f "$TMPV"; log "drive: rootfs row changed"; return 1; }
        if mv -f "$TMPV" "$DRV/IONIQ5_EU.ver" 2>/dev/null; then
            :
        elif cp -f "$TMPV" "$DRV/IONIQ5_EU.ver" 2>/dev/null; then
            rm -f "$TMPV"
        else
            rm -f "$TMPV"; log "drive: .ver replace failed"; return 1
        fi
        sync
        log "drive: row_after=$(grep -F "$ROW_KEY" "$DRV/IONIQ5_EU.ver" | head -1)"
    fi

    [ "$(md5_of "$DRV/AppUpgrade")" = "$PATCHED_WRAPPER_MD5" ] \
        || { log "drive: final wrapper verify failed"; return 1; }
    [ "$(grep -F "$ROW_KEY" "$DRV/IONIQ5_EU.ver" | head -1)" = "$ROW_PATCHED" ] \
        || { log "drive: final row verify failed"; return 1; }
    log "drive: OK (wrapper + row verified)"
    return 0
}

# ============================================================
# Stage 3: update watcher (ps transitions + /tmp/AppUpgrade capture)
# ============================================================
stage_watcher(){
    WDIR="${RW_OVERRIDE:-/rw_data}"
    WSH="$WDIR/update_watcher.sh"
    WLOG="$WDIR/update_watch.log"
    WPID="$WDIR/update_watch.pid"

    # Stop a live watcher before replacing its script: a running instance
    # keeps the old file open and would keep logging on top of the replacement.
    _stop_failed=0
    if [ -f "$WPID" ]; then
        _oldpid=$(cat "$WPID" 2>/dev/null)
        if [ -n "$_oldpid" ] && [ -r "/proc/$_oldpid/cmdline" ] && \
           grep -q 'update_watcher' "/proc/$_oldpid/cmdline" 2>/dev/null; then
            : > "$WDIR/update_watch.stop"
            _wn=0
            while [ "$_wn" -lt 5 ] && kill -0 "$_oldpid" 2>/dev/null; do
                sleep 1
                _wn=$((_wn + 1))
            done
            if kill -0 "$_oldpid" 2>/dev/null; then
                kill -TERM "$_oldpid" 2>/dev/null
                sleep 1
            fi
            if kill -0 "$_oldpid" 2>/dev/null; then
                log "watcher: previous instance survived stop/TERM (pid $_oldpid); leaving it in place"
                _stop_failed=1
            else
                log "watcher: stopped previous instance (pid $_oldpid)"
                rm -f "$WPID" 2>/dev/null
            fi
        else
            # A recycled pid after a reboot is not evidence of a running
            # watcher: drop the stale file even when an unrelated live process
            # now owns that pid, or every later run would refuse to start.
            rm -f "$WPID" 2>/dev/null
        fi
    fi
    rm -f "$WDIR/update_watch.stop" 2>/dev/null
    [ "$_stop_failed" = 1 ] && return 1

    cat > "$WSH" <<'EOF'
#!/bin/sh
# update_watcher.sh — installed by both_copies_v1.
# Logs ps transitions and captures every distinct /tmp/AppUpgrade next to
# this script (normally /rw_data) during an update attempt.
# Self-terminates after at least 45 minutes or when <dir>/update_watch.stop appears.
WBASE=$(dirname "$0")
LOG="$WBASE/update_watch.log"
PIDF="$WBASE/update_watch.pid"
STOP="$WBASE/update_watch.stop"
MAXLOG=${UPDATE_WATCH_MAX_BYTES:-2097152}
MAXLINE=${UPDATE_WATCH_MAX_LINE:-200}
KEEPBYTES=${UPDATE_WATCH_KEEP_BYTES:-32768}
TMPD=${TMPDIR:-/tmp}
PREV="$TMPD/.update_watch.prev.$$"
CUR="$TMPD/.update_watch.cur.$$"

# A pid recycled after a power cycle must not block a fresh watcher.
if [ -f "$PIDF" ]; then
    oldpid=$(cat "$PIDF" 2>/dev/null)
    if [ -n "$oldpid" ] && [ -r "/proc/$oldpid/cmdline" ] && \
       grep -q 'update_watcher' "/proc/$oldpid/cmdline" 2>/dev/null; then
        exit 0
    fi
    rm -f "$PIDF" 2>/dev/null
fi
echo $$ > "$PIDF" 2>/dev/null
trap '[ "$(cat "$PIDF" 2>/dev/null)" = "$$" ] && rm -f "$PIDF" 2>/dev/null
      rm -f "$PREV" "$CUR" 2>/dev/null' EXIT
trap 'exit 0' INT TERM HUP

# Optional helpers: fall back to a plain snapshot / full record rather than
# silently logging nothing when an applet is missing from the vendor rootfs.
HAVE_AWK=1
command -v awk >/dev/null 2>&1 || HAVE_AWK=0
HAVE_COMM=1
command -v comm >/dev/null 2>&1 || HAVE_COMM=0

# The watcher's own pipeline (ps/awk/cut/sort) and the kernel threads change
# on every sample, and logging them would reproduce the old spam at a smaller
# scale.  Keep the header; drop kthreadd's children (PPID 2) and our own
# helper commands (a pipeline element forks, so PPID alone is not enough),
# then bound each line.
snap() {
    if [ "$HAVE_AWK" = 1 ]; then
        ps -ef 2>/dev/null | awk -v me="$$" 'NR == 1 || ($3 != me && $3 != 2 &&
            $0 !~ /update_watcher/ && $8 !~ /^(ps|awk|cut|sort|comm)$/) {
            printf "%s %s %s %s\n", $1, $2, $3, substr($0, index($0, $8))
        }'
    else
        ps -ef 2>/dev/null
    fi | cut -c1-"$MAXLINE" | sort
}

# Hard cap.  The previous version appended a full `ps -ef` (about 50 KB on
# this head unit) on every 2-second transition and reached ~100 MB in one
# window; trimming to the tail keeps the file near MAXLOG.
trim_log() {
    [ -f "$LOG" ] || return 0
    sz=$(wc -c < "$LOG" 2>/dev/null || echo 0)
    [ "${sz:-0}" -gt "$MAXLOG" ] || return 0
    tail -c "$KEEPBYTES" "$LOG" > "$LOG.tmp" 2>/dev/null && mv -f "$LOG.tmp" "$LOG"
    echo "=== log trimmed (kept last $KEEPBYTES of $sz bytes) $(date -u 2>/dev/null) ===" >> "$LOG"
}

trim_log
echo "=== watcher start $(date -u 2>/dev/null) pid $$ base=$WBASE cap=${MAXLOG}B ===" >> "$LOG"
snap > "$PREV" 2>/dev/null
lastcap=""
i=0
# 1350 cycles of at least 2 s, so at least 45 minutes.  A cycle count does
# not trust the wall clock: a unit whose RTC reads 1970 made the old END
# comparison unreliable.
while [ "$i" -lt 1350 ]; do
    i=$((i + 1))
    [ -f "$STOP" ] && break
    snap > "$CUR" 2>/dev/null
    if ! cmp -s "$PREV" "$CUR"; then
        trim_log
        added=""
        removed=""
        if [ "$HAVE_COMM" = 1 ]; then
            added=$(comm -13 "$PREV" "$CUR" 2>/dev/null)
            removed=$(comm -23 "$PREV" "$CUR" 2>/dev/null)
        fi
        {
            if [ -n "$added" ] || [ -n "$removed" ]; then
                echo "--- $(date -u 2>/dev/null) +$(printf '%s\n' "$added" | grep -c .) -$(printf '%s\n' "$removed" | grep -c .) ---"
                [ -n "$added" ] && printf '%s\n' "$added" | sed 's/^/+ /'
                [ -n "$removed" ] && printf '%s\n' "$removed" | sed 's/^/- /'
            else
                echo "--- $(date -u 2>/dev/null) full snapshot (no diff tool) ---"
                cat "$CUR"
            fi
        } >> "$LOG"
        mv -f "$CUR" "$PREV"
    fi
    if [ -f /tmp/AppUpgrade ]; then
        m=$(md5sum /tmp/AppUpgrade 2>/dev/null | cut -d' ' -f1)
        if [ -n "$m" ] && [ "$m" != "$lastcap" ]; then
            if cp -f /tmp/AppUpgrade "$WBASE/tmp_AppUpgrade.$m" 2>/dev/null; then
                echo "captured /tmp/AppUpgrade md5=$m at $(date -u 2>/dev/null)" >> "$LOG"
                lastcap="$m"
            fi
        fi
    fi
    sleep 2
done
echo "=== watcher end $(date -u 2>/dev/null) ===" >> "$LOG"
EOF
    chmod 0755 "$WSH" 2>/dev/null

    if [ -f "$WPID" ] && kill -0 "$(cat "$WPID" 2>/dev/null)" 2>/dev/null; then
        log "watcher: pid file still points at a live process (pid $(cat "$WPID" 2>/dev/null))"
        return 1
    fi
    nohup /bin/sh "$WSH" >/dev/null 2>&1 &
    sleep 1
    if [ -f "$WPID" ] && kill -0 "$(cat "$WPID" 2>/dev/null)" 2>/dev/null; then
        log "watcher: started (pid $(cat "$WPID" 2>/dev/null)), 45-minute window"
        return 0
    fi
    log "watcher: start FAILED (pid file missing)"
    return 1
}

# ============================================================
# Stage 4: diagnostics to the stick
# ============================================================
stage_diag(){
    DIAG="$USB_DIR/both_copies_diag"
    mkdir -p "$DIAG" 2>/dev/null || { log "diag: cannot create $DIAG"; return 1; }

    {
        echo "=== both_copies_v1 diag ==="
        date -u 2>/dev/null
        uname -a 2>/dev/null
        echo "--- cmdline ---"; cat /proc/cmdline 2>/dev/null
        echo "--- ps ---"; ps -ef 2>/dev/null
        echo "--- /tmp ---"; ls -la /tmp 2>/dev/null
        echo "--- /update ---"; ls -la /update 2>/dev/null
        echo "--- mounts ---"; mount 2>/dev/null
        echo "--- df ---"; df -h 2>/dev/null
    } > "$DIAG/snapshot.txt" 2>&1

    {
        echo "installed /app/bin/AppUpgrade:"
        ls -l "$APPUPGRADE" 2>/dev/null
        md5sum "$APPUPGRADE" 2>/dev/null
        for pair in 2086500:FUN_002f5a00 2086852:FUN_002f5b60 \
                    2086916:FUN_002f5ba0 2087076:FUN_002f5c40 \
                    7346024:FUN_0028f5a0; do
            off=${pair%%:*}; name=${pair##*:}
            printf '%s bytes at %s: ' "$name" "$off"
            dd if="$APPUPGRADE" bs=1 skip="$off" count=3 2>/dev/null \
                | od -An -tx1 | tr -d ' \n'
            echo
        done
    } > "$DIAG/installed_appupgrade.txt" 2>&1

    DRV=$(find_drive) || DRV=""
    if [ -n "$DRV" ]; then
        {
            echo "drive: $DRV"
            md5sum "$DRV/AppUpgrade" 2>/dev/null
            grep -E 'mango-rootfs\.tar\.gz|IONIQ5_EU\|AppUpgrade\|' \
                "$DRV/IONIQ5_EU.ver" 2>/dev/null
        } > "$DIAG/drive_state.txt" 2>&1
    fi

    mkdir -p "$DIAG/engines" 2>/dev/null
    for f in /tmp/AppUpgrade /update/usb/bin/AppUpgrade \
             "${RW_OVERRIDE:-/rw_data}"/tmp_AppUpgrade.* \
             /media/usb/*/AppUpgrade /media/usb/*/*/AppUpgrade; do
        [ -f "$f" ] || continue
        cp -f "$f" "$DIAG/engines/$(echo "$f" | tr '/' '_')" 2>/dev/null
    done
    [ -f "$APPUPGRADE" ] && \
        cp -f "$APPUPGRADE" "$DIAG/engines/_app_bin_AppUpgrade" 2>/dev/null

    cp -a /log_data/AppUpgrade "$DIAG/log_AppUpgrade" 2>/dev/null || true
    cp -f "${RW_OVERRIDE:-/rw_data}/update_watch.log" "$DIAG/" 2>/dev/null || true
    for f in "${RW_OVERRIDE:-/rw_data}"/tmp_AppUpgrade.*; do
        [ -f "$f" ] && cp -f "$f" "$DIAG/engines/" 2>/dev/null
    done
    cp -f /rw_data/initial_root.log "$DIAG/" 2>/dev/null || true
    cp -f /rw_data/second_stage_preflight.txt "$DIAG/" 2>/dev/null || true
    cp -f /rw_data/AppUpgrade.txt "$DIAG/" 2>/dev/null || true

    tar -czf "$DIAG/dltlog.tar.gz" /log_data/dltlog 2>/dev/null \
        || echo "dltlog tar failed" >> "$DIAG/errors.txt" 2>/dev/null

    tar -czf "$USB_DIR/both_copies_diag.tar.gz" -C "$USB_DIR" both_copies_diag \
        2>/dev/null || true
    log "diag: wrote $DIAG and both_copies_diag.tar.gz"
    return 0
}

# ============================================================
# main
# ============================================================
RC_INSTALLED=FAIL
RC_DRIVE=FAIL
RC_WATCHER=FAIL
RC_DIAG=FAIL

stage_installed && RC_INSTALLED=OK
stage_drive
drc=$?
case "$drc" in
  0) RC_DRIVE=OK ;;
  2) RC_DRIVE=NOTFOUND ;;
  3) RC_DRIVE=REVERTED ;;
  *) RC_DRIVE=FAIL ;;
esac
stage_watcher && RC_WATCHER=OK
stage_diag && RC_DIAG=OK

log "summary installed=$RC_INSTALLED drive=$RC_DRIVE watcher=$RC_WATCHER diag=$RC_DIAG"
if [ "$RC_INSTALLED" = "OK" ] && [ "$RC_DRIVE" = "OK" ] \
   && [ "$RC_WATCHER" = "OK" ] && [ "$RC_DIAG" = "OK" ]; then
    finish "OK"
elif [ "$RC_DRIVE" = "REVERTED" ]; then
    finish "REVERTED"
else
    finish "PARTIAL installed=$RC_INSTALLED drive=$RC_DRIVE watcher=$RC_WATCHER diag=$RC_DIAG"
fi
