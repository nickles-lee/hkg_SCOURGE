#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# TARGET: AU Linux Gen5W 260319 head unit, run as root.
#
# Activate the shims through /etc/ld.so.preload instead of the LD_PRELOAD environment variable.
# Why: the vendor apps (AppProjection, iAP2Service-2.0, ...) carry SMACK exec labels, so their exec
# sets AT_SECURE=1 and glibc 2.25 ignores LD_PRELOAD entries that contain a '/'. /etc/ld.so.preload
# is not subject to that rule. See docs/hudnav/AA_PROTOCOL.md (VM rehearsal, incl. the
# dlsym bug that would have broken EVERY process).
#
# THIS CHANGES A FILE THAT EVERY PROCESS READS. Order of safety:
#   preflight  read-only. Runs a command battery with each library loaded through env LD_PRELOAD on
#              THIS unit's own loader (my shell is unlabelled, so env LD_PRELOAD works for it) and
#              requires zero loader errors. Catches the class of bug "library breaks processes".
#   canary     lists noop.so only (a library that does nothing). One reboot, proves the mechanism.
#   enable     lists the real shims (aa_shim.so + cp_hook.so). Needs a passing preflight.
#   disable    removes /etc/ld.so.preload (and remounts / read-only).
# The root fs is read-only; this remounts it rw only for the write and always returns it to ro.
# Recovery if a bad library were ever listed: every command would fail, so the only way back is
# the USB failsafe stick. Hence: preflight first, canary first.
#
# Usage: sh ld-preload.sh status|preflight|canary|enable|disable|verify [--yes]
set -u
PATH="${HUDNAV_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
DEST="${HUDNAV_RW_DIR:-/rw_data/hudnav}"
BIN="$DEST/bin"
PRE="${HUDNAV_LD_PRELOAD_FILE:-/etc/ld.so.preload}"
AA="$BIN/aa_shim.so"
CP="$BIN/cp_hook.so"
NOOP="$BIN/noop.so"
BK="$DEST/backup"
REMOUNT="${HUDNAV_REMOUNT:-yes}"

die() { echo "FAIL: $*" >&2; exit 1; }
need_root() { [ "$(id -u)" = 0 ] || die "must run as root"; }
need_yes() { [ "${1:-}" = "--yes" ] || die "refusing without --yes (this edits $PRE)"; }

show_status() {
    echo "preload file: $PRE"
    if [ -e "$PRE" ]; then ls -l "$PRE"; sed 's/^/  | /' "$PRE"; else echo "  (absent)"; fi
    for f in "$AA" "$CP" "$NOOP"; do
        if [ -f "$f" ]; then sha256sum "$f"; else echo "  missing: $f"; fi
    done
    grep ' / ' /proc/mounts | head -1
}

# Commands that must behave normally with a candidate library loaded. Includes processes that do NOT
# load libdl (the failure of the old cp_hook.so): busybox applets, sh, awk, ls, rm...
preflight_one() {
    lib="$1"
    [ -f "$lib" ] || { echo "  $lib: MISSING"; return 1; }
    bad=0
    for cmd in "ls /" "cat /proc/version" "sh -c true" "awk BEGIN{print(42)}" "grep -c root /etc/passwd" \
               "sha256sum /bin/sh" "date +%Y" "readlink /proc/self/exe" "head -c 4 /bin/sh" "sort /etc/passwd" \
               "tr a-z A-Z" "wc -c /etc/passwd" "stat /bin/sh" "id" "mkdir -p /tmp/.hn_pf" "rm -rf /tmp/.hn_pf"; do
        out=$(LD_PRELOAD="$lib" sh -c "$cmd" 2>&1 </dev/null)
        rc=$?
        case "$out" in
            *"relocation error"*|*"cannot be preloaded"*|*"symbol lookup error"*|*"ERROR: ld.so"*|*"cannot open shared object"*)
                echo "  BAD  [$cmd] $(echo "$out" | head -1)"; bad=1 ;;
        esac
        if [ "$rc" -ge 126 ] && [ "$rc" -le 255 ] && [ "$cmd" != "tr a-z A-Z" ]; then
            echo "  BAD  [$cmd] rc=$rc"; bad=1
        fi
    done
    # same battery must give the same stdout/rc as without the library
    for cmd in "ls /" "sha256sum /bin/sh" "awk BEGIN{print(42)}" "readlink /proc/self/exe"; do
        a=$(sh -c "$cmd" 2>/dev/null </dev/null); ra=$?
        b=$(LD_PRELOAD="$lib" sh -c "$cmd" 2>/dev/null </dev/null); rb=$?
        [ "$a" = "$b" ] && [ "$ra" = "$rb" ] || { echo "  BAD  [$cmd] differs with the library"; bad=1; }
    done
    [ "$bad" = 0 ] && echo "  OK   $lib" || echo "  FAIL $lib"
    return $bad
}

