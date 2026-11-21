#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 04_repack_rootfs.sh — repack the patched rootfs into a size/IV-identical
# TE2 envelope (deterministic: your build must reproduce the known-good
# hashes in docs/03-stage1-build.md).
#
#   work/stock-plain.tar.gz (genuine plaintext)
#     -> 03b patches /etc/smack_setup.sh + the permissive flag
#     -> surgery.py replaces AppUpgrade + AppScomoDM + AppDMClient +
#        AppLauncher + /etc/smack_setup.sh + every patched .qm file
#     -> tar --append the four failsafe entries + the SMACK flag
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
SMACK="$W/smack"
SMACK_ETC="$SMACK/etc"
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

echo "== patch /etc/smack_setup.sh + flag (build artifact in work/smack/)"
"$PYTHON" "$SCOURGE_ROOT/scripts/03b_patch_smack.py" --stock-tar "$STOCK" --out "$SMACK_ETC"

echo "== surgery: replace AppUpgrade + AppScomoDM + AppDMClient + AppLauncher + smack_setup + .qm"
QM_PAYLOAD=("$P"/translation_*.qm)
if [ ! -f "${QM_PAYLOAD[0]}" ]; then
    echo "ERROR: no patched .qm files in $P (run 03_patch_payload.py)" >&2
    exit 1
fi
qm_args=()
for qm in "${QM_PAYLOAD[@]}"; do
    base=$(basename "$qm")
    qm_args+=(--replace "./app/share/AppStandbyClock/localization/qm/$base=$qm")
done
"$PYTHON" "$SCOURGE_ROOT/tools/surgery.py" "$STOCK" "$REPLACED" \
  --replace ./app/bin/AppUpgrade="$P/AppUpgrade.patched" \
  --replace ./app/bin/AppScomoDM="$P/AppScomoDM.patched" \
  --replace ./app/bin/AppDMClient="$P/AppDMClient.patched" \
  --replace ./app/bin/AppLauncher="$P/AppLauncher.patched" \
  --replace ./etc/smack_setup.sh="$SMACK_ETC/smack_setup.sh" \
  "${qm_args[@]}"

echo "== decompress to tar"
zcat "$REPLACED" > "$TAR"

# Fixed mtimes for the appended payload entries: the TE2 hash is only
# deterministic if the tar headers are. Absolute epochs (not `touch -t`)
# keep the build identical on any host timezone; these match the
# car-validated stage-1 build-5 image.
touch -d @1793998494 "$SCOURGE_ROOT/payload/rootfs/etc/initial_root/run_stuff_loader.sh"
touch -d @1794152667 "$SCOURGE_ROOT/payload/rootfs/etc/initial_root/updater_guard.sh"
touch -d @1793997988 "$SCOURGE_ROOT/payload/rootfs/etc/systemd/system/initial-root-failsafe.service"
touch -h -d @1793997991 "$SCOURGE_ROOT/payload/rootfs/etc/systemd/system/multi-user.target.wants/initial-root-failsafe.service"
touch -d @1794317608 "$SMACK_ETC/smack_permissive"

echo "== append the failsafe entries (only new entries; parent dirs exist)"
tar --append -f "$TAR" --format=ustar --numeric-owner --owner=0 --group=0 \
    -C "$SCOURGE_ROOT/payload/rootfs" \
    ./etc/initial_root/run_stuff_loader.sh \
    ./etc/initial_root/updater_guard.sh \
    ./etc/systemd/system/initial-root-failsafe.service \
    ./etc/systemd/system/multi-user.target.wants/initial-root-failsafe.service
tar --append -f "$TAR" --format=ustar --numeric-owner --owner=0 --group=0 \
    -C "$SMACK" ./etc/smack_permissive

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
