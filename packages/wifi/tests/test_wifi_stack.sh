#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Static checks for the supplicant, AP and Bluetooth pairing implementation.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
SRC="$ROOT/src/wifi_daemon.c"
PAGE="$ROOT/artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/WifiPage.qml"
DIAG="$ROOT/scripts/run_stuff_diag.sh"

for needle in \
    "ensure_wpa_supplicant" \
    "strlen(\"DBUS_SESSION_BUS_ADDRESS=\")" \
    "ensure_wpa_conf" \
    "Stop the CarPlay AP before scanning" \
    "rebinding SSH" \
    "ensure_wifi_ready" \
    "wpa_cli -i wlan0 add_network" \
    "wpa_state=COMPLETED" \
    "tether 5GHz" \
    "bt_pair_start" \
    "set%sAttribute" \
    "bt_set_pairing" \
    "session_bus_address" \
    "/carplay/pair" \
    "/carplay/pair/stop"; do
    grep -q "$needle" "$SRC" || { echo "missing in daemon: $needle" >&2; exit 1; }
done

# Regression guards for the reviewed bugs.
grep -q "p += 23" "$SRC" && { echo "session bus off-by-2 is back" >&2; exit 1; }
grep -q "ctrl_interface=/var/run/wpa_supplicant" "$SRC" || { echo "own wpa conf missing" >&2; exit 1; }
grep -q '"/etc/wpa_supplicant.conf"' "$SRC" && { echo "still using the OEM wpa conf" >&2; exit 1; }
grep -q "g_cfg.mode = MODE_CLIENT;" "$SRC" && grep -n "scan_networks" "$SRC" >/dev/null || true
awk '/static void scan_networks/,/^}/' "$SRC" | grep -q "MODE_CLIENT" && { echo "scan still changes mode" >&2; exit 1; }
awk '/static bool bt_set_pairing/,/^}/' "$SRC" | grep -q 'Powered", true);' && { echo "pairing-off still powers BT on" >&2; exit 1; }

for needle in "Start CarPlay pairing" "Stop pairing" "btDiscoverable" "pairMessage" "pair_message"; do
    grep -q "$needle" "$PAGE" || { echo "missing in WifiPage: $needle" >&2; exit 1; }
done

for needle in "wpa_cli -i wlan0" "Bluetooth / vbtd" "getAddressAttribute" "wpa_supplicant.log"; do
    grep -q "$needle" "$DIAG" || { echo "missing in diagnostics: $needle" >&2; exit 1; }
done

# The start command must only use options the car's wpa_supplicant supports.
# Cross-check against the usage text compiled into the firmware binary.
ROOTFS="${SCOURGE_ROOTFS:-$(dirname "$0")/../../../work/rootfs}"
WPA_BIN="$ROOTFS/usr/sbin/wpa_supplicant"
CMD=$(grep -A2 'nohup /usr/sbin/wpa_supplicant' "$SRC" | grep -o 'wpa_supplicant -u[^"]*' | head -1)
[ -n "$CMD" ] || { echo "cannot extract wpa_supplicant invocation" >&2; exit 1; }
case "$CMD" in
  *" -f"*) echo "wpa_supplicant -f is not supported by the car build" >&2; exit 1;;
  *" -B"*) echo "wpa_supplicant -B silences the log (daemon(0,0))" >&2; exit 1;;
  *" -e "*) echo "wpa_supplicant -e /rw_data/wifi/entropy.bin would be killed by stop_oem_wifi" >&2; exit 1;;
esac
if [ -x "$WPA_BIN" ]; then
    for opt in $(echo "$CMD" | grep -oE ' -[A-Za-z]' | tr -d ' -' | tr '\n' ' '); do
        strings "$WPA_BIN" | grep -q "  -$opt = " ||
            { echo "car wpa_supplicant does not support -$opt" >&2; exit 1; }
    done
fi
grep -q 'start_wpa_supplicant_args("nl80211,wext")' "$SRC" ||
    { echo "driver list nl80211,wext missing from the start call" >&2; exit 1; }
