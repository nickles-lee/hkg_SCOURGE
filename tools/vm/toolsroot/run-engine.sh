#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Run the carved embedded engine directly, headless. Research only.
set +e
mkdir -p /media/usb/HU/firmware
[ -f /media/usb/HU/firmware/update.tar.gz ] || cp /media/tools/update.tar.gz /media/usb/HU/firmware/
cp -f /media/tools/engine.elf /media/usb/engine.elf
cd /media/usb
touch /update/UsbUpdateStartWithoutConfirm
QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-offscreen} \
QT_QUICK_BACKEND=${QT_QUICK_BACKEND:-software} \
LD_LIBRARY_PATH=/app/lib:/navi/navi/Lib:/usr/lib \
    /media/usb/engine.elf "$@" >/tmp/engine.out 2>&1 &
p=$!
sleep 80
echo "===ENGINE PS==="
ps -ef | grep -E 'engine|AppUpgrade' | grep -v grep
echo "===ENGINE OUT==="
head -120 /tmp/engine.out
echo "===ENGINE LOGDATA==="
find /log_data /update /rw_data/mnt -maxdepth 5 -type f 2>/dev/null | head -80
echo "===UPGRADE LOG==="
tail -120 /log_data/AppUpgrade/upgrade_log 2>&1
echo "===MEDIA==="
ls -la /media/usb /update/usb/bin 2>&1 | head -40
kill -9 "$p" 2>/dev/null