preflight() {
    echo "== preflight (read-only, env LD_PRELOAD only; nothing system-wide is touched) =="
    rc=0
    for lib in "$@"; do preflight_one "$lib" || rc=1; done
    return $rc
}

write_preload() {   # write_preload LINE...   (remounts rw only for the write)
    mkdir -p "$BK"
    # Record the ORIGINAL state exactly once (the first time this tool writes), never a state that
    # an earlier run of this tool produced (e.g. the canary list).
    if [ ! -e "$BK/ld.so.preload.before" ] && [ ! -e "$BK/ld.so.preload.was-absent" ]; then
        if [ -e "$PRE" ]; then cp "$PRE" "$BK/ld.so.preload.before"; else : > "$BK/ld.so.preload.was-absent"; fi
    fi
    if [ "$REMOUNT" = yes ]; then
        mount -o remount,rw / 2>/dev/null || die "cannot remount / rw"
        trap 'mount -o remount,ro / 2>/dev/null || true' EXIT INT TERM HUP
    fi
    tmp="$PRE.hudnav.$$"
    printf '%s\n' "$@" > "$tmp" || { rm -f "$tmp"; die "write failed"; }
    chmod 644 "$tmp"     # every process (not only root) must be able to read it
    mv "$tmp" "$PRE" || { rm -f "$tmp"; die "rename failed"; }
    sync
    if [ "$REMOUNT" = yes ]; then mount -o remount,ro / 2>/dev/null || true; trap - EXIT INT TERM HUP; fi
}

case "${1:-status}" in
    status) show_status ;;
    preflight)
        [ -f "$NOOP" ] || echo "(noop.so not installed; canary unavailable)"
        preflight "$AA" "$CP" ;;
    canary)
        need_root; need_yes "${2:-}"
        [ -f "$NOOP" ] || die "missing $NOOP"
        preflight "$NOOP" || die "canary failed preflight"
        write_preload "$NOOP"
        echo "canary listed; REBOOT, then: sh $0 verify   (and check SSH still works)"; show_status ;;
    enable)
        need_root; need_yes "${2:-}"
        preflight "$AA" "$CP" || die "preflight failed; NOT enabling"
        [ -e "$BK/canary.ok" ] || die "run canary and confirm it survived a reboot first (touch $BK/canary.ok)"
        write_preload "$AA" "$CP"
        echo "shims listed; REBOOT, then: sh $0 verify"; show_status ;;
    disable)
        need_root; need_yes "${2:-}"
        if [ "$REMOUNT" = yes ]; then
            mount -o remount,rw / 2>/dev/null || die "cannot remount / rw"
            trap 'mount -o remount,ro / 2>/dev/null || true' EXIT INT TERM HUP
        fi
        if [ -e "$BK/ld.so.preload.before" ]; then cp "$BK/ld.so.preload.before" "$PRE"; else rm -f "$PRE"; fi
        sync
        if [ "$REMOUNT" = yes ]; then mount -o remount,ro / 2>/dev/null || true; trap - EXIT INT TERM HUP; fi
        echo "removed"; show_status ;;
    verify)
        show_status
        # This very shell started after the canary was listed: if the canary is mapped in it, the
        # mechanism works and the system survived a boot with it. That unlocks `enable`.
        if grep -q 'noop\.so' "/proc/$$/maps" 2>/dev/null; then
            mkdir -p "$BK"; : > "$BK/canary.ok"
            echo "canary is mapped into this shell: mechanism works (canary.ok written)"
        fi
        for p in AppProjection iAP2Service-2.0; do
            pid=$(pgrep -x "$p" 2>/dev/null | head -1)
            if [ -z "$pid" ]; then echo "$p: not running"; continue; fi
            sec=$(od -A n -t u8 -w16 "/proc/$pid/auxv" 2>/dev/null | awk '$1==23{print $2}')
            echo "$p pid=$pid label=$(cat /proc/$pid/attr/current 2>/dev/null) AT_SECURE=${sec:-?}" \
                 "aa_shim_maps=$(grep -c aa_shim "/proc/$pid/maps" 2>/dev/null) cp_hook_maps=$(grep -c cp_hook "/proc/$pid/maps" 2>/dev/null)" \
                 "noop_maps=$(grep -c noop.so "/proc/$pid/maps" 2>/dev/null)"
        done ;;
    *) echo "usage: sh ld-preload.sh status|preflight|canary|enable|disable|verify [--yes]" >&2; exit 2 ;;
esac
