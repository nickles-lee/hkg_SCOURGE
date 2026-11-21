#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# rig-ap-fallback.sh — if the rig cannot join any known network, become one.
#
# Runs on the rig as a service. Every AP_CHECK_INTERVAL seconds it asks
# NetworkManager whether wlan0 is associated. After AP_GRACE_SECONDS of
# uninterrupted failure it raises a WPA2 access point (NetworkManager's own
# shared mode, no hostapd config to rot), where rig-wifi-agent.py serves the
# setup page at 10.42.0.1. As soon as the radio associates — which is what the
# setup page causes — the AP is torn down again.
#
# The AP profile is never autoconnected and is deleted on the way out, so a
# later boot goes straight back to station mode.
set -uo pipefail

INTERVAL="${AP_CHECK_INTERVAL:-15}"
GRACE="${AP_GRACE_SECONDS:-90}"
AP_CONN="${AP_CONNECTION_NAME:-rig-setup-ap}"
: "${RIG_AP_SSID:?RIG_AP_SSID is not set (see /etc/rig/wifi.env)}"
: "${RIG_AP_PASSPHRASE:?RIG_AP_PASSPHRASE is not set (see /etc/rig/wifi.env)}"

log() { echo "rig-ap-fallback: $*"; }

associated() {
    # 100 = activated (associated, whatever the IP state); 30/40 = connecting.
    nmcli -t -f GENERAL.STATE device show wlan0 2>/dev/null |
        grep -q '^GENERAL.STATE:100'
}

ap_active() {
    nmcli -t -f NAME connection show --active 2>/dev/null | grep -qx -- "$AP_CONN"
}

start_ap() {
    ap_active && return 0
    nmcli connection delete "$AP_CONN" >/dev/null 2>&1 || true
    log "no known network after ${GRACE}s — raising AP '$RIG_AP_SSID'"
    if nmcli device wifi hotspot ifname wlan0 con-name "$AP_CONN" \
            ssid "$RIG_AP_SSID" password "$RIG_AP_PASSPHRASE"; then
        nmcli connection modify "$AP_CONN" connection.autoconnect no \
            >/dev/null 2>&1 || true
        log "AP up at 10.42.0.1 — open http://10.42.0.1/ to pick a network"
        return 0
    fi
    log "could not raise the AP (dnsmasq-base installed? radio unblocked?)"
    return 1
}

stop_ap() {
    ap_active || { nmcli connection delete "$AP_CONN" >/dev/null 2>&1 || true; return 0; }
    log "associated — tearing the AP down"
    nmcli connection down "$AP_CONN" >/dev/null 2>&1 || true
    nmcli connection delete "$AP_CONN" >/dev/null 2>&1 || true
}

failures=0
log "watching wlan0 every ${INTERVAL}s (grace ${GRACE}s)"
while true; do
    if associated; then
        failures=0
        stop_ap
    else
        failures=$((failures + 1))
        if [ $((failures * INTERVAL)) -ge "$GRACE" ]; then
            start_ap || true
        fi
    fi
    sleep "$INTERVAL"
done
