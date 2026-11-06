# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#!/bin/sh
# button_gate_v1 — patch the AppScomoDM button gate + both engine copies,
# install the update watcher, and collect diagnostics.
#
# Stages:
#   1. installed  /app/bin/AppUpgrade: verify/apply the four validator stubs
#                 (no-op if already md5 c17c2655...)
#   2. button     /app/bin/AppScomoDM (UsbUpdateManager): stub the two
#                 verifier entries (FUN_001e3430 @ 930864, FUN_001e3470 @
#                 930928). This is the gate that grays the Update button.
#   3. drive      update drive: verify/swap the wrapper + .ver row 341142
#   4. watcher    install + start /rw_data/update_watcher.sh (45 min)
#   5. diag       collect snapshot, engine copies, AppScomoDM state, DLT tar
#
# The AppScomoDM process must reload to pick up the patch: REBOOT the head
# unit after this payload runs, before starting the update.
#
# Optional marker files next to this script:
#   REVERT   restore AppScomoDM and the drive wrapper from the backups
#
# Payload files (same directory as this script):
#   AppUpgrade.patched.te2   the patched wrapper (10,803,676 B)
#
# Test hooks: APPUPGRADE_OVERRIDE, APPSCDM_OVERRIDE, DRIVE_OVERRIDE,
# RW_OVERRIDE.

set -u
PATH=/bin:/sbin:/usr/bin:/usr/sbin
USB_DIR="${USB_DIR:-$(dirname "$0")}"
RESULT="$USB_DIR/button_gate_result.txt"
RW="${RW_OVERRIDE:-/rw_data/button_gate}"
LOG="$RW/log"
STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || echo unknown)

APPUPGRADE="${APPUPGRADE_OVERRIDE:-/app/bin/AppUpgrade}"
APPSCDM="${APPSCDM_OVERRIDE:-/app/bin/AppScomoDM}"
F_SIZE=10787256
F_PATCHED_MD5=c17c2655aecef461d2d9ddae15af775b
SCDM_SIZE=1934752

PATCHED_WRAPPER="$USB_DIR/AppUpgrade.patched.te2"
PATCHED_WRAPPER_MD5=67a9fa486dbcae63e1a20ceee6eb2d5d
STOCK_WRAPPER_MD5=15a33d05b52ffa3d2ada5d44b4f37253
WRAPPER_SIZE=10803676
ROW_STOCK="IONIQ5_EU|AppUpgrade|341142|609028602|10803676|1"
ROW_PATCHED="IONIQ5_EU|AppUpgrade|341142|-990581836|10803676|1"
ROW_KEY='IONIQ5_EU|AppUpgrade|341142|'

mkdir -p "$RW" 2>/dev/null || true
: > "$LOG" 2>/dev/null
log(){ echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null; }
md5_of(){ md5sum "$1" 2>/dev/null | cut -d' ' -f1; }

finish(){
    {
        echo "BUTTON_GATE_V1"
        echo "time=$(date -u 2>/dev/null)"
        echo "usb=$USB_DIR"
        echo "uid=$(id -u 2>/dev/null)"
        echo "uname=$(uname -a 2>/dev/null)"
        cat "$LOG" 2>/dev/null
        echo "reboot_required=1"
        echo "status=$1"
    } > "$RESULT" 2>&1
    cp -f "$RESULT" "$RW/result.txt" 2>/dev/null
    sync
    exit 0
}

# generic stub: patch FILE at OFF if the stock bytes match; idempotent
patch_one_file(){
    file=$1
    off=$2
    name=$3
    expect=$4
    count=$(( ${#expect} / 2 ))
    cur=$(dd if="$file" bs=1 skip="$off" count="$count" 2>/dev/null \
          | od -An -tx1 | tr -d ' \n')
    case "$cur" in
        31c0c3*) log "$name: already (31c0c3)"; return 0 ;;
        "$expect")
            printf '\061\300\303' \
                | dd of="$file" bs=1 seek="$off" conv=notrunc 2>/dev/null
            log "$name: patched $cur -> 31c0c3"
            ;;
        *)  log "$name: UNEXPECTED $cur"; return 1 ;;
    esac
}

