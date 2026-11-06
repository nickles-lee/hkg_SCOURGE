#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# build_all.sh — full stage-1 build: patched rootfs -> verify -> packages ->
# vanilla TOCTOU image -> preflight.
#
# Requires: 00_fetch_third_party.sh, 00_env.sh and 01_stage_inputs.sh done
# (nudl download or user-provided package staged + gated).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

echo "########## 1. patch the payload binaries"
"$PYTHON" "$HERE/03_patch_payload.py" \
    --stock-tar "$WORK_DIR/stock-plain.tar.gz" \
    --message "$SCOURGE_ROOT/config/message.txt" \
    --out "$WORK_DIR/payload"

echo "########## 2. repack the rootfs"
bash "$HERE/04_repack_rootfs.sh"

echo "########## 3. verify the rootfs"
load_te2_key
"$PYTHON" "$HERE/05_verify_rootfs.py" --container "$OUT_DIR/stage1.te2"

echo "########## 4. patched USB wrapper (optional variant)"
bash "$HERE/07_build_patched_usb_appupgrade.sh"

echo "########## 5. normal-USB packages"
bash "$HERE/06_build_package.sh"

echo "########## 6. vanilla TOCTOU image + regions (needs sudo/loop)"
bash "$HERE/08_build_usb_image.sh"

echo "########## 7. preflight"
bash "$HERE/09_preflight.sh"

echo
echo "Compare every artifact against the known-good hash table in"
echo "docs/03-stage1-build.md. Any mismatch: stop and investigate."
sha256sum "$OUT_DIR/stage1.te2" "$WORK_DIR/stage1.padded.tar.gz" \
    "$OUT_DIR/usb.img" "$OUT_DIR/regions.json" 2>/dev/null
