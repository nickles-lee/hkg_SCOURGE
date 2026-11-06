#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 10_provision_pi.sh — provision the Raspberry Pi Zero 2 W gadget rig.
# Run ON the Pi as root:  sudo bash 10_provision_pi.sh
#
# Installs the gadget dependencies, disables the hardware watchdog, makes
# the journal persistent, and switches the dwc2 controller to peripheral
# mode. A reboot is required afterwards — see docs/04-pi-rig.md.
set -euo pipefail

echo "== apt packages"
apt update
apt install -y nbdkit nbdkit-plugin-python nbd-client exfatprogs e2fsprogs fake-hwclock

SYS="$(dirname "$(readlink -f "$0")")/../third_party/ioniq5-usb-toctou/pizero/system"
[ -d "$SYS" ] || SYS="./pizero/system"
[ -d "$SYS" ] || { echo "missing $SYS — run 00_fetch_third_party.sh or copy the pizero/ tree here" >&2; exit 1; }

echo "== watchdog + journald drop-ins"
install -D "$SYS/99-no-watchdog.conf" /etc/systemd/system.conf.d/99-no-watchdog.conf
install -D "$SYS/99-toctou-persistent.conf" /etc/systemd/journald.conf.d/99-toctou-persistent.conf

echo "== dwc2 peripheral under [all]"
CFG=/boot/firmware/config.txt
[ -f "$CFG" ] || CFG=/boot/config.txt
if ! grep -q '^dtoverlay=dwc2,dr_mode=peripheral' "$CFG"; then
    printf '\n[all]\ndtoverlay=dwc2,dr_mode=peripheral\n' >> "$CFG"
fi

echo "== fake-hwclock"
systemctl enable fake-hwclock 2>/dev/null || true

echo
echo "REBOOT REQUIRED. After reboot verify (scripts/12_verify_pi.sh):"
echo "  ls /sys/class/udc                                  # 3f980000.usb"
echo "  readlink /sys/bus/platform/devices/3f980000.usb/driver   # .../dwc2"
echo "  systemctl show -p RuntimeWatchdogUSec              # =0"
echo "  dmesg | grep -i 'hardware watchdog' || echo clean"
