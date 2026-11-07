#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Manual AppUpgrade probe inside the guest (research only).
set +e
modprobe ehci-pci; modprobe usb-storage; modprobe sd_mod; sleep 3
mkdir -p /media/tools /media/usb /update/usb/bin
mount /dev/sdb /media/tools
mount /dev/sda /media/usb
cp /app/bin/AppUpgrade /update/usb/bin/AppUpgrade
cp /media/usb/.lge.upgrade.xml /update/usb/bin/
chmod +x /update/usb/bin/AppUpgrade
cd /update/usb/bin
strace -f -s 200 -o /tmp/au.strace ./AppUpgrade >/tmp/au.out 2>&1 &
p=$!
sleep 10
ps -ef | grep -E 'AppUpgrade|embedded' | grep -v grep
kill -9 $p 2>/dev/null
echo "===AUOUT==="
cat /tmp/au.out
echo "===AUSTRACE==="
tail -120 /tmp/au.strace
echo "===LOGDATA==="
find /log_data /update -maxdepth 3 -type f 2>/dev/null | head -40
