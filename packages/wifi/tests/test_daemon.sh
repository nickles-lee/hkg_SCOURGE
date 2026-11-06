#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Daemon smoke test (no hardware, no root required).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BIN="$ROOT/src/wifi_daemon"
[ -x "$BIN" ] || make -C "$ROOT/src"

out=$("$BIN" --status 2>/dev/null)
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] is True; assert d["iface"]=="wlan0"; assert "ssh_enabled" in d; assert "ssh_port_fallback" in d; assert "vendor_ssh_22" in d; assert "secure_console" in d; assert "debug_console" in d; assert "bt_discoverable" in d; assert "bt_pairable" in d; assert "pair_message" in d; assert "applying" in d; assert "home_seq" in d; assert "home_epoch" in d; assert "home_monitor" in d; assert "home_source" in d; assert "last_app_key" in d'

imp=$("$BIN" --import-file "$ROOT/wifi.txt.sample" 2>/dev/null)
echo "$imp" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] is True; assert d["ssid"]=="MyHomeWiFi"; assert d["carplay_ssid"]=="IONIQ5-CarPlay"'

echo "PASS: daemon smoke test"
