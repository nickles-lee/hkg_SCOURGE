#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Read-only WiFi/CarPlay diagnostic for Ioniq 5 Gen5W 260128.
# Writes wifi_diag.txt to the USB root and /rw_data/wifi/diag.txt.
set -u
PATH=/bin:/sbin:/usr/bin:/usr/sbin
USB_DIR="${USB_DIR:-/media/usb}"
OUT="$USB_DIR/wifi_diag.txt"
RW_OUT="/rw_data/wifi/diag.txt"

mkdir -p /rw_data/wifi 2>/dev/null
: > "$OUT" 2>/dev/null

sec() { echo "" >> "$OUT"; echo "===== $* =====" >> "$OUT"; }
run() { echo "\$ $*" >> "$OUT"; "$@" >> "$OUT" 2>&1 || true; }
raw() { echo "\$ $*" >> "$OUT"; sh -c "$*" >> "$OUT" 2>&1 || true; }

{
    echo "WIFI_DIAG $(date -u 2>/dev/null)"
    echo "uname: $(uname -a 2>/dev/null)"
} >> "$OUT"

sec "tool availability"
raw 'ls -l /sbin/chsmack /usr/bin/qt5/qmllint 2>&1'
raw 'command -v md5sum md5sum.coreutils busybox dbus-monitor dbus-send chsmack 2>&1'
raw 'busybox 2>&1 | grep -E "wget|md5sum" || echo "busybox applet list unavailable"'

sec "micom / variant"
raw 'for p in pcb_version optUNECE country hw_version; do echo -n "$p="; cat /sys/module/micom/parameters/$p 2>/dev/null; done'
raw 'ls -l /tmp/no_unece 2>&1'
raw 'getprop 2>/dev/null | head'

sec "PCI / modules"
raw 'lspci -nn 2>/dev/null | grep -iE "14e4|network|wireless"'
raw 'lsmod | grep -E "bcmdhd|cfg80211"'
raw 'dmesg | grep -iE "bcmdhd|wlan|firmware|nvram" | tail -60'

sec "network interfaces"
raw 'ifconfig -a 2>/dev/null'
raw 'cat /proc/net/wireless 2>/dev/null'
raw 'iw dev 2>/dev/null'
raw 'iw dev wlan0 link 2>/dev/null'
raw 'iwconfig 2>/dev/null'

sec "services"
raw 'systemctl status supplicantmanager.path supplicantmanager.service connman.service 2>&1 | head -120'
raw 'systemctl is-enabled supplicantmanager.path supplicantmanager.service 2>&1'
raw 'systemctl status carplay_iAP2.service carplay_mdnsd.service 2>&1 | head -120'
raw 'systemctl status wifi-daemon.service 2>&1 | head -160'
raw 'systemctl cat wifi-daemon.service 2>&1'
raw 'journalctl -u wifi-daemon.service --no-pager -n 120 2>&1 | grep -E "wifi_daemon started:|watchdog:|apply (ok|failed)| msg:"'
raw 'ps w 2>/dev/null | grep -E "connmand|wpa_supplicant|supplicantmanager|iAP2|mdnsd|AppProjection|vbtd|wifi_daemon|dropbear" | grep -v grep'

sec "WiFi supplicant"
raw 'ps w 2>/dev/null | grep -E "wpa_supplicant|supplicantmanager|connmand" | grep -v grep'
raw 'wpa_cli -p /var/run/wpa_supplicant -i wlan0 ping 2>&1'
raw 'wpa_cli -p /var/run/wpa_supplicant -i wlan0 status 2>&1'
raw 'wpa_cli -p /var/run/wpa_supplicant -i wlan0 get ap_scan 2>&1'
raw 'wpa_cli -p /var/run/wpa_supplicant -i wlan0 list_networks 2>&1'
raw 'wpa_cli -p /var/run/wpa_supplicant -i wlan0 scan_results 2>&1 | head -40'
raw 'cat /rw_data/wifi/wpa_supplicant.conf 2>&1'
raw 'cat /etc/wpa_supplicant.conf 2>&1'
raw 'tail -n 120 /rw_data/wifi/wpa_supplicant.log 2>/dev/null | grep -E "State:|CTRL-EVENT-|AP-(ENABLED|DISABLED)|Failed to start AP functionality"'
raw 'getfattr -n security.SMACK64 /usr/sbin/wpa_supplicant /usr/sbin/wpa_cli /usr/sbin/connmanctl /rw_data/wifi/wifi_daemon /rw_data/wifi/bin/dropbearmulti 2>&1'
raw 'getfattr -n security.SMACK64EXEC /usr/sbin/wpa_supplicant /usr/sbin/wpa_cli /usr/sbin/connmanctl /rw_data/wifi/wifi_daemon /rw_data/wifi/bin/dropbearmulti 2>&1'
raw 'ls -la /var/run/wpa_supplicant 2>&1'
raw 'iw dev wlan0 info 2>&1'
raw 'iw list 2>/dev/null | grep -A8 -i "Supported interface modes"'