# ============================================================
# Stage 1: installed /app/bin/AppUpgrade — four validator stubs
# ============================================================
stage_installed(){
    [ -f "$APPUPGRADE" ] || { log "installed: missing $APPUPGRADE"; return 1; }
    SZ=$(stat -c%s "$APPUPGRADE" 2>/dev/null)
    [ "$SZ" = "$F_SIZE" ] || { log "installed: unexpected size $SZ"; return 1; }
    M=$(md5_of "$APPUPGRADE")
    log "installed: md5=$M"
    if [ "$M" = "$F_PATCHED_MD5" ]; then
        log "installed: already four-stub"
        return 0
    fi
    mkdir -p "$RW" 2>/dev/null
    cp -f "$APPUPGRADE" "$RW/AppUpgrade.before.$STAMP" 2>/dev/null
    cp -f "$APPUPGRADE" "$USB_DIR/AppUpgrade.before-button-gate" 2>/dev/null

    was_ro=0
    grep -q ' / .* ro[, ]' /proc/mounts && was_ro=1
    [ "$was_ro" = 1 ] && mount -o remount,rw / 2>/dev/null

    rc=0
    patch_one_file "$APPUPGRADE" 2086500 FUN_002f5a00 415455534883ec50 || rc=1
    patch_one_file "$APPUPGRADE" 2086852 FUN_002f5b60 4883ec1864488b04 || rc=1
    patch_one_file "$APPUPGRADE" 2086916 FUN_002f5ba0 554889f531f65348 || rc=1
    patch_one_file "$APPUPGRADE" 2087076 FUN_002f5c40 554889f5534883ec || rc=1
    sync
    [ "$was_ro" = 1 ] && mount -o remount,ro / 2>/dev/null
    [ "$rc" = 0 ] || { log "installed: patch failed"; return 1; }

    M=$(md5_of "$APPUPGRADE")
    if [ "$M" = "$F_PATCHED_MD5" ]; then
        log "installed: patched to four-stub (md5=$M)"
        return 0
    fi
    log "installed: md5 mismatch after patch: $M"
    return 1
}

