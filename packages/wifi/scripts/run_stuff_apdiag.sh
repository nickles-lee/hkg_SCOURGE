#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Opt-in, read-only AP trace. Never enables WLAN, restarts it, or changes policy.
# D-Bus profile mode captures headers ONLY, not credential-bearing bodies.
set -eu
PATH=/bin:/sbin:/usr/bin:/usr/sbin
umask 077
USB_DIR=${USB_DIR:-/media/usb}
command -v timeout >/dev/null || { echo 'AP diagnostic requires timeout; no trace started' >&2; exit 1; }
command -v dbus-monitor >/dev/null || { echo 'dbus-monitor unavailable; no trace started' >&2; exit 1; }
# Defense in depth if a vendor monitor ignores --profile: never save normal
# message bodies. The timed outer shell owns both pipeline children.
if [ "${1:-}" = '--headers' ]; then
    shift
    dbus-monitor --system --profile "$@" |
        awk '$1=="mc" || $1=="#type" { print; fflush() }'
    exit
fi
# Bound the whole diagnostic too, including process discovery and snapshots.
if [ "${1:-}" != '--bounded' ]; then
    exec timeout -k 2 90 sh "$0" --bounded
fi
OUT="$USB_DIR/wifi_apdiag_$(date -u +%Y%m%d%H%M%S)_$$"
mkdir "$OUT"
monitor=
cleanup() {
    if [ -n "$monitor" ]; then kill "$monitor" 2>/dev/null || true; wait "$monitor" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM

run() {
    echo "\$ $*"
    timeout -k 1 2 "$@" 2>&1 || echo "command unavailable/failed/timed out: rc=$?"
}
processes() {
    inspected=0
    for proc in /proc/[0-9]*; do
        inspected=$((inspected + 1))
        if [ "$inspected" -gt 1024 ]; then echo 'process discovery truncated at 1024 entries'; break; fi
        exe=$(readlink "$proc/exe" 2>/dev/null) || continue
        case "${exe##*/}" in
            connmand|wpa_supplicant|supplicantmanager|wifi_daemon|dropbear|dropbearmulti)
                echo "pid=${proc##*/} exe=$exe"
                cat "$proc/attr/current" 2>/dev/null || true
                grep -E '^(Name|Pid|PPid|Uid|Gid|CapInh|CapPrm|CapEff|CapBnd|Seccomp):' "$proc/status" 2>/dev/null || true
                ;;
        esac
    done
}
snapshot() {
    date -u
    cat /proc/uptime
    run iw dev wlan0 info
    run iwconfig wlan0
    run ifconfig wlan0
    run wpa_cli -p /var/run/wpa_supplicant -i wlan0 get ap_scan
    # Whitelists avoid dumping technology passphrases or arbitrary status keys.
    timeout -k 1 2 connmanctl technologies 2>&1 | awk '
        /^\/net\/connman\/technology\// { wifi=($0=="/net/connman/technology/wifi"); if(wifi) print; next }
        wifi && /^[[:space:]]*(Powered|Connected|Tethering|TetheringIdentifier) = / { print }' || true
    timeout -k 1 2 wpa_cli -p /var/run/wpa_supplicant -i wlan0 status 2>&1 |
        awk '/^(mode|wpa_state|ssid|ip_address|freq|bssid)=/ { print }' || true
    processes
}
{
    echo 'WIFI_APDIAG read-only; no activation or automatic restart'
    echo 'get ap_scan and D-Bus profile support are vendor-dependent; failures are not state evidence'
    echo 'Caller resolution is best-effort: short-lived/disconnected bus names may be unresolvable'
    printf 'build_id='
    sed -n 's/.*"build_id":"\([0-9a-f]*\)".*/\1/p' /rw_data/wifi/status.json 2>/dev/null || true
} > "$OUT/README.txt"

snapshot > "$OUT/before.txt"
echo 'Capturing 25 seconds of AP method headers. Use Restart in the WiFi app only if desired.'
# Never fall back to ordinary monitor output: SetProperty bodies can contain keys.
timeout -k 1 25 sh "$0" --headers \
    "type='method_call',interface='fi.w1.wpa_supplicant1.Interface',member='Disconnect'" \
    "type='method_call',interface='fi.w1.wpa_supplicant1',member='RemoveInterface'" \
    "type='method_call',interface='fi.w1.wpa_supplicant1',member='CreateInterface'" \
    "type='method_call',interface='fi.epitest.hostap.WPASupplicant.Interface',member='disconnect'" \
    "type='method_call',interface='net.connman.Technology',member='SetProperty',path='/net/connman/technology/wifi'" \
    "type='method_call',interface='org.freedesktop.DBus.Properties',member='Set',path='/net/connman/technology/wifi'" \
    > "$OUT/dbus.headers" 2> "$OUT/dbus.errors" &
monitor=$!
# Resolve senders while the monitor is running, not just after it exits.
# At most 24 lookups, each with a 500ms D-Bus / 1s process deadline.
: > "$OUT/seen"
: > "$OUT/callers.txt"
lookups=0
for tick in 1 2 3 4 5 6 7 8 9 10 11 12; do
    sleep 2
    awk '$1=="mc" { for(i=2;i<=NF;i++) if($i ~ /^:[0-9]+\.[0-9]+$/) {print $i; break} }' "$OUT/dbus.headers" |
        sort -u > "$OUT/names"
    while IFS= read -r name; do
        [ "$lookups" -lt 24 ] || break
        grep -Fxq "$name" "$OUT/seen" && continue
        echo "$name" >> "$OUT/seen"
        lookups=$((lookups + 1))
        reply=$(timeout -k 1 1 dbus-send --system --print-reply --reply-timeout=500 \
            --dest=org.freedesktop.DBus /org/freedesktop/DBus \
            org.freedesktop.DBus.GetConnectionUnixProcessID "string:$name" 2>/dev/null) || reply=
        pid=$(printf '%s\n' "$reply" | awk '$1=="uint32" && $2 ~ /^[0-9]+$/ {print $2; exit}')
        {
            echo "bus=$name pid=${pid:-unknown}"
            if [ -n "$pid" ]; then
                readlink "/proc/$pid/exe" 2>/dev/null || true
                cat "/proc/$pid/attr/current" 2>/dev/null || true
                grep -E '^(Name|Pid|PPid|Uid|Gid|CapEff):' "/proc/$pid/status" 2>/dev/null || true
            fi
        } >> "$OUT/callers.txt"
    done < "$OUT/names"
done
wait "$monitor" || echo 'monitor ended (timeout after 25s is expected; inspect dbus.errors)' >> "$OUT/README.txt"
monitor=
snapshot > "$OUT/after.txt"
echo "wrote $OUT (sensitive network/process metadata; no message bodies)"
