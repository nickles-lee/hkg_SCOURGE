#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# TARGET: AU Linux Gen5W 260319 head unit, run as root.
#
# Enable / disable the Android Auto interposer the reversible way found in
# docs/hudnav/AA_PROTOCOL.md:
#
#   /app/bin/AppLauncher.sh already exports LD_PRELOAD=/usr/lib/libEGL.so.1
#   AppLauncher passes QProcessEnvironment::systemEnvironment() to every child
#   and never removes LD_PRELOAD, so appending aa_shim.so there loads it into
#   AppProjection without patching any signed ELF. aa_shim.so self-gates on
#   /proc/self/exe == AppProjection, so other launcher children stay inert.
#
# Only that one line is changed. A byte-identical backup is kept under
# /rw_data/hudnav/backup, and `disable` restores it.
#
# Usage: sh aa-preload.sh status|enable|disable|verify [--yes]
set -eu
PATH="${HUDNAV_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"

WRAP=/app/bin/AppLauncher.sh
SHIM=/rw_data/hudnav/bin/aa_shim.so
BK=/rw_data/hudnav/backup
STOCK_LINE='export LD_PRELOAD=/usr/lib/libEGL.so.1'
PATCH_LINE="export LD_PRELOAD=/usr/lib/libEGL.so.1:$SHIM"

die() { echo "FAIL: $*" >&2; exit 1; }
need_root() { [ "$(id -u)" = 0 ] || die "must run as root"; }
need_yes() { [ "${1:-}" = "--yes" ] || die "refusing without --yes (changes /app)"; }

stock_line_present() { grep -qxF "$STOCK_LINE" "$WRAP" 2>/dev/null; }
patch_line_present() { grep -qxF "$PATCH_LINE" "$WRAP" 2>/dev/null; }

rw_root() {
    mount -o remount,rw / 2>/dev/null || die "cannot remount / rw"
    trap 'mount -o remount,ro / 2>/dev/null || true' EXIT INT TERM HUP
}

show_status() {
    echo "wrapper: $WRAP"
    [ -f "$WRAP" ] || { echo "  MISSING"; return; }
    grep -n '^export LD_PRELOAD=' "$WRAP" || echo "  (no LD_PRELOAD line)"
    echo "shim:    $SHIM"
    if [ -f "$SHIM" ]; then sha256sum "$SHIM"; else echo "  MISSING"; fi
    echo "backups: $BK"
    ls -l "$BK"/AppLauncher.sh.* 2>/dev/null || echo "  (none)"
}

verify() {
    show_status
    pid=$(pgrep -x AppProjection 2>/dev/null | head -1 || true)
    if [ -z "$pid" ]; then
        echo "AppProjection is not running; reboot and re-run 'verify'"
        return 0
    fi
    echo "--- AppProjection pid=$pid ---"
    tr '\0' '\n' < "/proc/$pid/environ" | grep '^LD_PRELOAD=' || echo "  (no LD_PRELOAD)"
    n=$(grep -c 'aa_shim\.so' "/proc/$pid/maps" 2>/dev/null || true)
    echo "aa_shim mappings: ${n:-0}"
    [ "${n:-0}" -gt 0 ] || echo "WARNING: shim is not mapped into AppProjection"
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
        need_yes "${2:-}"
        [ -f "$WRAP" ] || die "missing $WRAP"
        [ -f "$SHIM" ] || die "missing $SHIM (run install-on-hu.sh first)"
        if patch_line_present; then
            echo "already enabled:"
            grep -n '^export LD_PRELOAD=' "$WRAP"
            exit 0
        fi
        stock_line_present || die "unexpected LD_PRELOAD line; refusing to edit"

        mkdir -p "$BK"
        ts=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null || echo manual)
        cp "$WRAP" "$BK/AppLauncher.sh.$ts"
        if [ ! -f "$BK/AppLauncher.sh.stock" ]; then
            cp "$WRAP" "$BK/AppLauncher.sh.stock"
            ( cd "$BK" && sha256sum AppLauncher.sh.stock > AppLauncher.sh.stock.sha256 )
        fi

        rw_root
        tmp=$(mktemp /tmp/AppLauncher.XXXXXX)
        sed "s|^export LD_PRELOAD=.*$|$PATCH_LINE|" "$WRAP" > "$tmp"
        grep -qxF "$PATCH_LINE" "$tmp" || die "sed did not produce the expected line"
        cat "$tmp" > "$WRAP"        # keep the inode (and its SMACK attributes)
        rm -f "$tmp"
        mount -o remount,ro / 2>/dev/null || true
        trap - EXIT INT TERM HUP
        echo "enabled; reboot required for AppLauncher to re-export the environment"
        grep -n '^export LD_PRELOAD=' "$WRAP"
        ;;
    disable)
        need_root
        need_yes "${2:-}"
        [ -f "$BK/AppLauncher.sh.stock" ] || die "no stock backup in $BK"
        rw_root
        cat "$BK/AppLauncher.sh.stock" > "$WRAP"
        mount -o remount,ro / 2>/dev/null || true
        trap - EXIT INT TERM HUP
        stock_line_present || die "restore failed: stock line not present"
        echo "disabled; reboot required"
        grep -n '^export LD_PRELOAD=' "$WRAP"
        ;;
    *)
        echo "usage: sh aa-preload.sh status|enable|disable|verify [--yes]" >&2
        exit 2
        ;;
esac
