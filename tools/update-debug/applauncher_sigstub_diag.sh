# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#!/bin/sh
# APPLAUNCHER_SIGSTUB_DIAG_REV=1
# Read-only diagnostics for applauncher_sigstub.  Rename to run_stuff.sh on
# the stick and plug it in; writes applauncher_sigstub_diag.txt to the stick
# root.  Changes nothing on the head unit.
set -u
PATH=/bin:/sbin:/usr/bin:/usr/sbin
USB_DIR="${USB_DIR:-$(CDPATH= cd -- "$(dirname "$0")" && pwd)}"
F="${APPLAUNCHER_BIN:-/app/bin/AppLauncher}"
OUT="$USB_DIR/applauncher_sigstub_diag.txt"

mount -o remount,rw "$USB_DIR" 2>/dev/null || true

{
    echo "APPLAUNCHER_SIGSTUB_DIAG"
    echo "time=$(date -u 2>/dev/null)"
    echo "usb=$USB_DIR"
    echo "uid=$(id -u 2>/dev/null)"
    echo "target=$F"
    if [ -f "$F" ]; then
        echo "size=$(stat -c%s "$F" 2>/dev/null)"
        echo "md5=$(md5sum "$F" 2>/dev/null | cut -d' ' -f1)"
        b=$(dd if="$F" bs=1 skip=88912 count=8 2>/dev/null | od -An -tx1 | tr -d ' \n')
        echo "FUN_00115b50@88912=$b (stock 4157be1000000041 stub b801000000c30041)"
        b=$(dd if="$F" bs=1 skip=73984 count=8 2>/dev/null | od -An -tx1 | tr -d ' \n')
        echo "FUN_00112100@73984=$b (stock 4157415641554154 stub b801000000c34154)"
    else
        echo "target=MISSING"
    fi
    echo "--- launcher processes ---"
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        if [ "$(cat "$p/comm" 2>/dev/null)" = "AppLauncher" ]; then
            pid=${p#/proc/}
            echo "pid=$pid start=$(cat "$p/stat" 2>/dev/null | cut -d' ' -f22)"
        fi
    done
    echo "--- bus ownership ---"
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        if [ "$(cat "$p/comm" 2>/dev/null)" = "AppLauncher" ]; then
            pid=${p#/proc/}
            sess=$(tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | \
                   grep '^DBUS_SESSION_BUS_ADDRESS=' | head -n 1 | cut -d= -f2-)
            if [ -n "$sess" ] && command -v dbus-send >/dev/null 2>&1; then
                if DBUS_SESSION_BUS_ADDRESS="$sess" dbus-send --session --print-reply \
                        --dest=org.freedesktop.DBus / org.freedesktop.DBus.NameHasOwner \
                        string:com.lge.avn.applauncher 2>/dev/null | grep -q 'boolean true'; then
                    echo "com.lge.avn.applauncher=owned"
                else
                    echo "com.lge.avn.applauncher=not-owned"
                fi
            else
                echo "com.lge.avn.applauncher=unknown (no session bus)"
            fi
            break
        fi
    done
    echo "--- key files ---"
    ls -la /tmp/app_normal.key /tmp/navi_public.key 2>/dev/null || echo "no /tmp key files"
    echo "--- recent spawn/CheckSign log lines ---"
    for p in /proc/[0-9]*; do
        [ -r "$p/comm" ] || continue
        if [ "$(cat "$p/comm" 2>/dev/null)" = "AppLauncher" ]; then
            pid=${p#/proc/}
            echo "(stdout of pid $pid is not captured here; see"
            echo " /rw_data/applauncher_sigstub/launcher.out after a respawn)"
            break
        fi
    done
    [ -f /rw_data/applauncher_sigstub/launcher.out ] && {
        echo "--- launcher.out tail ---"
        tail -40 /rw_data/applauncher_sigstub/launcher.out 2>/dev/null
    }
    echo "--- backups ---"
    ls -la /rw_data/applauncher_sigstub/ 2>/dev/null || echo "no /rw_data/applauncher_sigstub"
    echo "status=OK"
} > "$OUT" 2>&1

sync
exit 0
