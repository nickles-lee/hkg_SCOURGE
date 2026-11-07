#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Research updater initialization with unmodified apps, not genuine crypto proof."""
from pathlib import Path
import shlex
import sys

VM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(VM))
import launch

mounts = '''set -eu
modprobe ehci-pci; modprobe usb-storage; modprobe sd_mod
mkdir -p /media/tools /media/graphics
for role in tools graphics; do
  found=0
  for attempt in {1..20}; do
    for dev in /dev/sd?; do
      [ -b "$dev" ] || continue
      mount -o ro "$dev" "/media/$role" 2>/dev/null || continue
      if { [ "$role" = tools ] && [ -d /media/tools/mesa112 ]; } ||
         { [ "$role" = graphics ] && [ -f /media/graphics/GRAPHICS_RUNTIME ]; }; then
        found=1; break
      fi
      umount "/media/$role"
    done
    [ "$found" = 1 ] && break
    sleep 1
  done
  [ "$found" = 1 ]
done
export HARNESS_GRAPHICS_RUNTIME=/media/graphics/runtime
export USE_MESA=1 HARNESS_COMPOSITOR=noop
export QSG_INFO=1
set +e
'''
after = '''
phase_rc=$?
sleep 15
echo '=== POST-LAUNCH UPDATER OUTPUT ==='
tail -220 /tmp/appupgrade.out 2>/dev/null
echo '=== POST-LAUNCH ENGINE LOG ==='
tail -220 /log_data/AppUpgrade/upgrade_log 2>/dev/null
echo '=== POST-LAUNCH PROCESSES ==='
ps -ef | grep -E 'AppUpgrade|AppDMClient|CryptoService|DecryptToPIPE|tar' | grep -v grep
echo '=== POST-LAUNCH FILES ==='
find /log_data/AppUpgrade /update -maxdepth 4 -type f -printf '%p %s\\n' 2>/dev/null | head -80
exit "$phase_rc"
'''

if __name__ == '__main__':
    command = mounts + '/bin/bash -c ' + shlex.quote((VM / 'toolsroot/phase4.sh').read_text()) + after
    sys.exit(launch.run(command, False, 240, usb=VM / 'update-k.img', tools=VM / 'tools.dev.img',
                        payload=VM / 'graphics-xenial-v3/payload', storage_mode='split', collect_audit=True))
