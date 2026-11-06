#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 12_verify_pi.sh — verify the Pi rig is provisioned correctly.
# Run from the build host; PI_HOST/PI_USER come from config/paths.local.env.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

[ -n "$PI_HOST" ] || { echo "set PI_HOST/PI_USER in config/paths.local.env" >&2; exit 1; }

echo "== udc"
ssh "$PI_USER@$PI_HOST" 'ls /sys/class/udc'
echo "== dwc2 driver bound"
ssh "$PI_USER@$PI_HOST" 'readlink /sys/bus/platform/devices/3f980000.usb/driver || true'
echo "== watchdog"
ssh "$PI_USER@$PI_HOST" 'systemctl show -p RuntimeWatchdogUSec; dmesg | grep -i "hardware watchdog" || echo clean'
echo "== packages"
ssh "$PI_USER@$PI_HOST" 'command -v nbdkit nbd-client mkfs.exfat || true'
echo "== artifacts"
ssh "$PI_USER@$PI_HOST" "cd ~/$PI_DIR/build 2>/dev/null && sha256sum usb.img stage1.te2 regions.json 2>/dev/null || echo '(artifacts not deployed yet)'"
