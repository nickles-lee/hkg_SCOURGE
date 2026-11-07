#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Build a new tools image without overwriting the existing tools.img.
set -eu
cd "$(dirname "$0")"
OUT=${1:?usage: sh build_tools.sh NEW-tools.img}
python3 -m harness.images reserve "$OUT"
sh build_research_tools.sh
python3 -m harness.tools manifest --root toolsroot
if [ -n "${TOOLS_IMAGE_SIZE:-}" ]; then
    SIZE=$TOOLS_IMAGE_SIZE
else
    # Size from the actual tree: the Mesa/LLVM fixtures plus update.tar.gz
    # exceed the old fixed 200M image. Keep the historical 200M floor.
    BYTES=$(du -sb toolsroot | cut -f1)
    SIZE=$((BYTES + BYTES / 4 + 32 * 1024 * 1024))
    FLOOR=$((200 * 1024 * 1024))
    [ "$SIZE" -lt "$FLOOR" ] && SIZE=$FLOOR
fi
truncate -s "$SIZE" "$OUT"
fakeroot mkfs.ext4 -F -b 4096 -I 256 \
    -O none,has_journal,ext_attr,resize_inode,dir_index,filetype,extent,flex_bg,sparse_super,large_file,huge_file,dir_nlink,extra_isize \
    -E lazy_itable_init=0,lazy_journal_init=0,root_owner=0:0 \
    -d toolsroot "$OUT"
e2fsck -fn "$OUT"
python3 -m harness.tools check --image "$OUT" --root toolsroot --quiet
echo 'tools manifest: fresh'
for helper in applauncher-stub appmanager-stub automount-stub credman-stub entropy-seed; do
    built=$(sha256sum "build/helpers/$helper" | cut -d' ' -f1)
    embedded=$(debugfs -R "cat /$helper" "$OUT" 2>/dev/null | sha256sum | cut -d' ' -f1)
    [ "$built" = "$embedded" ] || {
        echo "ERROR: image helper /$helper does not match build/helpers/$helper" >&2
        exit 1
    }
    echo "helper verified: $helper $built"
done
sha256sum "$OUT"
