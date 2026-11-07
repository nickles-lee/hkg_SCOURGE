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
    "wpa_cli -p \" WPA_CTRL_DIR \" -i wlan0 add_network" \
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

# Log-bundle pruning: shell-free, and only names this daemon itself writes
# are candidates (rule 9 — stick content is data, never instructions).
grep -q 'sh("rm -rf %s/%s' "$SRC" &&
    { echo "shell prune path is back; injection risk" >&2; exit 1; }
grep -q "is_pruneable_name" "$SRC" ||
    { echo "prune name allowlist missing" >&2; exit 1; }
grep -q "safe_rmtree" "$SRC" ||
    { echo "shell-free prune helper missing" >&2; exit 1; }
awk '/static bool safe_rmtree/,/^}/' "$SRC" | grep -q "lstat(" ||
    { echo "prune delete does not lstat before following" >&2; exit 1; }

for needle in "Start CarPlay AP" "Stop pairing" "btDiscoverable" "pairMessage" "pair_message"; do
    grep -q "$needle" "$PAGE" || { echo "missing in WifiPage: $needle" >&2; exit 1; }
done

for needle in "wpa_cli -p /var/run/wpa_supplicant -i wlan0" "Bluetooth / vbtd" "getAddressAttribute" "wpa_supplicant.log"; do
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
grep -q "psk=<redacted>" "$SRC" ||
    { echo "config dump does not redact secrets" >&2; exit 1; }
grep -q "Save logs" "$PAGE" ||
    { echo "Save logs button missing from the UI" >&2; exit 1; }

# Scan parser: 5 fields leave nf == 4; `nf < 5` discarded every result.
grep -q "if (nf < 4)" "$SRC" || { echo "scan field guard not fixed" >&2; exit 1; }
grep -q "if (nf < 5)" "$SRC" && { echo "scan off-by-one is back" >&2; exit 1; }
grep -q "ssid_md5" "$SRC" || { echo "md5 service matching missing" >&2; exit 1; }
grep -q "connmanctl agent rc=" "$SRC" ||
    { echo "connmanctl agent output is not captured" >&2; exit 1; }

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


# ---- firmware-verified command syntax --------------------------------
# connmanctl tether takes options, not positionals (the car binary prints
# "unknown option [?][no]" for the old form) and --appinfo 1 marks CarPlay.
grep -q -- '--appinfo 1' "$SRC" ||
    { echo "tether is missing --appinfo 1" >&2; exit 1; }
grep -q -- '--frequency %d --maxsta %d' "$SRC" ||
    { echo "tether does not use the option form" >&2; exit 1; }
grep -q 'tether wifi on %s %s %s %d %d' "$SRC" &&
    { echo "positional tether args are back" >&2; exit 1; }

# This connmanctl has no `config --wifi`; the passphrase goes through the
# interactive agent.  The script is built from stdin and redacted in logs.
grep -q 'client_connman_agent' "$SRC" ||
    { echo "connmanctl agent flow missing" >&2; exit 1; }
grep -q 'agent on' "$SRC" ||
    { echo "agent registration missing from the connect flow" >&2; exit 1; }
grep -q 'sh %s | connmanctl' "$SRC" ||
    { echo "agent script is not fed to connmanctl" >&2; exit 1; }
grep -q 'config %s --wifi' "$SRC" &&
    { echo "invalid connmanctl config --wifi is back" >&2; exit 1; }
grep -q 'redact_secret' "$SRC" ||
    { echo "agent output is not redacted" >&2; exit 1; }

# Every wpa_cli call names the control socket explicitly.
grep -q 'wpa_cli -p " WPA_CTRL_DIR " -i wlan0' "$SRC" ||
    { echo "wpa_cli ctrl dir is not explicit" >&2; exit 1; }
grep -q 'wpa_cli -i wlan0' "$SRC" &&
    { echo "a wpa_cli call still relies on the default ctrl dir" >&2; exit 1; }

# Home/Star is a session-bus event; the daemon must expose home_seq.
for needle in 'NotifyRequestHomeScreen' 'custom_key_value' 'home_monitor_tick' \
              'home_monitor_cleanup' 'home_seq'; do
    grep -q -- "$needle" "$SRC" ||
        { echo "home/star watcher missing: $needle" >&2; exit 1; }
