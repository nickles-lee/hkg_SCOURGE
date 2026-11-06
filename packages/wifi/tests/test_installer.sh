#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Offline installer/restore test using a fake root and fake /etc.
# Usage: sh tests/test_installer.sh [stock|hello|battery|hello_battery|battery_tile|hello_battery_tile]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
KIND=${1:-stock}
TMP=$(mktemp -d /tmp/wifi-installer-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

case "$KIND" in
  stock)
    A="$ROOT/base/stock/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$ROOT/base/stock/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$ROOT/base/stock/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$ROOT/base/stock/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$ROOT/base/stock/component/QCustomizedMenuListAllowEmpty.qml"
    T="${SCOURGE_ROOTFS:-$(dirname "$0")/../../work/rootfs}/app/share/AppHomeScreen/qml_eco3.0/component/common/Q_ThemeImage.qml"
    EXPECT="wifi/wifi/wifi/wifi/wifi"
    ;;
  hello)
    A="$ROOT/base/hello/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$ROOT/base/hello/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$ROOT/base/hello/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$ROOT/base/hello/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$ROOT/base/hello/component/QCustomizedMenuListAllowEmpty.qml"
    T="${SCOURGE_FIXTURES:-.}/hello_qml_eco3.0/component/common/Q_ThemeImage.qml"
    EXPECT="hello_wifi/hello_wifi/hello_wifi/hello_wifi/hello_wifi"
    ;;
  battery)
    A="$ROOT/base/battery/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$ROOT/base/stock/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$ROOT/base/stock/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$ROOT/base/stock/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$ROOT/base/stock/component/QCustomizedMenuListAllowEmpty.qml"
    T="${SCOURGE_ROOTFS:-$(dirname "$0")/../../work/rootfs}/app/share/AppHomeScreen/qml_eco3.0/component/common/Q_ThemeImage.qml"
    EXPECT="battery_wifi/wifi/wifi/wifi/wifi"
    ;;
  hello_battery)
    A="$ROOT/base/battery_hello/HomeScreen/QHMI_HomeScreenMain_Advanced.hello.qml"
    W="$ROOT/base/hello/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$ROOT/base/hello/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$ROOT/base/hello/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$ROOT/base/hello/component/QCustomizedMenuListAllowEmpty.qml"
    T="${SCOURGE_FIXTURES:-.}/hello_qml_eco3.0/component/common/Q_ThemeImage.qml"
    EXPECT="hello_battery_wifi/hello_wifi/hello_wifi/hello_wifi/hello_wifi"
    ;;
  battery_tile)
    A="$ROOT/base/battery_tile/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$ROOT/base/stock/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$ROOT/base/stock/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$ROOT/base/stock/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$ROOT/base/stock/component/QCustomizedMenuListAllowEmpty.qml"
    T="${SCOURGE_ROOTFS:-$(dirname "$0")/../../work/rootfs}/app/share/AppHomeScreen/qml_eco3.0/component/common/Q_ThemeImage.qml"
    EXPECT="battery_tile_wifi/wifi/wifi/wifi/wifi"
    ;;
  hello_battery_tile)
    A="$ROOT/base/battery_tile_hello/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$ROOT/base/hello/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$ROOT/base/hello/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$ROOT/base/hello/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$ROOT/base/hello/component/QCustomizedMenuListAllowEmpty.qml"
    T="${SCOURGE_FIXTURES:-.}/hello_qml_eco3.0/component/common/Q_ThemeImage.qml"
    EXPECT="hello_battery_tile_wifi/hello_wifi/hello_wifi/hello_wifi/hello_wifi"
    ;;
  *) echo "unknown kind: $KIND" >&2; exit 2 ;;
esac

mkdir -p "$TMP/etc/systemd/system" "$TMP/home" "$TMP/cache" "$TMP/tmp" \
         "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen" \
         "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/component/common" \
         "$TMP/usb"
cp "$A" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
cp "$W" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
cp "$M" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/Screen_HomeScreen_AllMenus.qml"
cp "$S" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
cp "$C" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/component/QCustomizedMenuListAllowEmpty.qml"
cp "$T" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/component/common/Q_ThemeImage.qml"

