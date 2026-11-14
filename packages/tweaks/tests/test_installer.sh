#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Offline installer/restore test using a fake root.
# Usage: sh tests/test_installer.sh [stock|hello|battery|hello_battery|wifi|hello_wifi|battery_wifi|hello_battery_wifi]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
KIND=${1:-stock}
TMP=$(mktemp -d /tmp/tweaks-installer-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

B="$ROOT/base"
EXPECT_GLOBAL=0
case "$KIND" in
  stock)
    A="$B/stock/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$B/stock/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$B/stock/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$B/stock/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$B/stock/component/QCustomizedMenuListAllowEmpty.qml"
    EXPECT="stock/stock/stock/stock/stock"
    ;;
  hello)
    A="$B/hello/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$B/hello/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$B/hello/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$B/hello/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$B/hello/component/QCustomizedMenuListAllowEmpty.qml"
    EXPECT="hello/hello/hello/hello/hello"
    ;;
  battery)
    A="$B/battery/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$B/stock/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$B/stock/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$B/stock/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$B/stock/component/QCustomizedMenuListAllowEmpty.qml"
    EXPECT="battery/stock/stock/stock/stock"
    ;;
  hello_battery)
    A="$B/battery_hello/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$B/hello/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$B/hello/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$B/hello/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$B/hello/component/QCustomizedMenuListAllowEmpty.qml"
    EXPECT="battery_hello/hello/hello/hello/hello"
    ;;
  wifi)
    A="$B/wifi/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$B/wifi/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$B/wifi/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$B/wifi/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$B/wifi/component/QCustomizedMenuListAllowEmpty.qml"
    EXPECT="wifi/wifi/wifi/wifi/wifi"
    ;;
  hello_wifi)
    A="$B/hello_wifi/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$B/hello_wifi/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$B/hello_wifi/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$B/hello_wifi/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$B/hello_wifi/component/QCustomizedMenuListAllowEmpty.qml"
    EXPECT="hello_wifi/hello_wifi/hello_wifi/hello_wifi/hello_wifi"
    ;;
  battery_wifi)
    A="$B/battery_wifi/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$B/wifi/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$B/wifi/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$B/wifi/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$B/wifi/component/QCustomizedMenuListAllowEmpty.qml"
    EXPECT="battery_wifi/wifi/wifi/wifi/wifi"
    ;;
  hello_battery_wifi)
    A="$B/hello_battery_wifi/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$B/hello_wifi/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$B/hello_wifi/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$B/hello_wifi/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$B/hello_wifi/component/QCustomizedMenuListAllowEmpty.qml"
    EXPECT="hello_battery_wifi/hello_wifi/hello_wifi/hello_wifi/hello_wifi"
    ;;
  battery_tile)
    A="$B/battery_tile/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$B/stock/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$B/stock/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$B/stock/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$B/stock/component/QCustomizedMenuListAllowEmpty.qml"
    EXPECT="battery_tile/stock/stock/stock/stock"
    EXPECT_GLOBAL=1
    ;;
  hello_battery_tile)
    A="$B/battery_tile_hello/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$B/hello/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$B/hello/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$B/hello/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$B/hello/component/QCustomizedMenuListAllowEmpty.qml"
    EXPECT="battery_tile_hello/hello/hello/hello/hello"
    EXPECT_GLOBAL=1
    ;;
  battery_tile_wifi)
    A="$B/battery_tile_wifi/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$B/wifi/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$B/wifi/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$B/wifi/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$B/wifi/component/QCustomizedMenuListAllowEmpty.qml"
    EXPECT="battery_tile_wifi/wifi/wifi/wifi/wifi"
    EXPECT_GLOBAL=1
    ;;
  hello_battery_tile_wifi)
    A="$B/hello_battery_tile_wifi/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    W="$B/hello_wifi/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
    M="$B/hello_wifi/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    S="$B/hello_wifi/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
    C="$B/hello_wifi/component/QCustomizedMenuListAllowEmpty.qml"
    EXPECT="hello_battery_tile_wifi/hello_wifi/hello_wifi/hello_wifi/hello_wifi"
    EXPECT_GLOBAL=1
    ;;
  *) echo "unknown kind: $KIND" >&2; exit 2 ;;
esac

mkdir -p "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen" \
         "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/component/common" \
         "$TMP/root/app/share/AppStandbyClock/qml_eco3.0" \
         "$TMP/root/usr/bin" "$TMP/etc/systemd/system" "$TMP/rw" "$TMP/usb" \
         "$TMP/cache" "$TMP/tmp" "$TMP/root/update"
