#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: AU Linux Gen5W 260319
set -eu
cd "$(dirname "$0")"
kit=$PWD
target_root=${HUDNAV_TARGET_ROOT:-/}
(cd "$target_root" && sha256sum -c "$kit/firmware.sha256")
sha256sum -c package.sha256
[ "${1:-}" != "--check" ] || exit 0
[ "$#" -eq 0 ] || { echo "usage: sh run-aa-diagnostic.sh [--check]" >&2; exit 2; }

umask 077
session_bus=${HUDNAV_SESSION_ADDRESS:-}
if [ -z "$session_bus" ]; then
    session_bus=$(sed 's/,guid=.*//' /tmp/session_appmgr)
fi
[ -n "$session_bus" ] || { echo "AppManager bus address unavailable" >&2; exit 1; }
run=$(mktemp -d /tmp/hudnav-aa-log.XXXXXXXX)
fifo=$run/owners
mkfifo "$fifo"
monitor=
receiver=
cleanup() {
    for pid in "$receiver" "$monitor"; do
        if [ -n "$pid" ]; then
            kill "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
        fi
    done
    rm -f "$fifo" "$run/hudnav.sock"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
dbus-monitor --address "$session_bus" \
    "type='signal',sender='com.lge.hyundai.avn.appmanager',path='/Information',interface='com.lge.avn.appmanager.info',member='navi_route_guide_application'" \
    "type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='com.lge.hyundai.avn.appmanager'" \
    >"$fifo" 2>"$run/monitor.log" &
monitor=$!
"$kit/hudnavd" --appmanager-stdin --socket "$run/hudnav.sock" <"$fifo" >"$run/navigation.log" 2>&1 &
receiver=$!
printf 'HUDNAV_SOCK=%s\nAA_SHIM=%s\nLOG=%s\n' "$run/hudnav.sock" "$kit/aa_shim.so" "$run/navigation.log"
wait "$receiver"
