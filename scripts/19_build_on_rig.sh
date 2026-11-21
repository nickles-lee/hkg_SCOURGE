#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 19_build_on_rig.sh — build the stage-1 rootfs envelope ON the rig.
#
#   scripts/19_build_on_rig.sh [--dry-run] [--install-deps]
#
# Why: the 27 GB usb.img is 100% genuine stock firmware and never changes. What
# changes per iteration is out/stage1.te2 (~880 MB) and, at most, regions.json.
# So the payload patch -> repack -> verify chain can run on the board itself and
# only the ~880 MB envelope has to exist there — no 27 GB copy over the WiFi.
#
# This needs ~3 GB free on the rig (input ~880 MB + work/stock-plain ~880 MB +
# out/stage1.te2 ~880 MB) and a Python venv with pycryptodome/deflate/
# cryptography. It does NOT build usb.img: that needs 27 GB and stays a PC job.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

DRY=0
DEPS=0
WITH_STOCK=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) DRY=1; shift ;;
        --install-deps) DEPS=1; shift ;;
        --with-stock) WITH_STOCK=1; shift ;;
        -h|--help) sed -n '4,20p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ -n "$PI_HOST" ] || { echo "set PI_HOST/PI_USER in config/paths.local.env" >&2; exit 1; }
[ -f "$INPUT_DIR/mango-rootfs.tar.gz" ] || {
    echo "missing $INPUT_DIR/mango-rootfs.tar.gz — run 01_stage_inputs.sh first" >&2
    exit 1
}
[ -d "$TOCTOU_REPO/pizero" ] || {
    echo "missing $TOCTOU_REPO — run 00_fetch_third_party.sh first" >&2
    exit 1
}

REMOTE="cd ~/$PI_DIR"
NEED_MB=$((3 * 1024))

echo "== preflight"
FREE_MB="$(ssh "$PI_USER@$PI_HOST" 'df -m --output=avail ~ | tail -n1 | tr -d " "' || true)"
case "$FREE_MB" in ''|*[!0-9]*) echo "could not read free space on the rig" >&2; exit 1 ;; esac
echo "  rig free space: ${FREE_MB} MB (need ~${NEED_MB} MB)"
[ "$FREE_MB" -ge "$NEED_MB" ] || {
    echo "not enough space on the rig: ${FREE_MB} MB < ${NEED_MB} MB." >&2
    echo "Point build/ at an SSD, or keep the build on this host." >&2
    exit 1
}
ssh "$PI_USER@$PI_HOST" 'command -v python3 >/dev/null' || {
    echo "python3 missing on the rig" >&2; exit 1
}
if ssh "$PI_USER@$PI_HOST" '[ -x ~/scourge/.venv/bin/python ]'; then
    echo "  rig venv: present"
else
    echo "  rig venv: missing (will be created)"
    [ "$DEPS" -eq 1 ] || echo "  (pass --install-deps to install it automatically)"
fi

echo "== copying what the chain needs"
echo "  input/ scripts/ payload/ config/ third_party/ioniq5-usb-toctou -> $PI_USER@$PI_HOST:~/scourge"
if [ "$DRY" -eq 1 ]; then
    echo "== dry run: would then run on the rig"
    echo "  $REMOTE && .venv/bin/python scripts/03_patch_payload.py --stock-tar work/stock-plain.tar.gz --messages config/messages.json --out work/payload"
    echo "  $REMOTE && bash scripts/04_repack_rootfs.sh"
    echo "  $REMOTE && .venv/bin/python scripts/05_verify_rootfs.py --container out/stage1.te2"
    echo "  install out/stage1.te2 (+ regions.json if present) into build/"
    exit 0
fi

rsync -a --relative -e 'ssh -o BatchMode=yes' \
    input scripts payload config third_party/ioniq5-usb-toctou \
    "$PI_USER@$PI_HOST:~/scourge/"

if [ "$DEPS" -eq 1 ] && ! ssh "$PI_USER@$PI_HOST" '[ -x ~/scourge/.venv/bin/python ]'; then
    echo "== creating the venv on the rig"
    ssh "$PI_USER@$PI_HOST" "$REMOTE && \
        { python3 -m venv .venv || { sudo apt-get install -y python3-venv && python3 -m venv .venv; }; } && \
        .venv/bin/pip install --quiet --upgrade pip && \
        .venv/bin/pip install --quiet pycryptodome deflate cryptography"
fi
ssh "$PI_USER@$PI_HOST" '[ -x ~/scourge/.venv/bin/python ]' || {
    echo "no venv on the rig — rerun with --install-deps" >&2; exit 1
}
ssh "$PI_USER@$PI_HOST" "$REMOTE && .venv/bin/python -c 'import Crypto, deflate, cryptography'" || {
    echo "the rig venv lacks pycryptodome/deflate/cryptography" >&2; exit 1
}

echo "== the decrypted stock rootfs has to exist on the rig"
if ! ssh "$PI_USER@$PI_HOST" '[ -f ~/scourge/work/stock-plain.tar.gz ]'; then
    if [ "$WITH_STOCK" -eq 1 ]; then
        echo "  copying work/stock-plain.tar.gz (~880 MB, one time — ~16 min on the rig's WiFi)"
        ssh "$PI_USER@$PI_HOST" "mkdir -p ~/scourge/work"
        rsync -a --progress -e 'ssh -o BatchMode=yes' \
            work/stock-plain.tar.gz "$PI_USER@$PI_HOST:~/scourge/work/"
    else
        echo "missing ~/scourge/work/stock-plain.tar.gz on the rig." >&2
        echo "  Either rerun with --with-stock (copies it once, ~880 MB), or run" >&2
        echo "  02_decrypt_rootfs.sh on the board with config/key.local in place." >&2
        exit 1
    fi
fi

echo "== patch payload"
ssh "$PI_USER@$PI_HOST" "$REMOTE && .venv/bin/python scripts/03_patch_payload.py \
    --stock-tar work/stock-plain.tar.gz --messages config/messages.json --out work/payload"
echo "== repack rootfs"
ssh "$PI_USER@$PI_HOST" "$REMOTE && bash scripts/04_repack_rootfs.sh"
echo "== verify"
ssh "$PI_USER@$PI_HOST" "$REMOTE && .venv/bin/python scripts/05_verify_rootfs.py \
    --container out/stage1.te2"

echo "== publishing to build/"
ssh "$PI_USER@$PI_HOST" "$REMOTE && mkdir -p build && install -m 0644 out/stage1.te2 build/ \
    && { [ -f out/regions.json ] && install -m 0644 out/regions.json build/ || \
         echo 'no out/regions.json on the rig — copy it from the PC once (it does not change per patch)'; } \
    && ls -l build"
echo
echo "stage-1 built on the rig. usb.img stays the PC's job — it is stock firmware and does not change."