cp "$A" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
cp "$W" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Wide.qml"
cp "$M" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/Screen_HomeScreen_AllMenus.qml"
cp "$S" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
cp "$C" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/component/QCustomizedMenuListAllowEmpty.qml"
cp "$B/stock/component/common/Q_ThemeImage.qml" "$TMP/root/app/share/AppHomeScreen/qml_eco3.0/component/common/Q_ThemeImage.qml"
cat > "$TMP/root/usr/bin/wrandr" <<EOF
#!/bin/sh
echo "\$@" >> "$TMP/wrandr.log"
exit 0
EOF
chmod 755 "$TMP/root/usr/bin/wrandr"
cat > "$TMP/root/usr/bin/chsmack" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "$TMP/chsmack.log"
exit 0
EOF
cat > "$TMP/root/usr/bin/getfattr" <<'EOF'
#!/bin/sh
case "$*" in
  *security.SMACK64EXEC*) if [ "${BAD_LABEL:-0}" = 1 ]; then printf 'Privileged';
                          elif [ "${UNREADABLE_LABEL:-0}" = 1 ]; then exit 1;
                          else printf '_'; fi ;;
  *security.SMACK64*)     [ "${BAD_QML_LABEL:-0}" = 1 ] && exit 1; printf '_' ;;
  *) exit 1 ;;
esac
EOF
cat > "$TMP/root/usr/bin/setfattr" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "$TMP/setfattr.log"
exit 0
EOF
chmod 755 "$TMP/root/usr/bin/chsmack" "$TMP/root/usr/bin/getfattr" \
          "$TMP/root/usr/bin/setfattr"

# Stand-in for the vendor dbus-send the installer copies to CANService. The
# generated scripts replace the literal dbus-send with false, so the helper
# source is injected with TWEAKS_DBUS_SEND.
cat > "$TMP/can_dbus" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "$TMP/can.log"
exit 0
EOF
chmod 755 "$TMP/can_dbus"

TWEAKS_PATH="$TMP/root/usr/bin:/usr/bin:/bin"
printf 'device %s ext4 rw 0 0\n' "$TMP/root" > "$TMP/mounts"
export INSTALL_IO_ROOT="$TMP/root" INSTALL_IO_MOUNTS="$TMP/mounts"
TWEAKS_SMACK_SELF=_
TWEAKS_DAEMON_SMACK=_
TWEAKS_DBUS_SEND="$TMP/can_dbus"
export TWEAKS_PATH TWEAKS_SMACK_SELF TWEAKS_DAEMON_SMACK TWEAKS_DBUS_SEND
for f in main.qml Screen_AppStandbyClock_Confirm.qml Screen_AppStandbyClock_Confirm_Profile.qml; do
    printf '// STOCK_STANDBY_260128\n' > "$TMP/root/app/share/AppStandbyClock/qml_eco3.0/$f"
done
printf '0' > "$TMP/root/update/offline_status"

cp -a "$ROOT/out/usb/payload" "$TMP/usb/"
cp -a "$ROOT/out/usb/backup" "$TMP/usb/"
cp "$ROOT/out/usb/manifest.md5" "$TMP/usb/"

sed -e "s#/usr/bin/wrandr#$TMP/root/usr/bin/wrandr#g" \
    -e "s#/usr/bin/wrandr.real#$TMP/root/usr/bin/wrandr.real#g" \
    -e "s#/root/.cache#$TMP/cache#g" \
    -e 's#mount -o remount,rw "$USB_DIR"#true#g' \
    -e 's#mount -o remount,rw /#true#g' \
    -e 's#mount -o remount,ro /#true#g' \
    -e 's#systemctl#true#g' \
    -e 's#dbus-send#false#g' \
    -e 's#sleep 2#sleep 0#g' \
    -e 's#sleep 1#sleep 0#g' \
    "$ROOT/scripts/run_stuff.sh" > "$TMP/install.sh"
