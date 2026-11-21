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

echo "== wifi fallback (setup AP + its setup page)"
# Optional: needs RIG_AP_SSID / RIG_AP_PASSPHRASE (config/rig.local.env or the
# environment). Without them the rig simply has no fallback AP yet.
HERE="$(cd "$(dirname "$0")" && pwd)"
if systemctl is-enabled --quiet rig-ap-fallback.service 2>/dev/null; then
    echo "(already installed — nothing to do)"
elif [ -f "$HERE/17_provision_wifi_fallback.sh" ]; then
    bash "$HERE/17_provision_wifi_fallback.sh" ||
        echo "(wifi fallback skipped — set RIG_AP_SSID and RIG_AP_PASSPHRASE)"
else
    echo "(no 17_provision_wifi_fallback.sh next to this script — skipped)"
fi

echo "== image stick (fstab, by label)"
# The 27 GB image can live on a stick instead of the SD card (docs/04). A headless
# image runs no automounter, and a serve.img symlink that dangles after a reboot
# turns a car-side update into a mystery, so mount it at boot: nofail (a rig
# without the stick still boots), ro (serving only ever reads).
STICK_LABEL="${RIG_IMAGE_STICK_LABEL:-RIGIMG}"
FSTAB=/etc/fstab
if grep -q "^LABEL=$STICK_LABEL " "$FSTAB"; then
    echo "(fstab already mounts LABEL=$STICK_LABEL)"
else
    install -d "/media/$STICK_LABEL"
    printf 'LABEL=%s /media/%s exfat ro,nofail,noatime,uid=1000,gid=1000,x-systemd.device-timeout=10 0 0\n' \
        "$STICK_LABEL" "$STICK_LABEL" >> "$FSTAB"
    echo "added: LABEL=$STICK_LABEL -> /media/$STICK_LABEL (ro, nofail)"
fi
systemctl daemon-reload

echo
echo "REBOOT REQUIRED. After reboot verify (scripts/12_verify_pi.sh):"
echo "  ls /sys/class/udc                        # Zero 2 W: 3f980000.usb, Pi 4/5: fe980000.usb"
echo "  readlink /sys/bus/platform/devices/\$(ls /sys/class/udc|head -n1)/driver   # .../dwc2"
echo "  systemctl show -p RuntimeWatchdogUSec              # =0"
echo "  dmesg | grep -i 'hardware watchdog' || echo clean"
