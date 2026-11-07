#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Build a USB image (ext4) from an update tree such as usb_build/K.
# usage: make_update_usb.sh <source-dir> <out.img> [size]
set -eu
cd "$(dirname "$0")"

SRC=${1:?source directory, e.g. ../usb_build/K}
OUT=${2:?output image, e.g. update-k.img}
SIZE=${3:-2G}

if [ ! -d "$SRC" ]; then
    echo "source not found: $SRC" >&2
    exit 1
fi
python3 -m harness.images reserve "$OUT"

truncate -s "$SIZE" "$OUT"
fakeroot mkfs.ext4 -F -b 4096 -I 256 \
    -O none,has_journal,ext_attr,resize_inode,dir_index,filetype,extent,flex_bg,sparse_super,large_file,huge_file,dir_nlink,extra_isize \
    -E lazy_itable_init=0,lazy_journal_init=0,root_owner=0:0 \
    -d "$SRC" "$OUT"

# Convenience copy of the current prelude for isolated script-stage tests.
if [ -f ../usb_build/tools/scripts/update_start_5w.sh ]; then
    debugfs -w -R "write ../usb_build/tools/scripts/update_start_5w.sh /poc5w_prelude.sh" "$OUT" >/dev/null
    debugfs -w -R "sif /poc5w_prelude.sh mode 0100755" "$OUT" >/dev/null
fi
e2fsck -fn "$OUT"
sha256sum "$OUT"
