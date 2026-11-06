#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 04_repack_rootfs.sh — repack the patched rootfs into a size/IV-identical
# TE2 envelope (deterministic: your build must reproduce the known-good
# hashes in docs/03-stage1-build.md).
#
#   work/stock-plain.tar.gz (genuine plaintext)
#     -> surgery.py replaces AppUpgrade + AppScomoDM + the two .qm files
#     -> tar --append the four failsafe entries from payload/rootfs/
#     -> libdeflate L10 (concatenated gzip members)
#     -> truncate to exactly GENUINE_BODY_SIZE
#     -> TE2-encrypt with the genuine envelope as the metadata template
#     -> out/stage1.te2
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"
load_te2_key
need_venv
need_fetched

W="$WORK_DIR"
P="$W/payload"
mkdir -p "$W" "$OUT_DIR"

STOCK="$W/stock-plain.tar.gz"
REPLACED="$W/replaced.tar.gz"
TAR="$W/stage1.tar"
GZ="$W/stage1.tar.gz"
PADDED="$W/stage1.padded.tar.gz"
OUT="$OUT_DIR/stage1.te2"
GENUINE="$INPUT_DIR/mango-rootfs.tar.gz"

[ -f "$STOCK" ] || { echo "missing $STOCK (run 02_decrypt_rootfs.sh)" >&2; exit 1; }
[ -f "$GENUINE" ] || { echo "missing $GENUINE (run 01_stage_inputs.sh)" >&2; exit 1; }
[ -f "$P/AppUpgrade.patched" ] || { echo "missing patched payload (run 03_patch_payload.py)" >&2; exit 1; }
[ -f "$P/AppScomoDM.patched" ] || { echo "missing patched AppScomoDM (run 03_patch_payload.py)" >&2; exit 1; }
[ -f "$P/AppDMClient.patched" ] || { echo "missing patched AppDMClient (run 03_patch_payload.py)" >&2; exit 1; }
[ -f "$P/AppLauncher.patched" ] || { echo "missing patched AppLauncher (run 03_patch_payload.py)" >&2; exit 1; }

echo "== surgery: replace AppUpgrade + AppScomoDM + AppDMClient + AppLauncher + .qm"
"$PYTHON" "$SCOURGE_ROOT/tools/surgery.py" "$STOCK" "$REPLACED" \
  --replace ./app/bin/AppUpgrade="$P/AppUpgrade.patched" \
  --replace ./app/bin/AppScomoDM="$P/AppScomoDM.patched" \
  --replace ./app/bin/AppDMClient="$P/AppDMClient.patched" \
  --replace ./app/bin/AppLauncher="$P/AppLauncher.patched" \
  --replace ./app/share/AppStandbyClock/localization/qm/translation_en-gb.qm="$P/translation_en-gb.qm" \
  --replace ./app/share/AppStandbyClock/localization/qm/translation_en-au.qm="$P/translation_en-au.qm"

echo "== decompress to tar"
zcat "$REPLACED" > "$TAR"

echo "== append the failsafe entries (only new entries; parent dirs exist)"
tar --append -f "$TAR" --format=ustar --numeric-owner --owner=0 --group=0 \
    -C "$SCOURGE_ROOT/payload/rootfs" \
    ./etc/initial_root/run_stuff_loader.sh \
    ./etc/initial_root/updater_guard.sh \
    ./etc/systemd/system/initial-root-failsafe.service \
    ./etc/systemd/system/multi-user.target.wants/initial-root-failsafe.service

echo "== libdeflate L10"
"$PYTHON" "$TOCTOU_REPO/pizero/chunkgz.py" "$TAR" "$GZ" 10 67108864

SZ=$(stat -c%s "$GZ")
echo "compressed=$SZ ceiling=$GENUINE_BODY_SIZE"
if [ "$SZ" -gt "$GENUINE_BODY_SIZE" ]; then
    echo "ERROR: over the ceiling by $((SZ - GENUINE_BODY_SIZE)) bytes" >&2
    exit 1
fi

echo "== pad to exactly $GENUINE_BODY_SIZE"
cp "$GZ" "$PADDED"
truncate -s "$GENUINE_BODY_SIZE" "$PADDED"

echo "== TE2 encrypt (basename $GENUINE_BASENAME — the IV depends on it)"
rm -f "$OUT"
"$PYTHON" "$TOCTOU_REPO/pizero/te2_encrypt.py" \
    "$PADDED" "$GENUINE" "$OUT" "$GENUINE_BASENAME"

echo "== inspect under the HU-visible basename"
mkdir -p "$W/verify_name"
ln -sf "$(readlink -f "$OUT")" "$W/verify_name/$GENUINE_BASENAME"
"$PYTHON" "$TOCTOU_REPO/pizero/gen5w_decrypt.py" inspect "$W/verify_name/$GENUINE_BASENAME"

echo "== sha256 (compare against docs/03-stage1-build.md)"
sha256sum "$OUT" "$PADDED"
ls -la "$OUT"
