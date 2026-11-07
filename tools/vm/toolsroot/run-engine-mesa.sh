#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Direct engine, plaintext XML, Mesa 11.2 swrast + headless Weston.
set +e
mkdir -p /run/user/root /media/usb/HU/firmware /update/usb/bin /rw_data/var/run/dbus /var/run/dbus
chmod 700 /run/user/root
[ -f /media/usb/HU/firmware/update.tar.gz ] || cp /media/tools/update.tar.gz /media/usb/HU/firmware/
cp -f /media/tools/engine.elf /media/usb/engine.elf
cp -f /media/tools/xml.plain /media/usb/.lge.upgrade.xml
touch /update/disableEncryptionUpdate /update/UsbUpdateStartWithoutConfirm

cp /etc/passwd.legacy /tmp/passwd 2>/dev/null
ln -sf /etc/shadow.Global /tmp/shadow 2>/dev/null
dbus-daemon --system --fork
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/var/run/dbus/system_bus_socket
/usr/bin/dbus-monitor --system >/tmp/dbus-system-engine.log 2>&1 &
/micom-dbus-sim 00000826050000051302000003c8140000000000000000000000020001000000000000 >/tmp/micom.log 2>&1 &
sleep 1
cp -f /media/tools/automount-stub /usr/bin/automount-stub 2>/dev/null
/usr/bin/automount-stub /media/usb /dev/sda ext4 >/tmp/automount-stub.log 2>&1 &
sleep 1
export XDG_RUNTIME_DIR=/run/user/root
printf '[core]\nshell=ivi-shell.so\n[ivi-shell]\nivi-module=ivi-window-manager.so,ivi-controller.so\nivi-shell-user-interface=/bin/true\n' >/tmp/weston.ini
/usr/bin/weston --backend=headless-backend.so --config=/tmp/weston.ini \
    --use-pixman --width=1920 --height=720 --socket=wayland-0 \
    --idle-time=0 --debug >/tmp/weston.log 2>&1 &
sleep 3

cd /media/usb
export LD_LIBRARY_PATH=/media/tools/mesa112/usr/lib/x86_64-linux-gnu/mesa-egl:/media/tools/mesa112/usr/lib/x86_64-linux-gnu:/media/tools/llvm38/usr/lib/x86_64-linux-gnu:/app/lib:/navi/navi/Lib:/usr/lib
export LIBGL_DRIVERS_PATH=/media/tools/mesa112/usr/lib/x86_64-linux-gnu/dri
export MESA_LOADER_DRIVER_OVERRIDE=swrast
export EGL_PLATFORM=wayland
export QT_QPA_PLATFORM=wayland
export WAYLAND_DISPLAY=wayland-0
export QT_QUICK_BACKEND=software
export QT_LOGGING_RULES='qt.qpa.*=true'
/media/usb/engine.elf >/tmp/engine.out 2>&1 &
p=$!
sleep 8
/mnt/usb-mount-signal 2>/dev/null || dbus-send --system --dest=com.lge.car.automount --type=method_call /com/lge/car/automount com.lge.car.automount.trigger_mount >/dev/null 2>&1
sleep 6
dbus-send --system --dest=com.lge.car.automount --type=method_call /com/lge/car/automount com.lge.car.automount.trigger_mount >/dev/null 2>&1
sleep 76
echo "===ENGINE PS==="
ps -ef | grep -E 'engine|weston' | grep -v grep
echo "===ENGINE OUT==="
head -160 /tmp/engine.out
echo "===WESTON==="
tail -40 /tmp/weston.log
echo "===UPGRADE LOG==="
tail -200 /log_data/AppUpgrade/upgrade_log 2>&1
echo "===DBUS==="
tail -80 /tmp/dbus-system-engine.log 2>&1
echo "===RW DATA==="
ls -la /rw_data/dlt /rw_data/mnt/root/etc/update_start_5w.sh 2>&1
kill -9 "$p" 2>/dev/null
