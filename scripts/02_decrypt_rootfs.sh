#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 02_decrypt_rootfs.sh — decrypt the genuine rootfs container to plaintext.
#
#   input/mango-rootfs.tar.gz  ->  work/stock-plain.tar.gz
#
# The TE2 IV is derived from the basename, so the input must be staged under
# its HU-visible name (01_stage_inputs.sh guarantees that). The content key
# comes from config/key.local / $TE2_CONTENT_KEY — never hardcoded.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"
load_te2_key
need_venv
need_fetched

SRC="$INPUT_DIR/mango-rootfs.tar.gz"
OUT="$WORK_DIR/stock-plain.tar.gz"

[ -f "$SRC" ] || { echo "missing $SRC (run 01_stage_inputs.sh)" >&2; exit 1; }
lock_check_or_die "$SRC" "" "rootfs" || true
[ "$(md5sum "$SRC" | cut -d' ' -f1)" = "$GENUINE_MD5" ] || {
    echo "ERROR: rootfs md5 mismatch (want $GENUINE_MD5)" >&2; exit 1; }

mkdir -p "$WORK_DIR"
if [ -f "$OUT" ]; then
    echo "already exists: $OUT (delete it to re-decrypt)"
else
    "$PYTHON" "$TOCTOU_REPO/pizero/gen5w_decrypt.py" decrypt "$SRC" "$OUT" --force
fi

echo "== gates"
SZ="$(stat -c%s "$OUT")"
[ "$SZ" = "$GENUINE_BODY_SIZE" ] || { echo "ERROR: plaintext size $SZ != $GENUINE_BODY_SIZE" >&2; exit 1; }
gzip -t "$OUT"
sha256sum "$OUT"
ls -la "$OUT"
echo "DECRYPT: OK"