sed -e "s#/usr/bin/wrandr#$TMP/root/usr/bin/wrandr#g" \
    -e "s#/usr/bin/wrandr.real#$TMP/root/usr/bin/wrandr.real#g" \
    -e "s#/root/.cache#$TMP/cache#g" \
    -e 's#mount -o remount,rw "$USB_DIR"#true#g' \
    -e 's#mount -o remount,rw /#true#g' \
    -e 's#mount -o remount,ro /#true#g' \
    -e 's#systemctl#true#g' \
    -e 's#dbus-send#false#g' \
    -e 's#sleep 2#sleep 0#g' \
    -e 's#sleep 1#sleep 0#g' \
    "$ROOT/scripts/run_stuff_restore.sh" > "$TMP/restore.sh"

TWEAKS_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
TWEAKS_RW_DIR="$TMP/rw" \
TWEAKS_SERVICE="$TMP/etc/systemd/system/tweaks-daemon.service" \
TWEAKS_STANDBY_ROOT="$TMP/root/app/share/AppStandbyClock" \
TWEAKS_OFFLINE_STATUS="$TMP/root/update/offline_status" \
TWEAKS_WRANDR="$TMP/root/usr/bin/wrandr" \
TWEAKS_WRANDR_REAL="$TMP/root/usr/bin/wrandr.real" \
USB_DIR="$TMP/usb" \
sh "$TMP/install.sh"

grep -q "variant=$EXPECT" "$TMP/usb/run_stuff_result.txt" || {
    echo "unexpected variant:" >&2
    cat "$TMP/usb/run_stuff_result.txt" >&2
    exit 1
}
Q="$TMP/root/app/share/AppHomeScreen/qml_eco3.0"
test -f "$Q/HomeScreen/TweaksPage.qml"
test -f "$Q/HomeScreen/RebootPage.qml"
test -f "$Q/component/TweaksTile.qml"
test -f "$Q/component/RebootTile.qml"
test -f "$Q/component/TweaksZonePicker.qml"
test -f "$TMP/root/app/share/AppHomeScreen/tweaks/tweaks_icon_n.png"
test -f "$TMP/root/app/share/AppHomeScreen/tweaks/reboot_icon_n.png"
test -f "$TMP/rw/tweaks_daemon"
test -f "$TMP/rw/tweaks_persist"
test -f "$TMP/rw/smack_expected"
[ "$(cat "$TMP/rw/smack_expected")" = "_" ]
test -f "$TMP/etc/systemd/system/tweaks-daemon.service"
grep -q 'id: rebootPage' "$Q/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'RebootTile' "$Q/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'rebootPage.open()' "$Q/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'smack_self=_' "$TMP/usb/run_stuff_result.txt"
grep -q 'daemon_smack=_' "$TMP/usb/run_stuff_result.txt"
grep -q 'smack_check=ok' "$TMP/usb/run_stuff_result.txt"
grep -q -- "-e _ $TMP/rw/tweaks_daemon" "$TMP/chsmack.log"
grep -q -- "-e _ $TMP/rw/tweaks_persist" "$TMP/chsmack.log"
grep -q -- "SMACK64 -v _" "$TMP/setfattr.log"
grep -q 'TWEAKS_WRANDR_WRAPPER' "$TMP/root/usr/bin/wrandr"
test -f "$TMP/root/usr/bin/wrandr.real"
grep -q 'TWEAKS_PAYLOAD_260128' "$Q/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'TweaksPage' "$Q/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
grep -q 'tweaksTile2' "$Q/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
if [ "$EXPECT_GLOBAL" = 1 ]; then
    grep -q 'property var slot: firstEmptySlot' "$Q/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    if grep -q 'tweaksTargetPage' "$Q/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"; then
        echo "old per-page helper leaked into $KIND" >&2
        exit 1
    fi
else
    grep -q 'tweaksTargetPage' "$Q/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
fi
grep -q 'TWEAKS_DISCLAIMER_260128' "$TMP/root/app/share/AppStandbyClock/qml_eco3.0/main.qml"
test -f "$TMP/rw/disclaimer_auto"
test -f "$TMP/rw/disclaimer_timeout_ms"
test -f "$TMP/rw/zones.txt"
test -f "$TMP/rw/tzdata_version"
test -f "$TMP/rw/zoneinfo/Europe/Amsterdam"
grep -q 'auto_dst=1' "$TMP/rw/config.conf"
grep -q 'ota_block=0' "$TMP/rw/config.conf"
grep -q '^config_version=2$' "$TMP/rw/config.conf"
grep -q '^deep_red=0$' "$TMP/rw/config.conf"

