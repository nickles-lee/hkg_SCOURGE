#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# TARGET: AU Linux Gen5W 260319 head unit, run as root.
#
# Enable/disable the CarPlay iAP2 interposer the reversible way: one added
# `Environment=LD_PRELOAD=` line in the DATA-ONLY systemd unit
# /lib/systemd/system/carplay_iAP2.service. No signed ELF is patched.
# cp_hook.so self-gates on /proc/self/exe == iAP2Service-2.0.
#
# The stock unit is backed up byte-for-byte; `disable` restores it.
# Service restart is a separate explicit step (--restart) so the first pass
# can be reviewed before anything is stopped.
#
# Usage: sh cp-preload.sh status|enable|disable|verify [--yes] [--restart]
set -eu
PATH="${HUDNAV_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"

UNIT=/lib/systemd/system/carplay_iAP2.service
SHIM=/rw_data/hudnav/bin/cp_hook.so
BK=/rw_data/hudnav/backup
LINE="Environment=LD_PRELOAD=$SHIM"

die() { echo "FAIL: $*" >&2; exit 1; }
need_root() { [ "$(id -u)" = 0 ] || die "must run as root"; }
line_present() { grep -qxF "$LINE" "$UNIT" 2>/dev/null; }

rw_root() {
    mount -o remount,rw / 2>/dev/null || die "cannot remount / rw"
    trap 'mount -o remount,ro / 2>/dev/null || true' EXIT INT TERM HUP
}

restart_service() {
    command -v systemctl >/dev/null 2>&1 || { echo "systemctl missing; restart manually" >&2; return 0; }
    systemctl daemon-reload 2>/dev/null || true
    systemctl restart carplay_iAP2.service 2>/dev/null || echo "WARNING: restart failed" >&2
}

show_status() {
    echo "unit: $UNIT"
    [ -f "$UNIT" ] || { echo "  MISSING"; return; }
    grep -nE '^\[Service\]|^Environment=|^ExecStart=' "$UNIT" || true
    echo "shim: $SHIM"
    if [ -f "$SHIM" ]; then sha256sum "$SHIM"; else echo "  MISSING"; fi
    echo "backups: $BK"
    ls -l "$BK"/carplay_iAP2.service.* 2>/dev/null || echo "  (none)"
}

verify() {
    show_status
    pid=$(pgrep -x iAP2Service-2.0 2>/dev/null | head -1 || true)
    if [ -z "$pid" ]; then
        echo "iAP2Service-2.0 is not running (CarPlay may be idle); re-check with a phone connected"
        return 0
    fi
    echo "--- iAP2Service pid=$pid ---"
    tr '\0' '\n' < "/proc/$pid/environ" | grep '^LD_PRELOAD=' || echo "  (no LD_PRELOAD)"
    n=$(grep -c 'cp_hook\.so' "/proc/$pid/maps" 2>/dev/null || true)
    echo "cp_hook mappings: ${n:-0}"
    [ "${n:-0}" -gt 0 ] || echo "WARNING: shim is not mapped into iAP2Service"
}

case "${1:-status}" in
    status)
        show_status
        ;;
    verify)
        verify
        ;;
    enable)
        need_root
        [ "${2:-}" = "--yes" ] || [ "${3:-}" = "--yes" ] || die "refusing without --yes (changes /lib/systemd)"
        [ -f "$UNIT" ] || die "missing $UNIT"
        [ -f "$SHIM" ] || die "missing $SHIM (run install-on-hu.sh first)"
        if line_present; then echo "already enabled:"; grep -n '^Environment=LD_PRELOAD=' "$UNIT"; exit 0; fi
        grep -q '^Environment=LD_LIBRARY_PATH=' "$UNIT" || die "unexpected unit layout; refusing"

        mkdir -p "$BK"
        ts=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || echo manual)
        cp "$UNIT" "$BK/carplay_iAP2.service.$ts"
        if [ ! -f "$BK/carplay_iAP2.service.stock" ]; then
            cp "$UNIT" "$BK/carplay_iAP2.service.stock"
            ( cd "$BK" && sha256sum carplay_iAP2.service.stock > carplay_iAP2.service.stock.sha256 )
        fi

        rw_root
        tmp=$(mktemp /tmp/carplay_iAP2.XXXXXX)
        awk -v ins="$LINE" '{ print } /^Environment=LD_LIBRARY_PATH=/ && !done { print ins; done=1 }' \
            "$UNIT" > "$tmp"
        grep -qxF "$LINE" "$tmp" || die "awk did not insert the expected line"
        cat "$tmp" > "$UNIT"
        rm -f "$tmp"
        mount -o remount,ro / 2>/dev/null || true
        trap - EXIT INT TERM HUP
        echo "enabled in unit (not restarted):"
        grep -n '^Environment=LD_PRELOAD=' "$UNIT"
        case "${3:-}" in --restart) restart_service ;; esac
        ;;
    disable)
        need_root
        [ "${2:-}" = "--yes" ] || die "refusing without --yes"
        [ -f "$BK/carplay_iAP2.service.stock" ] || die "no stock backup in $BK"
        rw_root
        cat "$BK/carplay_iAP2.service.stock" > "$UNIT"
        mount -o remount,ro / 2>/dev/null || true
        trap - EXIT INT TERM HUP
        ! line_present || die "restore failed: preload line still present"
        echo "disabled (not restarted)"
        case "${3:-}" in --restart) restart_service ;; esac
        ;;
    *)
        echo "usage: sh cp-preload.sh status|enable|disable|verify [--yes] [--restart]" >&2
        exit 2
        ;;
esac