grep -q "log_tail(WPA_LOG" "$SRC" || { echo "start failure does not surface the log tail" >&2; exit 1; }
grep -q "g_apply_failed ? 60000" "$SRC" ||
    { echo "watchdog failure backoff missing" >&2; exit 1; }
grep -q '"/logs/dump"' "$SRC" ||
    { echo "log dump endpoint missing" >&2; exit 1; }
grep -q "dump_logs_to_usb(false)" "$SRC" ||
    { echo "automatic log dumps missing" >&2; exit 1; }
grep -q "proc/mounts" "$SRC" ||
    { echo "USB detection does not use /proc/mounts" >&2; exit 1; }
# USB detection must NOT use a fstype whitelist: the car's exFAT is Tuxera
# texfat.ko (/proc/mounts reports "texfat", not "exfat") and FUSE mounts
# report fuseblk/fuse.exfat — a whitelist silently rejects real sticks.
if grep -Eq 'strncmp\(fs, |strcasecmp\(fs, ' "$SRC"; then
    { echo "USB detection uses a fstype whitelist (texfat/fuseblk sticks would be missed)" >&2; exit 1; }
fi
grep -q "texfat" "$SRC" ||
    { echo "USB detection comment does not document the texfat gotcha" >&2; exit 1; }
grep -q "psk=<redacted>" "$SRC" ||
    { echo "config dump does not redact secrets" >&2; exit 1; }
grep -q "Save logs" "$PAGE" ||
    { echo "Save logs button missing from the UI" >&2; exit 1; }

# Scan parser: 5 fields leave nf == 4; `nf < 5` discarded every result.
grep -q "if (nf < 4)" "$SRC" || { echo "scan field guard not fixed" >&2; exit 1; }
grep -q "if (nf < 5)" "$SRC" && { echo "scan off-by-one is back" >&2; exit 1; }
grep -q "ssid_md5" "$SRC" || { echo "md5 service matching missing" >&2; exit 1; }
grep -q "connmanctl connect rc=" "$SRC" ||
    { echo "connmanctl connect output is not captured" >&2; exit 1; }

# Async apply: fork + result hand-off + cancel/reap.
for needle in APPLY_RESULT "fork()" apply_cancel apply_reap 'g_apply_pid > 0 ?'; do
    grep -q "$needle" "$SRC" || { echo "async apply missing: $needle" >&2; exit 1; }
done
grep -q 'apply_mode(true)' "$SRC" || { echo "pairing apply is not routed" >&2; exit 1; }

# 2.4 GHz tether fallback replaces the invalid simple retry.
grep -q "tether 2.4GHz" "$SRC" || { echo "2.4 GHz tether fallback missing" >&2; exit 1; }
grep -q "tether simple" "$SRC" && { echo "invalid simple tether retry is back" >&2; exit 1; }

# Child -> parent state hand-off.
grep -q "bt_state_save" "$SRC" || { echo "BT state hand-off missing" >&2; exit 1; }
grep -q "SSH_BOUND" "$SRC" || { echo "SSH bound-IP hand-off missing" >&2; exit 1; }

# Console: tail endpoint + UI panel.
grep -q 'read_tail(log_path' "$SRC" || { echo "/logs tail read missing" >&2; exit 1; }
grep -q "consolePanel" "$PAGE" || { echo "console panel missing" >&2; exit 1; }
grep -q "showConsole" "$PAGE" || { echo "console layout switch missing" >&2; exit 1; }
grep -q "REBOOT_REQUIRED" scripts/run_stuff.sh 2>/dev/null ||
    { echo "installer does not verify the AppHomeScreen restart" >&2; exit 1; }
grep -q ".cache/qmlcache" scripts/run_stuff.sh 2>/dev/null ||
    { echo "installer does not clear the QML disk cache" >&2; exit 1; }

echo "PASS: supplicant, AP and Bluetooth pairing checks"