# The daemon must run on the SMACK floor label `_` (the Battery-proven
# context). `Privileged` has no micommanager/system-bus rule, which would
# silently block every cluster CAN write.
grep -q 'set_exec_label "$RW_DIR/tweaks_daemon"' "$ROOT/scripts/run_stuff.sh"
grep -q 'set_exec_label "$RW_DIR/tweaks_persist"' "$ROOT/scripts/run_stuff.sh"
grep -q 'chsmack -e "$SMACK_SELF"' "$ROOT/scripts/run_stuff.sh"
! grep -q 'chsmack -e Privileged' "$ROOT/scripts/run_stuff.sh"
grep -q ' smack=' "$TMP/usb/run_stuff_result.txt"

# Without Battery installed, the installer creates a local micom helper: a
# copy of dbus-send under the whitelisted basename CANService plus a wrapper
# with the same fixed/raw interface. The micom drops any other sender name.
test -x "$TMP/rw/bin/CANService"
test -x "$TMP/rw/bin/micom-send"
grep -q "^can_service=$TMP/rw/bin/micom-send$" "$TMP/rw/config.conf"
grep -q 'can_source=local' "$TMP/usb/run_stuff_result.txt"
grep -q -- "-e _ $TMP/rw/bin/CANService" "$TMP/chsmack.log"
grep -q -- "-e _ $TMP/rw/bin/micom-send" "$TMP/chsmack.log"
: > "$TMP/can.log"
"$TMP/rw/bin/micom-send" fixed 0x06 0x3f 26 2 2
grep -q 'SetCANFixedMicomRecv array:byte:0x06,0x3f int32:26 int32:2 int32:2' "$TMP/can.log"
"$TMP/rw/bin/micom-send" fixed 0x06 0x3f 26 4 5
grep -q 'SetCANFixedMicomRecv array:byte:0x06,0x3f int32:26 int32:4 int32:5' "$TMP/can.log"
"$TMP/rw/bin/micom-send" array:byte:0x04,0xa2,0x08,0xff
grep -q 'SetCANMicomRecv array:byte:0x04,0xa2,0x08,0xff' "$TMP/can.log"
"$TMP/rw/bin/micom-send" raw array:byte:0x04,0xa2,0x08,0xef
grep -q 'SetCANMicomRecv array:byte:0x04,0xa2,0x08,0xef' "$TMP/can.log"

# A legacy v1 config (zero-based CRTC pair) is migrated in place on the
# next install, without touching deliberate custom values.
sed -i 's/^config_version=2$/config_version=1/' "$TMP/rw/config.conf"
sed -i 's/^main_crtc=.*/main_crtc=0/' "$TMP/rw/config.conf"
sed -i 's/^cluster_crtc=.*/cluster_crtc=1/' "$TMP/rw/config.conf"
TWEAKS_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
TWEAKS_RW_DIR="$TMP/rw" \
TWEAKS_SERVICE="$TMP/etc/systemd/system/tweaks-daemon.service" \
TWEAKS_STANDBY_ROOT="$TMP/root/app/share/AppStandbyClock" \
TWEAKS_OFFLINE_STATUS="$TMP/root/update/offline_status" \
TWEAKS_WRANDR="$TMP/root/usr/bin/wrandr" \
TWEAKS_WRANDR_REAL="$TMP/root/usr/bin/wrandr.real" \
USB_DIR="$TMP/usb" \
sh "$TMP/install.sh"
grep -q '^config_version=2$' "$TMP/rw/config.conf"
grep -q '^main_crtc=1$' "$TMP/rw/config.conf"
grep -q '^cluster_crtc=2$' "$TMP/rw/config.conf"

# Reinstall over an existing Tweaks install must keep the same variant.
TWEAKS_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
TWEAKS_RW_DIR="$TMP/rw" \
TWEAKS_SERVICE="$TMP/etc/systemd/system/tweaks-daemon.service" \
TWEAKS_STANDBY_ROOT="$TMP/root/app/share/AppStandbyClock" \
TWEAKS_OFFLINE_STATUS="$TMP/root/update/offline_status" \
TWEAKS_WRANDR="$TMP/root/usr/bin/wrandr" \
TWEAKS_WRANDR_REAL="$TMP/root/usr/bin/wrandr.real" \
USB_DIR="$TMP/usb" \
sh "$TMP/install.sh"
grep -q "variant=$EXPECT" "$TMP/usb/run_stuff_result.txt" || {
    echo "unexpected reinstall variant:" >&2
    cat "$TMP/usb/run_stuff_result.txt" >&2
    exit 1
}