printf 'root:x:0:0:root:/root:/bin/sh\n' > "$TMP/etc/passwd"
printf 'root:*:19000:0:99999:7:::\n' > "$TMP/etc/shadow"
: > "$TMP/tmp/shells"

cp -a "$ROOT/out/usb/payload" "$TMP/usb/"
cp -a "$ROOT/out/usb/backup" "$TMP/usb/"
cp "$ROOT/out/usb/manifest.md5" "$TMP/usb/"

sed -e "s#/etc/passwd#$TMP/etc/passwd#g" \
    -e "s#/etc/shadow#$TMP/etc/shadow#g" \
    -e "s#/home/car#$TMP/home/car#g" \
    -e "s#/tmp/shells#$TMP/tmp/shells#g" \
    -e "s#/root/.cache#$TMP/cache#g" \
    -e 's#mount -o remount,rw "$USB_DIR"#true#g' \
    -e 's#mount -o remount,rw /#true#g' \
    -e 's#mount -o remount,ro /#true#g' \
    -e 's#systemctl#true#g' \
    -e 's#dbus-send#false#g' \
    -e 's#sleep 2#sleep 0#g' \
    -e 's#sleep 1#sleep 0#g' \
    -e 's#^sync$#true#g' \
    "$ROOT/scripts/run_stuff.sh" > "$TMP/install.sh"
sed -e "s#/etc/passwd#$TMP/etc/passwd#g" \
    -e "s#/etc/shadow#$TMP/etc/shadow#g" \
    -e "s#/home/car#$TMP/home/car#g" \
    -e "s#/tmp/shells#$TMP/tmp/shells#g" \
    -e "s#/root/.cache#$TMP/cache#g" \
    -e 's#mount -o remount,rw "$USB_DIR"#true#g' \
    -e 's#mount -o remount,rw /#true#g' \
    -e 's#mount -o remount,ro /#true#g' \
    -e 's#systemctl#true#g' \
    -e 's#dbus-send#false#g' \
    -e 's#sleep 2#sleep 0#g' \
    -e 's#sleep 1#sleep 0#g' \
    -e 's#^sync$#true#g' \
    "$ROOT/scripts/run_stuff_restore.sh" > "$TMP/restore.sh"

WIFI_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
WIFI_RW_DIR="$TMP/rw" \
WIFI_SERVICE="$TMP/etc/systemd/system/wifi-daemon.service" \
USB_DIR="$TMP/usb" \
sh "$TMP/install.sh"

grep -q "variant=$EXPECT" "$TMP/usb/run_stuff_result.txt" || {
    echo "unexpected variant:" >&2
    cat "$TMP/usb/run_stuff_result.txt" >&2
    exit 1
}
grep -q 'oem_mask=ok' "$TMP/usb/run_stuff_result.txt" || {
    echo "OEM supplicant units were not masked" >&2
    cat "$TMP/usb/run_stuff_result.txt" >&2
    exit 1
}
test -f "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/WifiPage.qml"
test -f "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/component/WifiTile.qml"
test -f "$TMP/root/app/share/AppHomeScreen/wifi/wifi_icon_n.png"
test -f "$TMP/rw/wifi_daemon"
test -f "$TMP/rw/bin/dropbearmulti"
test -f "$TMP/etc/systemd/system/wifi-daemon.service"
grep -q '^car:' "$TMP/etc/passwd"
grep -q '^car:' "$TMP/etc/shadow"

WIFI_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
WIFI_RW_DIR="$TMP/rw" \
WIFI_SERVICE="$TMP/etc/systemd/system/wifi-daemon.service" \
USB_DIR="$TMP/usb" \
sh "$TMP/restore.sh"

grep -q 'status=OK' "$TMP/usb/run_stuff_result.txt"
test ! -f "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/WifiPage.qml"
test ! -f "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/component/WifiTile.qml"
test ! -f "$TMP/rw/wifi_daemon"
! grep -q '^car:' "$TMP/etc/passwd"
! grep -q '^car:' "$TMP/etc/shadow"

echo "PASS: $KIND install/restore"
