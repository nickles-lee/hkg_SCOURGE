#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Direct engine with plaintext XML + encryption-disable marker.
set +e
mkdir -p /media/usb/HU/firmware /update
[ -f /media/usb/HU/firmware/update.tar.gz ] || cp /media/tools/update.tar.gz /media/usb/HU/firmware/
cp -f /media/tools/engine.elf /media/usb/engine.elf
cp -f /media/tools/xml.plain /media/usb/.lge.upgrade.xml
touch /update/disableEncryptionUpdate /update/UsbUpdateStartWithoutConfirm
cd /media/usb
QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software \
LD_LIBRARY_PATH=/app/lib:/navi/navi/Lib:/usr/lib \
    /media/usb/engine.elf >/tmp/engine.out 2>&1 &
p=$!
sleep 60
echo "===ENGINE PS==="
ps -ef | grep -E 'engine|AppUpgrade' | grep -v grep
echo "===ENGINE OUT==="
head -80 /tmp/engine.out
echo "===UPGRADE LOG==="
tail -160 /log_data/AppUpgrade/upgrade_log 2>&1
echo "===RW SCRIPT==="
ls -la /rw_data/mnt/root/etc/update_start_5w.sh 2>&1
kill -9 "$p" 2>/dev/null
