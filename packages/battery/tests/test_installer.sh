#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Offline installer/restore test. Uses a fake root tree and stub system tools.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
USB=${1:-/tmp/battery_usb}
FAKE=$(mktemp -d /tmp/battery_fake.XXXXXX)
STUB=$(mktemp -d /tmp/battery_stub.XXXXXX)
trap 'rm -rf "$FAKE" "$STUB"' EXIT

QML="$FAKE/app/share/AppHomeScreen/qml_eco3.0"
RW="$FAKE/rw_data/battery"
SVC="$FAKE/etc/systemd/system/battery-daemon.service"
mkdir -p "$QML/HomeScreen" "$QML/component" "$FAKE/etc/systemd/system"
printf 'device %s ext4 rw 0 0\n' "$FAKE" > "$FAKE/mounts"
export INSTALL_IO_ROOT="$FAKE" INSTALL_IO_MOUNTS="$FAKE/mounts"
cp "$HERE/backup/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml" \
   "$QML/HomeScreen/"

for t in systemctl dbus-send dbus-monitor pidof; do
    printf '#!/bin/sh\nexit 0\n' > "$STUB/$t"
    chmod +x "$STUB/$t"
done
printf '#!/bin/sh\nprintf "%%s\\n" "$*" >> "$CHSMACK_LOG"\nexit 0\n' > "$STUB/chsmack"
printf '#!/bin/sh\nif [ "${BAD_LABEL:-0}" = 1 ]; then echo Privileged; else echo _; fi\n' > "$STUB/getfattr"
chmod +x "$STUB/chsmack" "$STUB/getfattr"

run() {
    CHSMACK_LOG="$STUB/chsmack.log" \
    BATTERY_SMACK_SELF="${TEST_SMACK_SELF:-_}" \
    BATTERY_DAEMON_SMACK="${TEST_DAEMON_SMACK:-_}" \
    BATTERY_PATH="$STUB:/usr/bin:/bin" \
    BATTERY_QML_ROOT="$FAKE/app/share/AppHomeScreen" \
    BATTERY_RW_DIR="$RW" \
    BATTERY_SERVICE="$SVC" \
    USB_DIR="$USB" \
    sh "$1"
}

echo "== first install =="
run "$USB/run_stuff.sh"
cat "$USB/run_stuff_result.txt"
grep -q 'status=OK' "$USB/run_stuff_result.txt"
grep -q 'variant=plain' "$USB/run_stuff_result.txt"
grep -q 'BATTERY_PAYLOAD_260128' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
test -f "$QML/HomeScreen/BatteryPage.qml"
test -f "$QML/component/BatterySwitch.qml"
test -f "$QML/component/BatteryTile.qml"
test -f "$FAKE/app/share/AppHomeScreen/battery/battery_icon_n.png"
test -f "$RW/battery_daemon"
test -f "$RW/bin/CANService"
test -f "$RW/bin/micom-send"
test -f "$RW/bin/dbus-monitor"
grep -q 'SetCANFixedMicomRecv' "$RW/bin/micom-send"
grep -q '^fixed)' "$RW/bin/micom-send"
# The fixed-mode interface other first-party apps use (Tweaks blue light).
cat > "$RW/bin/CANService" <<'CANEOF'
#!/bin/sh
printf '%s\n' "$*" >> "$CAN_LOG"
exit 0
CANEOF
chmod +x "$RW/bin/CANService"
CAN_LOG="$STUB/can.log" "$RW/bin/micom-send" fixed 0x06 0x3f 26 2 2
grep -q 'SetCANFixedMicomRecv array:byte:0x06,0x3f int32:26 int32:2 int32:2' "$STUB/can.log"
CAN_LOG="$STUB/can.log" "$RW/bin/micom-send" raw array:byte:0x04,0xa2,0x08,0xff
grep -q 'SetCANMicomRecv array:byte:0x04,0xa2,0x08,0xff' "$STUB/can.log"
test -f "$RW/smack_expected"
[ "$(cat "$RW/smack_expected")" = _ ]
grep -q "smack_self=_" "$USB/run_stuff_result.txt"
grep -q "daemon_smack=_" "$USB/run_stuff_result.txt"
grep -q "smack_check=ok" "$USB/run_stuff_result.txt"
grep -q -- "-e _ $RW/battery_daemon" "$STUB/chsmack.log"
grep -q -- "-e _ $RW/bin/CANService" "$STUB/chsmack.log"
grep -q -- "-e _ $RW/bin/micom-send" "$STUB/chsmack.log"
grep -q -- "-e _ $RW/bin/dbus-monitor" "$STUB/chsmack.log"
test -f "$SVC"
echo "first install OK"

