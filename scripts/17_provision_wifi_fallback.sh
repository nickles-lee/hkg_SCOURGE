#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 17_provision_wifi_fallback.sh — make the rig findable when it cannot find WiFi.
#
# Run ON the rig as root:  sudo bash 17_provision_wifi_fallback.sh
#
# Installs rig-ap-fallback.service (raise a setup AP after a grace period with no
# association) and rig-wifi-agent.service (the page served on that AP at
# 10.42.0.1), plus the two values they need in /etc/rig/wifi.env, mode 0600:
# RIG_AP_SSID and RIG_AP_PASSPHRASE. Those come from config/rig.local.env or the
# environment — never from this file.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

LOCAL="$SCOURGE_ROOT/config/rig.local.env"
# shellcheck disable=SC1090
if [ -f "$LOCAL" ]; then source "$LOCAL"; fi

[ -n "${RIG_AP_SSID:-}" ] || {
    echo "set RIG_AP_SSID in config/rig.local.env (the fallback AP name)" >&2
    exit 1
}
[ -n "${RIG_AP_PASSPHRASE:-}" ] || {
    echo "set RIG_AP_PASSPHRASE in config/rig.local.env" >&2
    exit 1
}
[ "${#RIG_AP_PASSPHRASE}" -ge 8 ] || {
    echo "RIG_AP_PASSPHRASE must be at least 8 characters (WPA2)" >&2
    exit 1
}

PI="$SCOURGE_ROOT/tools/rigdash/pi"
[ -f "$PI/rig_wifi_agent.py" ] || {
    echo "missing $PI/rig_wifi_agent.py — deploy tools/rigdash/pi/ first" >&2
    exit 1
}

echo "== packages"
apt-get update -qq
# NetworkManager's shared mode runs dnsmasq for DHCP/DNS on the AP.
apt-get install -y -qq dnsmasq-base

echo "== install"
install -D -m 0755 "$PI/rig_wifi_agent.py" /usr/local/lib/rig/rig_wifi_agent.py
install -D -m 0755 "$PI/rig-ap-fallback.sh" /usr/local/sbin/rig-ap-fallback
install -D -m 0644 "$PI/rig-ap-fallback.service" /etc/systemd/system/rig-ap-fallback.service
install -D -m 0644 "$PI/rig-wifi-agent.service" /etc/systemd/system/rig-wifi-agent.service
install -d -m 0755 /etc/rig
umask 077
# Two values only: the AP's name and its passphrase. There is no agent token any
# more — the page is reachable over the owner's LAN or this AP, and gating it
# bought a field to retype on a phone. A RIG_AGENT_TOKEN left over from an older
# provisioning is simply ignored (and dropped the next time this runs).
printf 'RIG_AP_SSID=%s\nRIG_AP_PASSPHRASE=%s\n' \
    "$RIG_AP_SSID" "$RIG_AP_PASSPHRASE" >/etc/rig/wifi.env
chmod 0600 /etc/rig/wifi.env

echo "== enable"
systemctl daemon-reload
systemctl enable --now rig-ap-fallback.service rig-wifi-agent.service
systemctl restart rig-wifi-agent.service

echo
systemctl --no-pager --lines=0 status rig-ap-fallback.service rig-wifi-agent.service \
    | grep -E '^(●|Active:)' || true
echo "setup AP: '$RIG_AP_SSID' at 10.42.0.1 after ${AP_GRACE_SECONDS:-90}s without a known network"
echo "then:     http://10.42.0.1/ — pick the network, type its passphrase"
