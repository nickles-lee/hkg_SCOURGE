#!/bin/sh
# initial_root USB failsafe loader.
#
# Runs as root from initial-root-failsafe.service. Two jobs:
#   1. Re-apply the AppUpgrade signature-check stub (idempotent, survives a
#      stock updater being copied back by the failsafe script).
#   2. Poll the HU's USB mounts for run_stuff.sh and run it as root, md5-gated
#      so a left-in stick does not re-run on every poll.
#
# The HU mounts USB at /media/usb/<N>/<VOLID>/ (two levels deep). The v2
# dev-loader field-tested this path list on the car.
PATH=/bin:/sbin:/usr/bin:/usr/sbin
STATE=/run/initial_root.lastmd5
LOG=/rw_data/initial_root.log
TMPMNT=/run/initial_root_usb
GUARD=/etc/initial_root/updater_guard.sh

log() {
    echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null || true
}

# 1. Keep the updater patch in place.
if [ -f "$GUARD" ]; then
    /bin/sh "$GUARD" >> "$LOG" 2>&1 || true
fi

run_if_new() {
    s="$1"
    [ -f "$s" ] || return 1
    m=$(md5sum "$s" 2>/dev/null | cut -d' ' -f1)
    [ -n "$m" ] || return 1
    [ "$m" = "$(cat "$STATE" 2>/dev/null)" ] && return 1
    d=$(dirname "$s")
    log "=== run $s md5=$m usb=$d ==="
    USB_DIR="$d" /bin/sh "$s" >> "$LOG" 2>&1
    rc=$?
    log "=== rc=$rc ==="
    echo "$m" > "$STATE" 2>/dev/null || true
    return 0
}

scan_mounts() {
    for s in /media/usb/*/*/run_stuff.sh /media/usb/*/run_stuff.sh \
             /media/usb/run_stuff.sh /media/*/run_stuff.sh \
             /run/media/*/run_stuff.sh /run/media/*/*/run_stuff.sh \
             /mnt/*/run_stuff.sh; do
        run_if_new "$s" && return 0
    done
    return 1
}

scan_raw() {
    for d in /dev/sd?1 /dev/sd?; do
        [ -b "$d" ] || continue
        grep -q "^$d " /proc/mounts && continue
        mkdir -p "$TMPMNT" 2>/dev/null
        mount -o ro "$d" "$TMPMNT" 2>/dev/null || continue
        run_if_new "$TMPMNT/run_stuff.sh"
        r=$?
        umount "$TMPMNT" 2>/dev/null
        [ "$r" = 0 ] && return 0
    done
    return 1
}

log "loader start"
while true; do
    scan_mounts || scan_raw
    sleep 8
done