# The persist helper is optional (SKIP_PERSIST builds). set -u must not abort
# the final result when it is absent. Remove it from the payload and from the
# manifest together.
mv "$TMP/usb/payload/bin/tweaks_persist" "$TMP/tweaks_persist.bak"
cp "$TMP/usb/manifest.md5" "$TMP/manifest.md5.bak"
grep -v 'payload/bin/tweaks_persist$' "$TMP/manifest.md5.bak" > "$TMP/usb/manifest.md5"
rm -f "$TMP/usb/run_stuff_result.txt"
TWEAKS_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
TWEAKS_RW_DIR="$TMP/rw" \
TWEAKS_SERVICE="$TMP/etc/systemd/system/tweaks-daemon.service" \
TWEAKS_STANDBY_ROOT="$TMP/root/app/share/AppStandbyClock" \
TWEAKS_OFFLINE_STATUS="$TMP/root/update/offline_status" \
TWEAKS_WRANDR="$TMP/root/usr/bin/wrandr" \
TWEAKS_WRANDR_REAL="$TMP/root/usr/bin/wrandr.real" \
USB_DIR="$TMP/usb" \
sh "$TMP/install.sh"
grep -q 'status=OK' "$TMP/usb/run_stuff_result.txt"
grep -q 'persist_smack=absent' "$TMP/usb/run_stuff_result.txt"
mv "$TMP/tweaks_persist.bak" "$TMP/usb/payload/bin/tweaks_persist"
cp "$TMP/manifest.md5.bak" "$TMP/usb/manifest.md5"

# Battery's car-proven helper is preferred when present: a fresh config
# records it and no local copy is made. Only a fixed-capable helper qualifies
# (older Battery installs shipped a raw-only wrapper).
mkdir -p "$TMP/battery/bin"
printf '#!/bin/sh\n# SetCANFixedMicomRecv\ncase "$1" in\nfixed) : ;;\nesac\nexit 0\n' > "$TMP/battery/bin/micom-send"
chmod 755 "$TMP/battery/bin/micom-send"

# An existing valid config is preserved; the result must report the effective
# path (local), not merely the detected Battery helper.
TWEAKS_BATTERY_DIR="$TMP/battery" \
TWEAKS_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
TWEAKS_RW_DIR="$TMP/rw" \
TWEAKS_SERVICE="$TMP/etc/systemd/system/tweaks-daemon.service" \
TWEAKS_STANDBY_ROOT="$TMP/root/app/share/AppStandbyClock" \
TWEAKS_OFFLINE_STATUS="$TMP/root/update/offline_status" \
TWEAKS_WRANDR="$TMP/root/usr/bin/wrandr" \
TWEAKS_WRANDR_REAL="$TMP/root/usr/bin/wrandr.real" \
USB_DIR="$TMP/usb" \
sh "$TMP/install.sh"
grep -q 'status=OK' "$TMP/usb/run_stuff_result.txt"
grep -q 'can_source=local' "$TMP/usb/run_stuff_result.txt"
grep -q "^can_service=$TMP/rw/bin/micom-send$" "$TMP/rw/config.conf"

# A raw-only Battery helper must be rejected: the installer falls back to its
# own fixed-capable copy instead of exec'ing "fixed" as a raw array.
mkdir -p "$TMP/battery_raw/bin"
printf '#!/bin/sh\n# SetCANMicomRecv only\nexit 0\n' > "$TMP/battery_raw/bin/micom-send"
chmod 755 "$TMP/battery_raw/bin/micom-send"
rm -f "$TMP/rw/config.conf"
TWEAKS_BATTERY_DIR="$TMP/battery_raw" \
TWEAKS_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
TWEAKS_RW_DIR="$TMP/rw" \
TWEAKS_SERVICE="$TMP/etc/systemd/system/tweaks-daemon.service" \
TWEAKS_STANDBY_ROOT="$TMP/root/app/share/AppStandbyClock" \
TWEAKS_OFFLINE_STATUS="$TMP/root/update/offline_status" \
TWEAKS_WRANDR="$TMP/root/usr/bin/wrandr" \
TWEAKS_WRANDR_REAL="$TMP/root/usr/bin/wrandr.real" \
USB_DIR="$TMP/usb" \
sh "$TMP/install.sh"
grep -q 'status=OK' "$TMP/usb/run_stuff_result.txt"
grep -q 'can_source=local' "$TMP/usb/run_stuff_result.txt"
grep -q "^can_service=$TMP/rw/bin/micom-send$" "$TMP/rw/config.conf"
grep -q 'Battery micom helper is raw-only' "$TMP/rw/install.log"

