#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Build the Mode C compat-kernel initramfs from the pinned host kernel modules.
# Nothing is downloaded: mkinitcpio uses /usr/lib/modules/$KVER. The image is
# local, ignored by git, and contains only transport/GPU helpers.
set -eu
cd "$(dirname "$0")/.."

KVER=${KVER:-$(uname -r)}
KERNEL=/usr/lib/modules/$KVER/vmlinuz
[ -f "$KERNEL" ] || {
    echo "host kernel not found: $KERNEL" >&2
    echo "set KVER to an installed kernel version" >&2
    exit 1
}
command -v mkinitcpio >/dev/null || {
    echo 'mkinitcpio is required' >&2
    exit 1
}
[ -f compat/kernel.lock.json ] || {
    echo 'no compat/kernel.lock.json; run: sh compat/pin-kernel.sh' >&2
    exit 1
}
python3 -m harness.compat verify-kernel --kver "$KVER" --skip-initramfs || {
    echo 'compat kernel pin mismatch; re-pin intentionally with: sh compat/pin-kernel.sh' >&2
    exit 1
}

OUT=${OUT:-initramfs-linux.img}
mkinitcpio -c "$(pwd)/compat/mkinitcpio.conf" -k "$KVER" -g "$(pwd)/compat/$OUT"
python3 -m harness.compat verify-kernel --kver "$KVER"
ls -lh "$(pwd)/compat/$OUT"