# ============================================================
# Stage 2: /app/bin/AppScomoDM — button-gate verifier stubs
# ============================================================
stage_scomo(){
    [ -f "$APPSCDM" ] || { log "button: missing $APPSCDM"; return 1; }
    SZ=$(stat -c%s "$APPSCDM" 2>/dev/null)
    [ "$SZ" = "$SCDM_SIZE" ] || { log "button: unexpected size $SZ"; return 1; }
    M=$(md5_of "$APPSCDM")
    log "button: md5=$M"

    # REVERT marker: restore the pre-patch AppScomoDM
    if [ -f "$USB_DIR/REVERT" ]; then
        B="$USB_DIR/AppScomoDM.before-button-gate"
        [ -f "$B" ] || { log "button: REVERT backup missing"; return 1; }
        cp -f "$B" "$APPSCDM" || { log "button: REVERT copy failed"; return 1; }
        sync
        cur=$(dd if="$APPSCDM" bs=1 skip=930928 count=3 2>/dev/null \
              | od -An -tx1 | tr -d ' \n')
        [ "$cur" != "31c0c3" ] || { log "button: REVERT still patched"; return 1; }
        log "button: reverted to stock AppScomoDM"
        return 3
    fi

    # already patched?
    a=$(dd if="$APPSCDM" bs=1 skip=930864 count=3 2>/dev/null | od -An -tx1 | tr -d ' \n')
    b=$(dd if="$APPSCDM" bs=1 skip=930928 count=3 2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$a" = "31c0c3" ] && [ "$b" = "31c0c3" ]; then
        log "button: already two-stub"
        return 0
    fi

    mkdir -p "$RW" 2>/dev/null
    cp -f "$APPSCDM" "$RW/AppScomoDM.before.$STAMP" 2>/dev/null
    cp -f "$APPSCDM" "$USB_DIR/AppScomoDM.before-button-gate" 2>/dev/null

    was_ro=0
    grep -q ' / .* ro[, ]' /proc/mounts && was_ro=1
    [ "$was_ro" = 1 ] && mount -o remount,rw / 2>/dev/null

    rc=0
    patch_one_file "$APPSCDM" 930864 FUN_001e3430 4883ec1864488b0425 || rc=1
    patch_one_file "$APPSCDM" 930928 FUN_001e3470 554889f531f65348 || rc=1
    sync
    [ "$was_ro" = 1 ] && mount -o remount,ro / 2>/dev/null
    [ "$rc" = 0 ] || { log "button: patch failed"; return 1; }

    a=$(dd if="$APPSCDM" bs=1 skip=930864 count=3 2>/dev/null | od -An -tx1 | tr -d ' \n')
    b=$(dd if="$APPSCDM" bs=1 skip=930928 count=3 2>/dev/null | od -An -tx1 | tr -d ' \n')
    [ "$a" = "31c0c3" ] && [ "$b" = "31c0c3" ] \
        || { log "button: verify failed a=$a b=$b"; return 1; }
    log "button: patched (md5=$(md5_of "$APPSCDM"))"
    return 0
}

# ============================================================
# Stage 3: update drive — wrapper swap + .ver row 341142
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

    if [ -f "$USB_DIR/REVERT" ]; then
        B="$USB_DIR/wrapper_swap_backup"
        [ -f "$B/AppUpgrade" ] && [ -f "$B/IONIQ5_EU.ver" ] \
            || { log "drive: REVERT backup missing"; return 1; }
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
# Stage 4: update watcher
# ============================================================
stage_watcher(){
    WDIR="${RW_OVERRIDE:-/rw_data}"
    WSH="$WDIR/update_watcher.sh"
    WPID="$WDIR/update_watch.pid"

    cat > "$WSH" <<'EOF'
#!/bin/sh
# update_watcher.sh — installed by button_gate_v1.
# Logs ps transitions and captures every distinct /tmp/AppUpgrade next to
# this script. Self-terminates after 45 minutes or on update_watch.stop.
WBASE=$(dirname "$0")
LOG="$WBASE/update_watch.log"
PIDF="$WBASE/update_watch.pid"
STOP="$WBASE/update_watch.stop"
[ -f "$PIDF" ] && kill -0 "$(cat "$PIDF" 2>/dev/null)" 2>/dev/null && exit 0
echo $$ > "$PIDF" 2>/dev/null
NOW=$(date +%s 2>/dev/null || echo 0)
END=$(( NOW + 2700 ))
prev=""
lastcap=""
echo "=== watcher start $(date -u 2>/dev/null) pid $$ base=$WBASE ===" >> "$LOG"
while [ "$(date +%s 2>/dev/null || echo 9999999999)" -lt "$END" ]; do
    [ -f "$STOP" ] && break
    cur=$(ps -ef 2>/dev/null | md5sum 2>/dev/null | cut -d' ' -f1)
    if [ -n "$cur" ] && [ "$cur" != "$prev" ]; then
        {
            echo "--- $(date -u 2>/dev/null) ps md5=$cur ---"
            ps -ef 2>/dev/null
        } >> "$LOG"
        prev="$cur"
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
rm -f "$PIDF" 2>/dev/null
EOF
    chmod 0755 "$WSH" 2>/dev/null

    if [ -f "$WPID" ] && kill -0 "$(cat "$WPID" 2>/dev/null)" 2>/dev/null; then
        log "watcher: already running (pid $(cat "$WPID" 2>/dev/null))"
        return 0
    fi
    rm -f "$WDIR/update_watch.stop" 2>/dev/null
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
# Stage 5: diagnostics
# ============================================================
stage_diag(){
    DIAG="$USB_DIR/button_gate_diag"
    mkdir -p "$DIAG" 2>/dev/null || { log "diag: cannot create $DIAG"; return 1; }

    {
        echo "=== button_gate_v1 diag ==="
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
                    2086916:FUN_002f5ba0 2087076:FUN_002f5c40; do
            off=${pair%%:*}; name=${pair##*:}
            printf '%s bytes at %s: ' "$name" "$off"
            dd if="$APPUPGRADE" bs=1 skip="$off" count=3 2>/dev/null \
                | od -An -tx1 | tr -d ' \n'
            echo
        done
    } > "$DIAG/installed_appupgrade.txt" 2>&1

    {
        echo "installed /app/bin/AppScomoDM:"
        ls -l "$APPSCDM" 2>/dev/null
        md5sum "$APPSCDM" 2>/dev/null
        for pair in 930864:FUN_001e3430 930928:FUN_001e3470; do
            off=${pair%%:*}; name=${pair##*:}
            printf '%s bytes at %s: ' "$name" "$off"
            dd if="$APPSCDM" bs=1 skip="$off" count=3 2>/dev/null \
                | od -An -tx1 | tr -d ' \n'
            echo
        done
    } > "$DIAG/installed_appscomo.txt" 2>&1

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
    [ -f "$APPSCDM" ] && \
        cp -f "$APPSCDM" "$DIAG/engines/_app_bin_AppScomoDM" 2>/dev/null

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

    tar -czf "$USB_DIR/button_gate_diag.tar.gz" -C "$USB_DIR" button_gate_diag \
        2>/dev/null || true
    log "diag: wrote $DIAG and button_gate_diag.tar.gz"
    return 0
}

# ============================================================
# main
# ============================================================
RC_INSTALLED=FAIL
RC_BUTTON=FAIL
RC_DRIVE=FAIL
RC_WATCHER=FAIL
RC_DIAG=FAIL

stage_installed && RC_INSTALLED=OK
stage_scomo
src=$?
case "$src" in
  0) RC_BUTTON=OK ;;
  3) RC_BUTTON=REVERTED ;;
  *) RC_BUTTON=FAIL ;;
esac
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

log "summary installed=$RC_INSTALLED button=$RC_BUTTON drive=$RC_DRIVE watcher=$RC_WATCHER diag=$RC_DIAG"
if [ "$RC_INSTALLED" = "OK" ] && [ "$RC_BUTTON" = "OK" ] \
   && [ "$RC_DRIVE" = "OK" ] && [ "$RC_WATCHER" = "OK" ] \
   && [ "$RC_DIAG" = "OK" ]; then
    finish "OK"
elif [ "$RC_BUTTON" = "REVERTED" ] || [ "$RC_DRIVE" = "REVERTED" ]; then
    finish "REVERTED"
else
    finish "PARTIAL installed=$RC_INSTALLED button=$RC_BUTTON drive=$RC_DRIVE watcher=$RC_WATCHER diag=$RC_DIAG"
fi
