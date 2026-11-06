#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 09_preflight.sh — offline go/no-go gate before any deployment.
#
#   A. stage-1 rootfs envelope: size/IV/integrity/decrypt/tar/message/six stubs
#   B. package_second:  stock USB wrapper, stubs in the rootfs
#   C. package_wrapper: patched USB wrapper, stubs in both
#
# Plus the binary-sanity gate: every ELF the packages ship must be
# statically linked (except tweaks_persist, which is vendor-dynamic by
# design — see docs/07-packages.md, toolchain policy).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"
load_te2_key
need_venv

echo "########## A. stage-1 rootfs"
"$PYTHON" "$HERE/05_verify_rootfs.py" --container "$OUT_DIR/stage1.te2"

if [ -d "$OUT_DIR/package_second" ]; then
    echo
    echo "########## B. package_second (stock wrapper)"
    "$PYTHON" "$HERE/verify_package.py" \
        --variant second \
        --package "$OUT_DIR/package_second" \
        --stock "$STOCK_PACKAGE" \
        --container "$OUT_DIR/stage1.te2" \
        --ver "$OUT_DIR/package_second/$VER_FILE"
fi

if [ -d "$OUT_DIR/package_wrapper" ]; then
    echo
    echo "########## C. package_wrapper (patched wrapper)"
    "$PYTHON" "$HERE/verify_package.py" \
        --variant wrapper \
        --package "$OUT_DIR/package_wrapper" \
        --stock "$STOCK_PACKAGE" \
        --container "$OUT_DIR/stage1.te2" \
        --ver "$OUT_DIR/package_wrapper/$VER_FILE"
fi

echo
echo "PREFLIGHT: OK"
