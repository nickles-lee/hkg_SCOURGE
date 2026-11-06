#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 06_build_package.sh — build the post-root normal-USB update packages.
#
#   package_second  — stock USB wrapper; the six stubs live in the rootfs
#   package_wrapper — patched USB wrapper + the same rootfs
#
# WIP: this whole flow (custom firmware via the patched updater) is under
# active car validation — see docs/08-postroot-updates.md.
#
# The .lge.upgrade.xml stays stock. The .ver rootfs row 341174 is patched;
# the wrapper variant also patches row 341142. The .ver is padded to the
# stock 2,467 bytes (the 9-digit CRC shortens it by one).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"
need_venv

STOCK="$STOCK_PACKAGE"
ROOTFS="$OUT_DIR/stage1.te2"
WRAPPER="$OUT_DIR/AppUpgrade.patched.te2"

[ -d "$STOCK" ] || { echo "missing stock package: $STOCK (run 01_stage_inputs.sh)" >&2; exit 1; }
[ -f "$ROOTFS" ] || { echo "missing $ROOTFS (run 04_repack_rootfs.sh)" >&2; exit 1; }

pad_ver() {
    local f=$1
    while [ "$(stat -c%s "$f")" -lt "$VER_STOCK_SIZE" ]; do
        printf '\n' >> "$f"
    done
}

build_variant() {
    local variant=$1
    local out="$OUT_DIR/package_${variant}"
    echo "== build $variant"

    rm -rf "$out"
    mkdir -p "$out"
    # Use "$STOCK"/. so a symlinked stock path cannot turn $out into a symlink.
    cp -al "$STOCK"/. "$out"/

    echo "== replace rootfs"
    rm -f "$out/HU/images/mango-rootfs.tar.gz"
    cp "$ROOTFS" "$out/HU/images/mango-rootfs.tar.gz"

    echo "== patch .ver rootfs row $VER_ROOTFS_ROW_ID"
    rm -f "$out/$VER_FILE"
    "$PYTHON" "$SCOURGE_ROOT/tools/patch_ver.py" \
        "$INPUT_DIR/$VER_FILE" "$out/HU/images/mango-rootfs.tar.gz" "$out/$VER_FILE"

    local wrapper_note="stock"
    if [ "$variant" = "wrapper" ]; then
        [ -f "$WRAPPER" ] || { echo "missing $WRAPPER (run 07_build_patched_usb_appupgrade.sh)" >&2; exit 1; }
        echo "== replace AppUpgrade wrapper + patch row $VER_WRAPPER_ROW_ID"
        rm -f "$out/AppUpgrade"
        cp "$WRAPPER" "$out/AppUpgrade"
        rm -f "$out/$VER_FILE.tmp"
        "$PYTHON" "$SCOURGE_ROOT/tools/patch_ver_row_id.py" \
            "$out/$VER_FILE" "$out/$VER_FILE.tmp" \
            "$VER_WRAPPER_ROW_ID" "$out/AppUpgrade" "AppUpgrade"
        mv "$out/$VER_FILE.tmp" "$out/$VER_FILE"
        wrapper_note="patched"
    fi

    echo "== pad .ver to the stock $VER_STOCK_SIZE bytes"
    pad_ver "$out/$VER_FILE"

    cat > "$out/BUILD.txt" <<EOF
variant=scourge_stage1_${variant}
rootfs_container=HU/images/mango-rootfs.tar.gz
rootfs_size=$(stat -c%s "$out/HU/images/mango-rootfs.tar.gz")
rootfs_sha256=$(sha256sum "$out/HU/images/mango-rootfs.tar.gz" | cut -d' ' -f1)
wrapper=${wrapper_note}
wrapper_sha256=$(sha256sum "$out/AppUpgrade" | cut -d' ' -f1)
ver_size=$(stat -c%s "$out/$VER_FILE")
ver_sha256=$(sha256sum "$out/$VER_FILE" | cut -d' ' -f1)
message_sha256=$(sha256sum "$SCOURGE_ROOT/config/message.txt" | cut -d' ' -f1)
note=normal USB install; six stubs baked into the rootfs; .lge.upgrade.xml is stock
EOF

    echo "== MANIFEST.sha256"
    ( cd "$out" && find . -type f ! -name MANIFEST.sha256 -print0 | sort -z | \
      xargs -0 sha256sum > MANIFEST.sha256 )

    cat "$out/BUILD.txt"
    ls -la "$out/HU/images/mango-rootfs.tar.gz" "$out/$VER_FILE" "$out/AppUpgrade"
}

build_variant second
build_variant wrapper
