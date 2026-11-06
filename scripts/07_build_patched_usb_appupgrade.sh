#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 07_build_patched_usb_appupgrade.sh — build the patched USB AppUpgrade wrapper.
#
# The wrapper is the TE2 0xf5 container at the package root. The rootfs copy
# and the wrapper plaintext are byte-identical on 260128, so the same six-stub
# patch set applies. Re-encrypted at the same size/IV.
#
# CAVEAT: class 0xf5 is RSA-signed over IV||ciphertext. Re-encrypting changes
# the ciphertext, so the OEM signature will not validate — this is why the
# AppScomoDM button gate and the engine stubs exist (docs/08). The .ver
# wrapper row is patched. Bench-test before relying on it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"
load_te2_key
need_venv
need_fetched

SRC="$INPUT_DIR/AppUpgrade.usb"
PLAIN="$WORK_DIR/AppUpgrade.usb.plain"
PATCHED="$WORK_DIR/AppUpgrade.usb.patched"
OUT="$OUT_DIR/AppUpgrade.patched.te2"
VER="$OUT_DIR/$VER_FILE.AppUpgrade"

[ -f "$SRC" ] || { echo "missing $SRC (run 01_stage_inputs.sh)" >&2; exit 1; }
lock_check_or_die "$SRC" "$WRAPPER_STOCK_SHA256" "USB wrapper container"

if [ ! -f "$PLAIN" ]; then
    echo "== decrypt the wrapper container"
    mkdir -p "$WORK_DIR/verify_appupgrade"
    ln -sf "$(readlink -f "$SRC")" "$WORK_DIR/verify_appupgrade/AppUpgrade"
    "$PYTHON" "$TOCTOU_REPO/pizero/gen5w_decrypt.py" decrypt \
        "$WORK_DIR/verify_appupgrade/AppUpgrade" "$PLAIN" --force
fi

echo "== apply the four engine stubs to the wrapper plaintext"
"$PYTHON" "$SCOURGE_ROOT/payload/patch_validators.py" appupgrade "$PLAIN" "$PATCHED"

echo "== TE2 encrypt (basename AppUpgrade)"
rm -f "$OUT"
"$PYTHON" "$TOCTOU_REPO/pizero/te2_encrypt.py" "$PATCHED" "$SRC" "$OUT" "AppUpgrade"

echo "== inspect under basename AppUpgrade"
mkdir -p "$WORK_DIR/verify_appupgrade"
ln -sf "$(readlink -f "$OUT")" "$WORK_DIR/verify_appupgrade/AppUpgrade"
"$PYTHON" "$TOCTOU_REPO/pizero/gen5w_decrypt.py" inspect "$WORK_DIR/verify_appupgrade/AppUpgrade"

echo "== patch .ver row $VER_WRAPPER_ROW_ID (reference copy)"
rm -f "$VER"
"$PYTHON" "$SCOURGE_ROOT/tools/patch_ver_row_id.py" \
    "$INPUT_DIR/$VER_FILE" "$VER" "$VER_WRAPPER_ROW_ID" "$OUT" "AppUpgrade"

echo "== caveat"
echo "   class 0xf5 signature is invalid after re-encryption;"
echo "   the button gate + engine stubs compensate (docs/08)."
ls -la "$OUT" "$VER"
