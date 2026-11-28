#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# TARGET: AU Linux Gen5W 260319 head unit, run as root by hudnav-premount.service
# at every boot, BEFORE carplay_iAP2.service and ApplicationLauncher.service.
#
# What this changes: only bind mounts of the two patched, DT_NEEDED-carrying copies
# over /app/bin/AppProjection and /app/bin/iAP2Service-2.0. It NEVER writes to /,
# NEVER uses /etc/ld.so.preload, and NEVER restarts a service or kills a process.
# Because the mounts are in place before the vendor units start, they exec the
# patched copies directly: a phone already plugged in at boot keeps its session.
#
# Fail closed: ANY check failure logs REFUSE, makes no mount (or rolls back a
# partial mount), leaves the stock binaries in place and exits 0 so the boot
# continues normally. The boot script (hudnav-aa-boot.sh) only starts hudnavd
# afterwards; it no longer calls aa-session.sh / cp-session.sh at boot.
#
# Kill switches (all must be considered):
#   /rw_data/hudnav/enabled        master; absent -> stock boot
#   /rw_data/hudnav/aa-nav-enable  AA overlay; absent -> stock AppProjection
#   /rw_data/hudnav/cp-enabled     CarPlay overlay; absent -> stock iAP2Service
# The helper gate /rw_data/hudnav/cp-rg-enable is created here for the CP library
# (and removed on rollback); aa-nav-enable is pre-existing and is never removed.
#
# Test hooks: HUDNAV_ROOT (fake root), HUDNAV_PATH_PREPEND (fake tools),
# HUDNAV_PIN_AP / HUDNAV_PIN_CP (stock hashes), HUDNAV_TEST_MOUNT_FAIL (simulate
# the CP mount failing, to exercise rollback).
set -u
PATH="${HUDNAV_PATH_PREPEND:+$HUDNAV_PATH_PREPEND:}/bin:/sbin:/usr/bin:/usr/sbin"
R="${HUDNAV_ROOT:-}"
KIT="$R/rw_data/hudnav"
BIN="$KIT/bin"
PRE="$R/etc/ld.so.preload"
MOUNTS="$R/proc/mounts"
LOG="$KIT/premount.log"
ENABLED="$KIT/enabled"

AP="$R/app/bin/AppProjection"
AP_COPY="$BIN/AppProjection.aa"
AP_SO="$BIN/aa_shim.so"
AP_SUMS="$BIN/SHA256SUMS"
AP_SWITCH="$KIT/aa-nav-enable"
AP_PIN="${HUDNAV_PIN_AP:-726c69ce627e9183771d11bdfac9acf9d92286a9b6c1a0d88bfbd6b5ac5a7d50}"

CP="$R/app/bin/iAP2Service-2.0"
CP_COPY="$BIN/iAP2Service-2.0.cp"
CP_SO="$BIN/cp_rg.so"
CP_SUMS="$BIN/SHA256SUMS.cp"
CP_SWITCH="$KIT/cp-enabled"
CP_GATE="$KIT/cp-rg-enable"
CP_PIN="${HUDNAV_PIN_CP:-e663d5129177a8bd241cb875e36ec5e507d39a2c8111216acafc272bdc3fa88b}"

shaof() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

log() {
    [ -d "$KIT" ] || return 0
    printf '%s %s\n' "$(date)" "$*" >> "$LOG" 2>/dev/null || return 0
    t="$LOG.tmp"
    if tail -n 200 "$LOG" > "$t" 2>/dev/null; then mv "$t" "$LOG" 2>/dev/null; else rm -f "$t"; fi
}

# A line in proc/mounts whose SECOND field is exactly the target path.
bound() {
    [ -r "$MOUNTS" ] || return 1
    while read -r dev mp rest; do
        [ "$mp" = "$1" ] && return 0
    done < "$MOUNTS"
    return 1
}

# Same exec-label rule as aa-session.sh / cp-session.sh: label the copy from the
# real binary so SMACK does not reject the exec at mount time.
label_copy() {
    target=$1; copy=$2; label=""
    if command -v chsmack >/dev/null 2>&1; then
        label=$(chsmack "$target" 2>/dev/null | sed -n 's/.*execute="\([^"]*\)".*/\1/p' | head -1)
    fi
    if [ -z "$label" ] && command -v getfattr >/dev/null 2>&1; then
        label=$(getfattr -n security.SMACK64EXEC --only-values "$target" 2>/dev/null | head -1)
    fi
    if [ -z "$label" ]; then
        log "no exec label readable on $target; defaulting to App::projection"
        label=App::projection
    fi
    if command -v chsmack >/dev/null 2>&1; then
        chsmack -e "$label" "$copy" || return 1
    elif command -v setfattr >/dev/null 2>&1; then
        setfattr -n security.SMACK64EXEC -v "$label" "$copy" || return 1
    else
        log "REFUSE: no chsmack or setfattr available to label $copy"
        return 1
    fi
}

