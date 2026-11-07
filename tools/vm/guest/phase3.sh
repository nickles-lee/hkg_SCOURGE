#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Phase-3 vendor stack on the EU 260128 rootfs.
# Boots D-Bus + Weston headless + micom bootinfo + AppLauncher + AppDMClient,
# then emits the automount UsbMount signal for /media/usb.
set +e
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/app/bin

MEDIA=${MEDIA:-/media/usb}
DEV=${DEV:-/dev/sda}
BOOTINFO=00000826050000051302000003c8140000000000000000000000020001000000000000

modprobe ehci-pci
modprobe usb-storage
modprobe sd_mod
modprobe texfat
sleep 2

mkdir -p "$MEDIA"
mount "$DEV" "$MEDIA"

ifconfig lo 127.0.0.1 up 2>/dev/null || ip link set lo up 2>/dev/null

# esinit normally provides these.
cp /etc/passwd.legacy /tmp/passwd
ln -sf /etc/shadow.Global /tmp/shadow
mkdir -p /rw_data/app_data /rw_data/usr_data /rw_data/var /rw_data/home \
         /rw_data/vr_data /rw_data/mnt /rw_data/var/run/dbus \
         /rw_data/var/log /rw_data/dlt /Data/mnt-c /Data/mnt-wt \
         /Data/mnt-backup /run/dbus /log_data /run/user/root \
         /update/usb/bin
chmod 700 /run/user/root

# The engine expects its own copy of the updater and the stock XML.
cp /app/bin/AppUpgrade /update/usb/bin/ 2>/dev/null
cp "$MEDIA/.lge.upgrade.xml" /update/usb/bin/ 2>/dev/null

# Headless Weston with the IVI shell.
printf '[core]\nshell=ivi-shell.so\n[ivi-shell]\nivi-module=ivi-window-manager.so,ivi-controller.so\nivi-shell-user-interface=/bin/true\n' >/tmp/weston.ini
export XDG_RUNTIME_DIR=/run/user/root
/usr/bin/weston --backend=headless-backend.so --config=/tmp/weston.ini \
    --use-pixman --width=1920 --height=720 --socket=wayland-0 \
    --idle-time=0 --debug >/tmp/weston.log 2>&1 &
sleep 4

# System and session buses.
dbus-daemon --system --fork
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/var/run/dbus/system_bus_socket
dbus-daemon --session --fork --print-address >/tmp/session.addr
export DBUS_SESSION_BUS_ADDRESS="$(cat /tmp/session.addr)"

# DLT for app logs.
dlt-daemon -d >/tmp/dlt.err 2>&1
/usr/bin/dlt-receive -a localhost >/tmp/dlt.out 2>&1 &

export WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
       QT_QUICK_BACKEND=software \
       LD_LIBRARY_PATH=/app/lib:/navi/navi/Lib:/usr/lib HOME=/root

# Micom boot-info service (exact 35-byte EU 260128 profile).
/micom-dbus-sim "$BOOTINFO" >/tmp/micom.log 2>&1 &
sleep 2

# Vendor launcher + manager.
/app/bin/AppLauncher 3 >/tmp/al.out 2>&1 &
sleep 40

# Launch AppDMClient through the vendor launcher API (AppID 12/311).
dbus-send --session --dest=com.lge.avn.applauncher --type=method_call \
    --print-reply /Request com.lge.avn.applauncher.request.launch \
    int32:12 string:AppDMClient string:/app/bin/AppDMClient >/tmp/launch.out 2>&1
sleep 60

# Emit the same signal usb-automount sends, from the service owner name.
/usb-mount-signal "$MEDIA" "$DEV" ext4 1
sleep 5
/usb-mount-signal "$MEDIA" "$DEV" ext4 1
sleep 12

echo "=== PHASE3 PROCESSES ==="
ps -ef | grep -E 'AppLauncher|ApplicationManager|AppDMClient|AppUpgrade' | grep -v grep
echo "=== PHASE3 UPDATER TREE ==="
ls -l /update/usb/bin 2>&1
echo "=== PHASE3 APPUPGRADE ==="
ps -ef | grep -i appupgrade | grep -v grep
echo "=== PHASE3 APP LOG ==="
grep -aE 'AppID : 311|AppDMClient|Usb|update|AppUpgrade|checkUSB|m_Usb|force' /tmp/al.out | tail -30
echo "=== PHASE3 DLT ==="
grep -aiE 'DMCL|AppDM|Usb|update|AppUpgrade|check' /tmp/dlt.out | tail -30
echo "=== PHASE3 DONE ==="
