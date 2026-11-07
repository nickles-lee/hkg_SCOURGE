#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Build a NEW local image; never overwrite shared backing-image links.
# Unprivileged: fakeroot makes the image's files root-owned without host root.
set -eu
cd "$(dirname "$0")"

ROOTFS=${ROOTFS:-../project/analysis/ioniq5_260128/work/rootfs}
OUT=
KERNEL_OUT=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --output) OUT=${2:?missing output}; shift 2 ;;
        --kernel-output) KERNEL_OUT=${2:?missing kernel output}; shift 2 ;;
        *) echo 'usage: sh build_rootfs.sh --output NEW.img --kernel-output NEW.bzImage' >&2; exit 1 ;;
    esac
done
[ -n "$OUT" ] && [ -n "$KERNEL_OUT" ] || {
    echo 'Explicit --output and --kernel-output required; --force is deliberately unsupported.' >&2
    exit 1
}

if [ ! -d "$ROOTFS" ]; then
    echo "rootfs not found: $ROOTFS" >&2
    echo "set ROOTFS=/path/to/decrypted/rootfs" >&2
    exit 1
fi
python3 -m harness.images reserve "$OUT" "$KERNEL_OUT"

cp "$ROOTFS/boot/bzImage-4.1.27-abl" "$KERNEL_OUT"
truncate -s 4G "$OUT"
fakeroot mkfs.ext4 -F -b 4096 -I 256 \
    -O none,has_journal,ext_attr,resize_inode,dir_index,filetype,extent,flex_bg,sparse_super,large_file,huge_file,dir_nlink,extra_isize \
    -E lazy_itable_init=0,lazy_journal_init=0,root_owner=0:0 \
    -d "$ROOTFS" "$OUT"
debugfs -w -R "write init /init" "$OUT" >/dev/null
debugfs -w -R "sif /init mode 0100755" "$OUT" >/dev/null

for pair in "guest/micom-dbus-sim /micom-dbus-sim" \
            "guest/usb-mount-signal /usb-mount-signal" \
            "guest/phase3.sh /phase3.sh"; do
    src=${pair%% *}
    dst=${pair##* }
    [ -f "$src" ] || continue
    debugfs -w -R "rm $dst" "$OUT" >/dev/null 2>&1 || true
    debugfs -w -R "write $src $dst" "$OUT" >/dev/null
    debugfs -w -R "sif $dst mode 0100755" "$OUT" >/dev/null
done

e2fsck -fn "$OUT"
sha256sum "$KERNEL_OUT" "$OUT"