# Fresh config with a fixed-capable Battery helper.
rm -f "$TMP/rw/config.conf"
TWEAKS_BATTERY_DIR="$TMP/battery" \
TWEAKS_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
TWEAKS_RW_DIR="$TMP/rw" \
TWEAKS_SERVICE="$TMP/etc/systemd/system/tweaks-daemon.service" \
TWEAKS_STANDBY_ROOT="$TMP/root/app/share/AppStandbyClock" \
TWEAKS_OFFLINE_STATUS="$TMP/root/update/offline_status" \
TWEAKS_WRANDR="$TMP/root/usr/bin/wrandr" \
TWEAKS_WRANDR_REAL="$TMP/root/usr/bin/wrandr.real" \
USB_DIR="$TMP/usb" \
sh "$TMP/install.sh"
grep -q 'status=OK' "$TMP/usb/run_stuff_result.txt"
grep -q 'can_source=battery' "$TMP/usb/run_stuff_result.txt"
grep -q "^can_service=$TMP/battery/bin/micom-send$" "$TMP/rw/config.conf"

# Pretend the user enabled OTA blocking before restoring.
printf '1' > "$TMP/rw/ota_block_owned"
printf '1' > "$TMP/root/update/offline_status"
# Pretend the daemon left a cluster/front override behind (crash case).
printf '1' > "$TMP/rw/blu_forced"
printf 'crtc=1\nrestore=128:120:112\n' > "$TMP/rw/front_override"
: > "$TMP/wrandr.log"

TWEAKS_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
TWEAKS_RW_DIR="$TMP/rw" \
TWEAKS_SERVICE="$TMP/etc/systemd/system/tweaks-daemon.service" \
TWEAKS_STANDBY_ROOT="$TMP/root/app/share/AppStandbyClock" \
TWEAKS_OFFLINE_STATUS="$TMP/root/update/offline_status" \
TWEAKS_WRANDR="$TMP/root/usr/bin/wrandr" \
TWEAKS_WRANDR_REAL="$TMP/root/usr/bin/wrandr.real" \
USB_DIR="$TMP/usb" \
PATH="$TMP/root/usr/bin:$PATH" \
sh "$TMP/restore.sh"

grep -q 'status=OK' "$TMP/usb/run_stuff_result.txt"
grep -q -- '--crtc=1 --brightness=128:120:112' "$TMP/wrandr.log"
grep -q "restored cluster blue-light off via $TMP/battery/bin/micom-send" "$TMP/rw/restore.log"
test ! -f "$TMP/rw/front_override"
test ! -d "$TMP/rw/bin"
test ! -f "$Q/HomeScreen/TweaksPage.qml"
test ! -f "$Q/HomeScreen/RebootPage.qml"
test ! -f "$Q/component/TweaksTile.qml"
test ! -f "$Q/component/RebootTile.qml"
test ! -f "$Q/component/TweaksZonePicker.qml"
test ! -f "$TMP/rw/tweaks_daemon"
test ! -f "$TMP/root/usr/bin/wrandr.real"
! grep -q 'TWEAKS_WRANDR_WRAPPER' "$TMP/root/usr/bin/wrandr"
! grep -q 'TWEAKS_PAYLOAD_260128' "$Q/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
! grep -q 'TWEAKS_DISCLAIMER_260128' "$TMP/root/app/share/AppStandbyClock/qml_eco3.0/main.qml"
grep -q 'STOCK_STANDBY_260128' "$TMP/root/app/share/AppStandbyClock/qml_eco3.0/main.qml"
test ! -d "$TMP/rw/zoneinfo"
test ! -f "$TMP/rw/disclaimer_auto"
test ! -f "$TMP/rw/tzdata_version"
test "$(cat "$TMP/root/update/offline_status")" = "0"
test ! -f "$TMP/rw/ota_block_owned"

