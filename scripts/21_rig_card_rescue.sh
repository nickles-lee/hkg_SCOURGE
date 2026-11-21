#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 21_rig_card_rescue.sh — take the two auto-start switches off a rig's SD card
# while it is in this host's reader, for a board that no longer reaches the
# network.
#
#   sudo bash scripts/21_rig_card_rescue.sh /dev/sdb2          # report only
#   sudo bash scripts/21_rig_card_rescue.sh /dev/sdb2 --fix    # remove them
#
# Power the board off and pull the card first. Two things can keep a headless rig
# off the network after a reboot:
#   - toctou-gadget.service enabled at boot: it binds the UDC a few seconds in,
#     and on a Pi 4/5 the USB-C port is also the power input (docs/04).
#   - a LABEL=<stick> fstab line whose device is not there. `nofail` should cover
#     it, and it has, but a stuck mount is not worth the doubt while recovering.
set -euo pipefail

DEV="${1:-}"
[ -n "$DEV" ] || { echo "usage: $0 /dev/sdXN [--fix]" >&2; exit 2; }
[ "${2:-}" = "--fix" ] && FIX=1 || FIX=0
[ -b "$DEV" ] || { echo "not a block device: $DEV" >&2; exit 2; }
findmnt -S "$DEV" -o TARGET -n | grep -q . && { echo "$DEV is mounted — unmount it first" >&2; exit 2; }
[ "$(id -u)" -eq 0 ] || { echo "must run as root (sudo)" >&2; exit 2; }

MNT="$(mktemp -d)"
trap 'umount "$MNT" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true' EXIT

mount -o ro "$DEV" "$MNT"
[ -d "$MNT/etc/systemd/system" ] || { echo "$DEV does not look like a rootfs" >&2; exit 2; }

UNIT_LINK="$MNT/etc/systemd/system/multi-user.target.wants/toctou-gadget.service"
FSTAB="$MNT/etc/fstab"
found=0
if [ -L "$UNIT_LINK" ] || [ -e "$UNIT_LINK" ]; then
    echo "found: toctou-gadget.service enabled at boot -> $(readlink "$UNIT_LINK" 2>/dev/null || echo "$UNIT_LINK")"
    found=1
else
    echo "ok:    toctou-gadget.service is not enabled at boot"
fi
if grep -nE '^[[:space:]]*LABEL=[^[:space:]]+[[:space:]]+/media/' "$FSTAB" >/dev/null 2>&1; then
    echo "found: stick mount line in /etc/fstab:"
    grep -nE '^[[:space:]]*LABEL=[^[:space:]]+[[:space:]]+/media/' "$FSTAB" | sed 's/^/       /'
    found=1
else
    echo "ok:    no stick mount line in /etc/fstab"
fi

if [ "$found" -eq 0 ]; then
    echo "nothing to undo."
    exit 0
fi
if [ "$FIX" -eq 0 ]; then
    echo
    echo "re-run with --fix to remove them (a backup of /etc/fstab is kept as /etc/fstab.bak-rescue)."
    exit 0
fi

mount -o remount,rw "$DEV" "$MNT"
if [ -e "$UNIT_LINK" ] || [ -L "$UNIT_LINK" ]; then
    rm -f "$UNIT_LINK"
    echo "removed: toctou-gadget.service autostart"
fi
if grep -qE '^[[:space:]]*LABEL=[^[:space:]]+[[:space:]]+/media/' "$FSTAB"; then
    cp -a "$FSTAB" "$FSTAB.bak-rescue"
    sed -i '/^[[:space:]]*LABEL=[^[:space:]]\+[[:space:]]\+\/media\//d' "$FSTAB"
    echo "removed: the stick mount line (backup: /etc/fstab.bak-rescue)"
fi
sync
echo
echo "done. Put the card back, power the board up, and verify with:"
echo "  systemctl is-enabled toctou-gadget.service   # should say disabled"
echo "  arm it by hand when the wiring allows: sudo systemctl start toctou-gadget"
