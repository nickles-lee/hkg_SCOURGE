#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# TARGET: AU Linux Gen5W 260319 head unit, run as root by hudnav-aa.service at every boot.
# Persistent Android Auto -> HUD: this script starts hudnavd (tx) once the session bus exists.
# The two overlay bind mounts are ALREADY in place at this point: hudnav-premount.service
# ordered them Before=carplay_iAP2.service / ApplicationLauncher.service, so the vendor
# services exec the patched copies on their first start. This script never restarts a
# service at boot (a phone plugged in before boot would be lost by a restart).
# Nothing here writes to / ; the bind mounts vanish at every reboot and are re-made by
# the premount unit. aa-session.sh / cp-session.sh keep their manual start/stop behaviour.
#
# It does NOTHING (exit 0, car boots stock) unless ALL of:
#   /rw_data/hudnav/enabled exists (master switch; delete it to disable without SSH tooling)
#   /etc/ld.so.preload is absent
#   the kit verifies against /rw_data/hudnav/bin/SHA256SUMS and the helpers are present
#   AppProjection appears within the wait window (i.e. this is a UI boot)
# The boot guard (hudnav-bootguard) and the USB recovery stick both disable it by renaming
# /rw_data/hudnav, which makes this script exit early on the next boot.
#
# Test hooks: HUDNAV_ROOT (fake root), HUDNAV_PATH_PREPEND (fake tools), HUDNAV_WAIT_S (wait window),
# HUDNAV_SLEEP (seconds per poll; 0 in tests).
set -u
PATH="${HUDNAV_PATH_PREPEND:+$HUDNAV_PATH_PREPEND:}/bin:/sbin:/usr/bin:/usr/sbin"
R="${HUDNAV_ROOT:-}"
KIT="$R/rw_data/hudnav"
BIN="$KIT/bin"
LOG="$KIT/boot.log"
WAIT="${HUDNAV_WAIT_S:-240}"
SLP="${HUDNAV_SLEEP:-1}"
SESSION_FILE="$R/tmp/session_appmgr"
MOUNTS="$R/proc/mounts"
AP="$R/app/bin/AppProjection"
CP="$R/app/bin/iAP2Service-2.0"

log() {
    [ -d "$KIT" ] || return 0
    printf '%s %s\n' "$(date)" "$*" >> "$LOG" 2>/dev/null
    if [ "$(wc -l < "$LOG" 2>/dev/null)" -gt 400 ] 2>/dev/null; then
        tail -n 300 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG" 2>/dev/null
    fi
}

waitfor() {   # waitfor DESCRIPTION TEST-COMMAND... : poll once per $SLP s, up to $WAIT polls
    what=$1; shift; i=0
    while [ "$i" -lt "$WAIT" ]; do
        if "$@"; then return 0; fi
        [ "$SLP" = 0 ] || sleep "$SLP"
        i=$((i+1))
    done
    log "gave up waiting for $what after $WAIT polls"
    return 1
}
have_ap()      { [ -n "$(pidof AppProjection 2>/dev/null)" ]; }
have_session() { [ -s "$SESSION_FILE" ]; }
have_iap2()    { [ -n "$(pidof iAP2Service-2.0 2>/dev/null)" ]; }

# True when hudnav-premount.service already bound the patched copy over TARGET.
bound() {
    [ -r "$MOUNTS" ] || return 1
    while read -r dev mp rest; do
        [ "$mp" = "$1" ] && return 0
    done < "$MOUNTS"
    return 1
}

case "${1:-}" in
    start) ;;
    *) echo "usage: $0 start" >&2; exit 2 ;;
esac

[ -f "$KIT/enabled" ] || { log "not enabled (no $KIT/enabled): stock boot"; exit 0; }
[ ! -e "$R/etc/ld.so.preload" ] || { log "REFUSE: /etc/ld.so.preload exists"; exit 0; }
for f in aa-session.sh hudnav-run.sh hudnavd micom-send micom-send-tp CANService AppProjection.aa aa_shim.so SHA256SUMS; do
    [ -f "$BIN/$f" ] || { log "REFUSE: missing $BIN/$f"; exit 0; }
done
chk=$( (cd "$BIN" && sha256sum -c SHA256SUMS 2>&1) ) || { log "REFUSE: kit hash check failed: $chk"; exit 0; }
log "boot: enabled, kit verified"

# 1. hudnavd first, so it sees the AppManager ownership signal when Android Auto connects.
if waitfor "session bus file" have_session; then
    out=$(sh "$BIN/hudnav-run.sh" start tx --yes 2>&1); log "hudnav-run: $out"
else
    log "no session bus: not starting hudnavd, not mounting"; exit 0
fi

# 2. AA overlay: premount did it before ApplicationLauncher started AppProjection.
#    NO restart here: on a boot with the phone already attached, restarting would
#    orphan the iAP2 session and the user would have to replug.
if waitfor "AppProjection" have_ap; then
    if bound "$AP"; then
        log "AA overlay already active (hudnav-premount); no restart"
    else
        log "AA overlay not active (premount refused or aa-nav-enable off): AppProjection left stock, no restart"
    fi
else
    log "no AppProjection (background wake?): stopping hudnavd"
    sh "$BIN/hudnav-run.sh" stop >/dev/null 2>&1
    exit 0
fi

# 3. CarPlay: same rule, only with its own switch /rw_data/hudnav/cp-enabled.
#    hudnav-premount.service verified the stock hash, the kit and the CP gate and
#    mounted the patched copy before carplay_iAP2.service started. If it refused,
#    leave the stock service running; never stop/start it here.
if [ -f "$KIT/cp-enabled" ]; then
    if waitfor "iAP2Service" have_iap2; then
        if bound "$CP"; then
            log "CarPlay overlay already active (hudnav-premount); no restart"
        else
            log "CarPlay overlay not active (premount refused): iAP2Service left stock, no restart"
        fi
    else
        log "CarPlay enabled but iAP2Service missing: CarPlay left stock"
    fi
fi
exit 0
