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
# The UDC name is board-specific (Pi Zero 2 W: 3f980000.usb, Pi 4/5: fe980000.usb), so
# resolve it from sysfs instead of hardcoding one board's address.
ssh "$PI_USER@$PI_HOST" 'udc=$(ls /sys/class/udc 2>/dev/null | head -n1); \
    [ -n "$udc" ] || { echo "no UDC — dwc2 overlay missing?"; exit 0; }; \
    echo "UDC=$udc"; readlink "/sys/bus/platform/devices/$udc/driver" || true'
echo "== watchdog"
ssh "$PI_USER@$PI_HOST" 'systemctl show -p RuntimeWatchdogUSec; dmesg | grep -i "hardware watchdog" || echo clean'
echo "== packages"
# /usr/sbin is not on a non-login ssh PATH, so search it explicitly or the
# check reports a false negative for nbd-client and mkfs.exfat.
ssh "$PI_USER@$PI_HOST" 'PATH=/usr/sbin:/sbin:$PATH; command -v nbdkit nbd-client mkfs.exfat || true'
echo "== artifacts"
ssh "$PI_USER@$PI_HOST" "cd ~/$PI_DIR/build 2>/dev/null && sha256sum usb.img stage1.te2 regions.json 2>/dev/null || echo '(artifacts not deployed yet)'"
