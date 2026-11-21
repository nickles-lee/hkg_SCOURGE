#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 23_usb_debug_link.sh — an ECM network function on the rig's USB-C, next to the
# drive, so the cable that serves the update also carries an ssh link.
#
#   scripts/23_usb_debug_link.sh --on [--address 10.55.0.1/24]
#   scripts/23_usb_debug_link.sh --off
#   scripts/23_usb_debug_link.sh --status
#
# Run ON the rig (root). This is ONE HALF of the link: it gives the head unit a
# `usb0` peer, but nothing on the head unit configures its end or listens on it
# until the Wifi1b package is installed (packages/wifi1b: usb0 at 10.55.0.2 plus
# key-only dropbear on 2224). Only then is
#
#   ssh -J pi@<rig> root@10.55.0.2 -p 2224
#
# a working hop from a laptop that shares the rig's WiFi: no NAT, no IP
# forwarding, the rig's own sshd carries the second connection.
#
# The toctou gadget owns the same UDC, so --on stops it first and --off leaves it
# stopped: re-arm with `systemctl start toctou-gadget`. Keeping the two apart is
# deliberate — during an update the head unit should see mass storage and nothing
# else, because every extra descriptor is a risk in the one phase that matters.
set -euo pipefail

GADGET=/sys/kernel/config/usb_gadget/dbg
IFACE=usb0
ADDRESS=10.55.0.1/24
ACTION=""

die() { echo "$*" >&2; exit 1; }

while [ "$#" -gt 0 ]; do
    case "$1" in
        --on|--off|--status) ACTION="${1#--}"; shift ;;
        --address) ADDRESS="${2:?--address needs a value}"; shift 2 ;;
        -h|--help) sed -n '4,8p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done
[ -n "$ACTION" ] || die "one of --on, --off, --status is required"

udc() {
    local found
    found="$(ls /sys/class/udc 2>/dev/null | head -n1)"
    [ -n "$found" ] || die "no UDC — dtoverlay=dwc2,dr_mode=peripheral is not active"
    echo "$found"
}

status() {
    echo "udc:          $(ls /sys/class/udc 2>/dev/null | tr '\n' ' ')"
    echo "dbg gadget:   $([ -d "$GADGET" ] && echo present || echo absent)"
    echo "bound to:     $([ -f "$GADGET/UDC" ] && cat "$GADGET/UDC" || echo '-')"
    echo "toctou:       $(systemctl is-active toctou-gadget 2>/dev/null || true)"
    if ip link show "$IFACE" >/dev/null 2>&1; then
        echo "address:      $(ip -4 -br addr show "$IFACE" | tr -s ' ')"
        echo "carrier:      $(cat "/sys/class/net/$IFACE/carrier" 2>/dev/null || echo '-')"
    else
        echo "address:      no $IFACE (the cable carries no network function)"
    fi
}

case "$ACTION" in
    status) status; exit 0 ;;
    off)
        if [ -d "$GADGET" ]; then
            [ -f "$GADGET/UDC" ] && echo "" > "$GADGET/UDC" || true
            rm -f "$GADGET/configs/c.1/ecm.usb0" 2>/dev/null || true
            rmdir "$GADGET"/functions/ecm.usb0 "$GADGET"/configs/c.1/strings/0x409 \
                  "$GADGET"/configs/c.1 "$GADGET"/strings/0x409 "$GADGET" 2>/dev/null || true
        fi
        ip addr flush dev "$IFACE" 2>/dev/null || true
        echo "debug link down; toctou-gadget left as it was ($(systemctl is-active toctou-gadget 2>/dev/null || true))"
        exit 0
        ;;
esac

systemctl stop toctou-gadget 2>/dev/null || true
modprobe libcomposite 2>/dev/null || true
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
mkdir -p "$GADGET"
cd "$GADGET"

echo 0x1d6b > idVendor
echo 0x0104 > idProduct
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB
mkdir -p strings/0x409
printf 'SCOURGE\n' > strings/0x409/manufacturer
printf 'rig debug link\n' > strings/0x409/product
cat /proc/sys/kernel/random/uuid > strings/0x409/serialnumber

mkdir -p configs/c.1/strings/0x409
printf 'ecm\n' > configs/c.1/strings/0x409/configuration
mkdir -p functions/ecm.usb0
[ -e configs/c.1/ecm.usb0 ] || ln -sf functions/ecm.usb0 configs/c.1/

echo "$(udc)" > UDC
ip link set "$IFACE" up
ip addr replace "$ADDRESS" dev "$IFACE"
status
echo
echo "The head unit still needs its end of the link (packages/wifi1b: usb0 + dropbear 2224)."
