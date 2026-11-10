#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Compile first-party source with a private APP_DIR; imports never touch /etc.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d /tmp/opencode/wifi-daemon-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
gcc -O1 -DWIFI_TEST -DAPP_DIR="\"$TMP/state\"" "$ROOT/src/wifi_daemon.c" -o "$TMP/daemon"
out=$("$TMP/daemon" --status 2>/dev/null)
printf '%s\n' "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] and d["iface"]=="wlan0"; assert "build_id" in d and "home_epoch" in d'
imp=$("$TMP/daemon" --import-file "$ROOT/wifi.txt.sample" 2>/dev/null)
printf '%s\n' "$imp" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] and d["ssid"]=="MyHomeWiFi"'
grep -qx 'mode=off' "$TMP/state/config.conf"
grep -qx 'wlan_on=0' "$TMP/state/config.conf"
grep -qx 'ssh=0' "$TMP/state/config.conf"
echo 'PASS: isolated daemon queries/import; activation remains manual'