undo_mount() {
    target=$1; gate=$2; name=$3
    if umount "$target" 2>/dev/null || umount -l "$target" 2>/dev/null; then
        log "$name: rollback umount ok"
    else
        log "$name: rollback umount FAILED"
    fi
    [ -z "$gate" ] || rm -f "$gate"
    if bound "$target"; then log "$name: STILL bind-mounted after rollback"; else log "$name: stock restored after rollback"; fi
}

# mount_one NAME TARGET COPY PIN GATE
# GATE is empty when no gate file is created by this script (AA), otherwise it is
# the gate we own and must remove on failure (CP).
mount_one() {
    name=$1; target=$2; copy=$3; pin=$4; gate=$5
    if bound "$target"; then
        log "$name: already bind-mounted; nothing to do"
        return 0
    fi
    now=$(shaof "$target")
    if [ "$now" != "$pin" ]; then
        log "REFUSE: $target sha256 $now != stock pin $pin"
        return 1
    fi
    if ! label_copy "$target" "$copy"; then
        log "REFUSE: could not label $copy"
        return 1
    fi
    if [ -n "$gate" ]; then
        if ! : > "$gate"; then
            log "REFUSE: could not create $gate"
            return 1
        fi
    fi
    if [ "$name" = CP ] && [ "${HUDNAV_TEST_MOUNT_FAIL:-}" = 1 ]; then
        [ -z "$gate" ] || rm -f "$gate"
        log "REFUSE: test-induced mount failure for $name"
        return 1
    fi
    if ! mount --bind "$copy" "$target" 2>/dev/null; then
        [ -z "$gate" ] || rm -f "$gate"
        log "REFUSE: mount --bind $copy $target failed"
        return 1
    fi
    if ! bound "$target"; then
        [ -z "$gate" ] || rm -f "$gate"
        umount "$target" 2>/dev/null || umount -l "$target" 2>/dev/null || true
        log "REFUSE: bind mount for $name not visible in $MOUNTS"
        return 1
    fi
    log "$name: bind mount verified in proc/mounts"
    return 0
}

log "premount: start"
[ -f "$ENABLED" ] || { log "not enabled ($ENABLED absent): stock boot"; exit 0; }
[ ! -e "$PRE" ] || { log "REFUSE: $PRE exists"; exit 0; }

missing=0
for f in AppProjection.aa aa_shim.so SHA256SUMS iAP2Service-2.0.cp cp_rg.so SHA256SUMS.cp; do
    [ -f "$BIN/$f" ] || { log "REFUSE: missing $BIN/$f"; missing=1; }
done
[ "$missing" = 0 ] || exit 0
chk=$( (cd "$BIN" && sha256sum -c SHA256SUMS 2>&1) ) || { log "REFUSE: SHA256SUMS failed: $chk"; exit 0; }
chk=$( (cd "$BIN" && sha256sum -c SHA256SUMS.cp 2>&1) ) || { log "REFUSE: SHA256SUMS.cp failed: $chk"; exit 0; }
log "premount: enabled, no preload, kit verified"

want_aa=0; want_cp=0
[ -f "$AP_SWITCH" ] && want_aa=1
[ -f "$CP_SWITCH" ] && want_cp=1
if [ "$want_aa" = 0 ] && [ "$want_cp" = 0 ]; then
    log "no aa-nav-enable and no cp-enabled: stock boot"
    exit 0
fi
if [ "$want_aa" = 1 ] && [ ! -f "$AP" ]; then log "REFUSE: $AP missing"; exit 0; fi
if [ "$want_cp" = 1 ] && [ ! -f "$CP" ]; then log "REFUSE: $CP missing"; exit 0; fi

aa_done=0
if [ "$want_aa" = 1 ]; then
    if mount_one AA "$AP" "$AP_COPY" "$AP_PIN" ""; then aa_done=1
    else log "premount: aborted before CarPlay; stock binaries left"; exit 0; fi
fi
if [ "$want_cp" = 1 ]; then
    if ! mount_one CP "$CP" "$CP_COPY" "$CP_PIN" "$CP_GATE"; then
        [ "$aa_done" = 1 ] && undo_mount "$AP" "" AA
        log "premount: aborted after rollback; stock binaries left"
        exit 0
    fi
fi
log "premount: done (AA=$want_aa CP=$want_cp) - no service restarted"
exit 0
