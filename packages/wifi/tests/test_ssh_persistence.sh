#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Simulate the real 260128 passwd/shadow symlink layout and verify that the
# SSH user survives an esinit-style passwd rewrite, then restore cleans up.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
TMP=$(mktemp -d /tmp/wifi-ssh-persist.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/etc" "$TMP/tmp" "$TMP/home" "$TMP/cache" \
         "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen" \
         "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/component/common" \
         "$TMP/root/etc/systemd/system" "$TMP/usb"

printf 'root:x:0:0:root:/home/root:/bin/sh\n' > "$TMP/etc/passwd.legacy"
printf 'root:x:0:0:root:/home/root:/usr/sbin/nologin\n' > "$TMP/etc/passwd.secure"
printf 'root:$6$I6C6apC0ue$J1MSIq2VqvBFdJhPwZZBfSD5qkZxUBhkemElmQd6CYDQ0bRo5PDcKBKJF7EqL6zuFlYCCY3X1kGIcUe3W0Rm60:20481:0:99999:7:::\n' > "$TMP/etc/shadow.Global"
ln -s "$TMP/tmp/passwd" "$TMP/etc/passwd"
ln -s "$TMP/tmp/shadow" "$TMP/etc/shadow"
ln -s "$TMP/etc/shadow.Global" "$TMP/tmp/shadow"
cp "$TMP/etc/passwd.legacy" "$TMP/tmp/passwd"
: > "$TMP/tmp/shells"

cp "$ROOT/base/stock/HomeScreen/QHMI_HomeScreenMain_Advanced.qml" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp "$ROOT/base/stock/HomeScreen/QHMI_HomeScreenMain_Wide.qml" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp "$ROOT/base/stock/HomeScreen/Screen_HomeScreen_AllMenus.qml" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp "$ROOT/base/stock/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp "$ROOT/base/stock/component/QCustomizedMenuListAllowEmpty.qml" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/component/"
cp "${SCOURGE_ROOTFS:-$(dirname "$0")/../../work/rootfs}/app/share/AppHomeScreen/qml_eco3.0/component/common/Q_ThemeImage.qml" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/component/common/"

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
# Do not let the restore test run the host daemon.
sed -i 's#if \[ -x "\$RW_DIR/wifi_daemon" \]; then#if false; then#' "$TMP/restore.sh"

WIFI_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
WIFI_RW_DIR="$TMP/rw" \
WIFI_SERVICE="$TMP/root/etc/systemd/system/wifi-daemon.service" \
USB_DIR="$TMP/usb" \
sh "$TMP/install.sh"

# Installer must have patched both persistent templates and the live file.
grep -q '^car:' "$TMP/etc/passwd.legacy"
grep -q '^car:' "$TMP/etc/passwd.secure"
grep -q '^car:' "$TMP/tmp/passwd"
grep -q '^car:' "$TMP/etc/shadow.Global"
grep -qx '/bin/sh' "$TMP/tmp/shells"

# Simulate esinit writing passwd.legacy over /tmp/passwd on the next boot.
cp "$TMP/etc/passwd.legacy" "$TMP/tmp/passwd"
grep -q '^car:' "$TMP/tmp/passwd"

# And the secure mode path.
cp "$TMP/etc/passwd.secure" "$TMP/tmp/passwd"
grep -q '^car:' "$TMP/tmp/passwd"

WIFI_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
WIFI_RW_DIR="$TMP/rw" \
WIFI_SERVICE="$TMP/root/etc/systemd/system/wifi-daemon.service" \
USB_DIR="$TMP/usb" \
sh "$TMP/restore.sh"

! grep -q '^car:' "$TMP/etc/passwd.legacy"
! grep -q '^car:' "$TMP/etc/passwd.secure"
! grep -q '^car:' "$TMP/tmp/passwd"
! grep -q '^car:' "$TMP/etc/shadow.Global"
test -L "$TMP/etc/passwd"
test -L "$TMP/etc/shadow"

echo "PASS: ssh user survives esinit passwd rewrite and restore removes it"