sec "Bluetooth / vbtd"
raw 'ps w 2>/dev/null | grep -E "vbtd|BluetoothHMI" | grep -v grep'
raw 'systemctl status vbtd.service 2>&1 | head -80'
raw 'journalctl -u vbtd.service --no-pager -n 80 2>&1'
raw 'ls -l /sys/class/bluetooth 2>&1'
raw 'cat /sys/class/bluetooth/hci0/address 2>&1'
raw 'cat /sys/class/bluetooth/hci0/name 2>&1'
raw 'dmesg 2>/dev/null | grep -iE "bluetooth|hci0|btusb|patchram" | tail -80'
raw 'grep -n ENABLE_WIRELESS_CARPLAY /app/data/vbt/VBTConfig.json /rw_data/app_data/vbt/VBTConfig.json 2>/dev/null'
raw 'grep -n "ENABLE_WIRELESS_CARPLAY" /app/data/vbt/VBTConfig.json /rw_data/app_data/vbt/VBTConfig.json 2>/dev/null'
raw 'cat /tmp/session_appmgr.env 2>&1; cat /tmp/session_appmgr 2>&1'
raw '. /tmp/session_appmgr.env 2>/dev/null; dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames 2>&1 | grep -iE "blue|vbt|Bluetooth"'
raw '. /tmp/session_appmgr.env 2>/dev/null; dbus-send --session --print-reply --dest=org.bluez.Adapter.Adapter.v2_0 /org/bluez/Adapter/Adapter/v2_0 org.freedesktop.DBus.Introspectable.Introspect 2>&1 | head -80'
raw '. /tmp/session_appmgr.env 2>/dev/null; dbus-send --session --print-reply --dest=org.bluez.Adapter.Adapter.v2_0 /org/bluez/Adapter/Adapter/v2_0 org.bluez.Adapter.Adapter.getAddressAttribute 2>&1'
raw '. /tmp/session_appmgr.env 2>/dev/null; dbus-send --session --print-reply --dest=com.lge.hkmc.avn.BluetoothHMI /com/lge/hkmc/avn/BluetoothHMI org.freedesktop.DBus.Introspectable.Introspect 2>&1 | head -80'

sec "secure console / SSH debug"
raw 'echo -n "secure_console="; cat /sys/module/micom/parameters/secure_console 2>/dev/null'
raw 'echo -n "iptable_debug="; cat /sys/module/micom/parameters/iptable_debug 2>/dev/null'
raw 'ls -l /tmp/secure_console_disable /tmp/shells 2>&1'
raw 'readlink -f /etc/passwd /etc/shadow /etc/shells 2>&1'
raw 'grep -E "^car:" /etc/passwd /etc/passwd.legacy /etc/passwd.secure 2>/dev/null'
raw 'ls -l /usr/sbin/dropbear* /usr/sbin/dropbearmulti 2>&1'
raw 'systemctl status dropbear.socket dropbear@*.service 2>&1 | head -100'
raw 'ss -ltnp 2>/dev/null | grep -E ":22 |:2222 "'
raw 'ps w 2>/dev/null | grep -E "dropbear|agetty|wifi_daemon" | grep -v grep'

sec "connman"
raw 'connmanctl technologies 2>&1'
raw 'connmanctl services 2>&1'

sec "VBT / CarPlay config"
raw 'ls -l /app/data/vbt /rw_data/app_data/vbt 2>&1'
raw 'grep -n ENABLE_WIRELESS_CARPLAY /app/data/vbt/VBTConfig.json /rw_data/app_data/vbt/VBTConfig.json 2>/dev/null'
raw 'ls -l /app/data/AppProjection /rw_data/app_data/AppProjection 2>&1'
raw 'cat /app/data/AppProjection/USBSerial.txt 2>/dev/null'
raw 'cat /rw_data/app_data/AppProjection/USBSerial.txt 2>/dev/null'
raw 'cat /sys/class/bluetooth/hci0/address 2>/dev/null'

sec "firewall"
raw 'iptables -S 2>&1 | head -160'
raw 'ip6tables -S 2>&1 | head -160'

