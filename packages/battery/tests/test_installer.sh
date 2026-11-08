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
cp "$HERE/backup/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml" \
   "$QML/HomeScreen/"

for t in systemctl dbus-send pidof chsmack; do
    printf '#!/bin/sh\nexit 0\n' > "$STUB/$t"
    chmod +x "$STUB/$t"
done

run() {
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
WIFI_QML="$HERE/../wifi_app/artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.hello_wifi.qml"
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

echo "ALL INSTALLER TESTS PASSED"