# The Reboot fallback requires the floor label.  A failed SMACK readback or a
# daemon running outside the installer label must fail the install, not warn.
echo "== smack installer label gate (negative) =="
if TWEAKS_SMACK_SELF=Privileged \
    TWEAKS_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
    TWEAKS_RW_DIR="$TMP/rw" \
    TWEAKS_SERVICE="$TMP/etc/systemd/system/tweaks-daemon.service" \
    TWEAKS_STANDBY_ROOT="$TMP/root/app/share/AppStandbyClock" \
    TWEAKS_OFFLINE_STATUS="$TMP/root/update/offline_status" \
    TWEAKS_WRANDR="$TMP/root/usr/bin/wrandr" \
    TWEAKS_WRANDR_REAL="$TMP/root/usr/bin/wrandr.real" \
    USB_DIR="$TMP/usb" \
    sh "$TMP/install.sh"; then
    echo "installer accepted a non-floor SMACK label" >&2
    exit 1
fi
grep -q 'status=FAIL' "$TMP/usb/run_stuff_result.txt"
grep -q "expected SMACK floor label" "$TMP/usb/run_stuff_result.txt"
echo "smack installer label gate OK"

echo "== smack label gate (negative) =="
if BAD_LABEL=1 TWEAKS_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
    TWEAKS_RW_DIR="$TMP/rw" \
    TWEAKS_SERVICE="$TMP/etc/systemd/system/tweaks-daemon.service" \
    TWEAKS_STANDBY_ROOT="$TMP/root/app/share/AppStandbyClock" \
    TWEAKS_OFFLINE_STATUS="$TMP/root/update/offline_status" \
    TWEAKS_WRANDR="$TMP/root/usr/bin/wrandr" \
    TWEAKS_WRANDR_REAL="$TMP/root/usr/bin/wrandr.real" \
    USB_DIR="$TMP/usb" \
    sh "$TMP/install.sh"; then
    echo "installer accepted a wrong SMACK exec label" >&2
    exit 1
fi
grep -q 'status=FAIL' "$TMP/usb/run_stuff_result.txt"
grep -q 'exec label readback failed' "$TMP/usb/run_stuff_result.txt"
echo "smack file gate OK"

echo "== exec-label readback unavailable (onlycap on-car behavior) =="
if UNREADABLE_LABEL=1 TWEAKS_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
    TWEAKS_RW_DIR="$TMP/rw" \
    TWEAKS_SERVICE="$TMP/etc/systemd/system/tweaks-daemon.service" \
    TWEAKS_STANDBY_ROOT="$TMP/root/app/share/AppStandbyClock" \
    TWEAKS_OFFLINE_STATUS="$TMP/root/update/offline_status" \
    TWEAKS_WRANDR="$TMP/root/usr/bin/wrandr" \
    TWEAKS_WRANDR_REAL="$TMP/root/usr/bin/wrandr.real" \
    USB_DIR="$TMP/usb" \
    sh "$TMP/install.sh"; then
    grep -q 'status=OK' "$TMP/usb/run_stuff_result.txt"
    grep -q 'smack=unverified' "$TMP/usb/run_stuff_result.txt"
    grep -q 'persist_smack=unverified' "$TMP/usb/run_stuff_result.txt"
    grep -q 'cannot read back SMACK64EXEC' "$TMP/rw/install.log"
else
    echo "installer failed with unreadable SMACK64EXEC readback" >&2
    cat "$TMP/usb/run_stuff_result.txt" >&2
    exit 1
fi
echo "unreadable readback gate OK"

echo "== smack runtime gate (negative) =="
if TWEAKS_DAEMON_SMACK=Privileged \
    TWEAKS_QML_ROOT="$TMP/root/app/share/AppHomeScreen" \
    TWEAKS_RW_DIR="$TMP/rw" \
    TWEAKS_SERVICE="$TMP/etc/systemd/system/tweaks-daemon.service" \
    TWEAKS_STANDBY_ROOT="$TMP/root/app/share/AppStandbyClock" \
    TWEAKS_OFFLINE_STATUS="$TMP/root/update/offline_status" \
    TWEAKS_WRANDR="$TMP/root/usr/bin/wrandr" \
    TWEAKS_WRANDR_REAL="$TMP/root/usr/bin/wrandr.real" \
    USB_DIR="$TMP/usb" \
    sh "$TMP/install.sh"; then
    echo "installer accepted a daemon outside the installer label" >&2
    exit 1
fi
grep -q 'status=FAIL' "$TMP/usb/run_stuff_result.txt"
grep -q 'smack_check=mismatch' "$TMP/usb/run_stuff_result.txt"
echo "smack runtime gate OK"

echo "PASS: $KIND install/restore"