sec "USB mounts"
raw 'mount 2>/dev/null | grep -iE "usb|media"'
raw 'ls -la /media/usb /run/media 2>&1'

sec "QML hashes / markers"
for f in \
  /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml \
  /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Wide.qml \
  /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/Screen_HomeScreen_AllMenus.qml \
  /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml \
  /app/share/AppHomeScreen/qml_eco3.0/component/QCustomizedMenuListAllowEmpty.qml \
  /app/share/AppHomeScreen/qml_eco3.0/component/common/Q_ThemeImage.qml; do
    echo "$f" >> "$OUT"
    md5sum "$f" >> "$OUT" 2>&1 || true
    grep -oE 'HELLO_PAYLOAD_260128|BATTERY_PAYLOAD_260128|WIFI_PAYLOAD_260128|TWEAKS_PAYLOAD_260128' "$f" 2>/dev/null | sort -u >> "$OUT"
done

sec "tile placement"
raw 'ls -l /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/WifiPage.qml /app/share/AppHomeScreen/qml_eco3.0/component/WifiTile.qml /app/share/AppHomeScreen/qml_eco3.0/component/HelloTile.qml /app/share/AppHomeScreen/qml_eco3.0/component/BatteryTile.qml /app/share/AppHomeScreen/qml_eco3.0/component/TweaksTile.qml 2>&1'
raw 'for f in /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Wide.qml; do echo "--- $f"; grep -nE "firstEmptySlot|WifiTile|HelloTile|BatteryTile|TweaksTile|totalPageCount|slot.page|itemWifi|itemHello|itemTweaks" "$f" 2>/dev/null | head -120; done'
raw 'for f in /app/share/AppHomeScreen/qml_eco3.0/component/WifiTile.qml /app/share/AppHomeScreen/qml_eco3.0/component/HelloTile.qml /app/share/AppHomeScreen/qml_eco3.0/component/BatteryTile.qml /app/share/AppHomeScreen/qml_eco3.0/component/TweaksTile.qml; do echo "--- $f"; grep -nE "color:|source:|themeSource:|text:" "$f" 2>/dev/null | head -40; done'
raw 'ls -l /app/share/AppHomeScreen/wifi /app/share/AppHomeScreen/hello /app/share/AppHomeScreen/battery /app/share/AppHomeScreen/tweaks 2>&1'

sec "install state"
raw 'sed -n '\''s/.*"build_id":"\([^"]*\)".*/build_id=\1/p'\'' /rw_data/wifi/status.json 2>/dev/null'
raw 'cat /rw_data/wifi/install.log 2>/dev/null | tail -n 160'
raw 'cat /rw_data/wifi/config.conf 2>/dev/null | sed "s/^psk=.*/psk=<redacted>/; s/^carplay_psk=.*/carplay_psk=<redacted>/"'
raw 'cat /rw_data/wifi/status.json 2>/dev/null'

sec "daemon state"
raw 'grep -RIn "wifiPage\\.\\(visible\\|open\\|close\\)" /app/share/AppHomeScreen/qml_eco3.0 2>/dev/null | head -80'
raw 'md5sum /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/WifiPage.qml /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml /app/share/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Wide.qml 2>&1'
raw 'journalctl -b --no-pager 2>/dev/null | grep -iE "WifiPage|qml" | tail -n 200'
raw 'cat /rw_data/wifi/status.json 2>/dev/null'
raw 'tail -n 120 /rw_data/wifi/daemon.log 2>/dev/null | grep -E "wifi_daemon started:|watchdog:|apply (ok|failed)| msg:"'
raw 'tail -n 120 /rw_data/wifi/wpa_supplicant.log 2>/dev/null | grep -E "State:|CTRL-EVENT-|AP-(ENABLED|DISABLED)|Failed to start AP functionality"'
raw 'cat /rw_data/wifi/config.conf 2>/dev/null | sed "s/^psk=.*/psk=<redacted>/; s/^carplay_psk=.*/carplay_psk=<redacted>/"'

# Structural filtering also covers historical/current keys that differ from
# config.conf. Omit entire secret-bearing JSON/config/transcript lines.
awk '{ lower=tolower($0); if (lower ~ /psk|passphrase|password|private key|hexdump|connmanctl>|error '\''/) next; print }' "$OUT" > "$OUT.redacted.$$" || exit 1
mv -f "$OUT.redacted.$$" "$OUT" || exit 1
cp -f "$OUT" "$RW_OUT" 2>/dev/null || true
echo "wrote $OUT"
exit 0