echo "== reinstall (already patched) =="
run "$USB/run_stuff.sh"
grep -q 'already_patched=1' "$USB/run_stuff_result.txt"
echo "reinstall OK"

echo "== restore =="
run "$USB/run_stuff_restore.sh"
cat "$USB/run_stuff_result.txt"
grep -q 'status=OK' "$USB/run_stuff_result.txt"
if grep -q 'BATTERY_PAYLOAD_260128' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"; then
    echo "restore left the marker behind" >&2
    exit 1
fi
test ! -f "$QML/HomeScreen/BatteryPage.qml"
test ! -f "$QML/component/BatterySwitch.qml"
test ! -f "$QML/component/BatteryTile.qml"
test ! -f "$SVC"
echo "restore OK"

echo "== hello coexistence =="
cp "$HERE/backup/hello/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml" \
   "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
run "$USB/run_stuff.sh"
cat "$USB/run_stuff_result.txt"
grep -q 'variant=hello' "$USB/run_stuff_result.txt"
grep -q 'hello_mismatch=0' "$USB/run_stuff_result.txt"
grep -q 'HELLO_PAYLOAD_260128' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'BATTERY_PAYLOAD_260128' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'HelloTile' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
echo "hello coexistence OK"

echo "== restore after hello =="
run "$USB/run_stuff_restore.sh"
grep -q 'HELLO_PAYLOAD_260128' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
if grep -q 'BATTERY_PAYLOAD_260128' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"; then
    echo "restore removed battery but damaged hello" >&2
    exit 1
fi
echo "hello restore OK"

echo "== wifi coexistence =="
WIFI_QML="$HERE/../wifi/artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.hello_wifi.qml"
if [ ! -f "$WIFI_QML" ]; then
    echo "wifi fixture not present (wifi package QML artifacts not shipped); skipping"
else
cp "$WIFI_QML" \
   "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
run "$USB/run_stuff.sh"
cat "$USB/run_stuff_result.txt"
grep -q 'status=OK' "$USB/run_stuff_result.txt"
grep -q 'variant=hello_wifi' "$USB/run_stuff_result.txt"
grep -q 'WIFI_PAYLOAD_260128' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'HELLO_PAYLOAD_260128' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'BATTERY_PAYLOAD_260128' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'firstEmptySlot' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'id: wifiTile1' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'id: batteryTile1' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
echo "wifi coexistence OK"

echo "== restore after wifi =="
run "$USB/run_stuff_restore.sh"
grep -q 'WIFI_PAYLOAD_260128' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'HELLO_PAYLOAD_260128' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
if grep -q 'BATTERY_PAYLOAD_260128' "$QML/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"; then
    echo "restore removed battery but damaged wifi/hello" >&2
    exit 1
fi
echo "wifi restore OK"
fi

echo "== smack label gate (negative) =="
if BAD_LABEL=1 run "$USB/run_stuff.sh"; then
    echo "installer accepted a wrong SMACK exec label" >&2
    exit 1
fi
grep -q 'status=FAIL' "$USB/run_stuff_result.txt"
grep -q 'exec label readback failed' "$USB/run_stuff_result.txt"
echo "smack gate OK"

echo "== smack runtime gate (negative) =="
if TEST_DAEMON_SMACK=Privileged run "$USB/run_stuff.sh"; then
    echo "installer accepted a daemon running outside the installer label" >&2
    exit 1
fi
grep -q 'status=FAIL' "$USB/run_stuff_result.txt"
grep -q 'smack_check=mismatch' "$USB/run_stuff_result.txt"
echo "smack runtime gate OK"

echo "ALL INSTALLER TESTS PASSED"