done

grep -q 'HOME_STATE' "$SRC" && grep -q 'home_state_write' "$SRC" ||
    { echo "home.json fast-path state is missing" >&2; exit 1; }
grep -q 'refreshHome' "$PAGE" && grep -q 'interval: 500' "$PAGE" ||
    { echo "QML does not fast-poll home.json" >&2; exit 1; }

grep -q 'if (!ok) str_copy(why, sizeof(why), g_last_msg)' "$SRC" ||
    { echo "client error is overwritten by ssh_start before it is saved" >&2; exit 1; }

grep -q 'sanitize_bus_address' "$SRC" ||
    { echo "session bus address quoting is not normalised" >&2; exit 1; }

grep -q 'fcntl(srv, F_SETFD, FD_CLOEXEC)' "$SRC" ||
    { echo "HTTP listen socket leaks into popen children" >&2; exit 1; }
grep -q 'fcntl(c, F_SETFD, FD_CLOEXEC)' "$SRC" ||
    { echo "HTTP client socket leaks into popen children" >&2; exit 1; }

grep -q 'CarPlay IE skipped: Bluetooth address unknown' "$SRC" ||
    { echo "empty-BT CarPlay IE guard missing" >&2; exit 1; }

# Phase-0 fixes: md5sum applet fallback and tolerant dbus-monitor parsing.
grep -q 'md5sum.coreutils' "$SRC" ||
    { echo "ssid_md5 has no md5sum.coreutils fallback" >&2; exit 1; }
grep -q 'MD5SUM_BIN' scripts/run_stuff.sh ||
    { echo "installer has no md5sum fallback" >&2; exit 1; }
grep -q 'MD5SUM_BIN' scripts/run_stuff_restore.sh ||
    { echo "restore has no md5sum fallback" >&2; exit 1; }
grep -q 'sscanf(p, "int32 %d"' "$SRC" ||
    { echo "home watcher parsing is not trim-tolerant" >&2; exit 1; }
grep -q 'already paired via Bluetooth' "$PAGE" ||
    { echo "CarPlay tab missing the already-paired helper text" >&2; exit 1; }

# The daemon must survive loopback not being up yet (VM-observed crash).
grep -q 'ip link set lo up' "$SRC" ||
    { echo "daemon does not bring up lo before binding" >&2; exit 1; }
grep -q 'for (int i = 0; i < 60; i++)' "$SRC" ||
    { echo "bind retry loop missing" >&2; exit 1; }

# SMACK floor label and OEM unit masking.
grep -q 'chsmack -e _' scripts/run_stuff.sh ||
    { echo "daemon/dropbear keep the Privileged label" >&2; exit 1; }
grep -q 'chsmack -e Privileged' scripts/run_stuff.sh &&
    { echo "Privileged SMACK label is back" >&2; exit 1; }
grep -q 'for unit in supplicantmanager.path supplicantmanager.service' scripts/run_stuff.sh ||
    { echo "OEM supplicant units are not masked" >&2; exit 1; }
grep -q 'systemctl mask "$unit"' scripts/run_stuff.sh ||
    { echo "supplicantmanager mask command missing" >&2; exit 1; }
grep -q 'systemctl unmask supplicantmanager.path supplicantmanager.service' scripts/run_stuff_restore.sh ||
    { echo "restore does not unmask the OEM supplicant units" >&2; exit 1; }

# Overlay checklist: requested/open/close, foreground gating, input shield,
# and the Home/Star counter.
for needle in 'property bool requested' 'visible: requested && !atOtherApp' \
              'function open()' 'function close()' 'preventStealing: true' \
              'acceptedButtons: Qt.AllButtons' 'onHomeSeqChanged' \
              'onCurrentScreenChanged' 'onCurrentHomeAdvancedPageIdxChanged'; do
    grep -q -- "$needle" "$PAGE" ||
        { echo "overlay pattern missing in WifiPage: $needle" >&2; exit 1; }
done

echo "PASS: supplicant, AP and Bluetooth pairing checks"
