#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 00_toctou.sh — one entry point for the whole rig preparation.
#
#   scripts/00_toctou.sh doctor            # what is missing, and the command that fixes it
#   scripts/00_toctou.sh prep              # venv -> deps -> inputs -> decrypt -> image
#   scripts/00_toctou.sh prep --from-dir DIR   # stage a package you already have
#   scripts/00_toctou.sh rig               # provision + verify the board over ssh
#   scripts/00_toctou.sh rig --agent       # also deploy/refresh the control page
#   scripts/00_toctou.sh sd /dev/sdb       # flash the card and install the first boot
#
# It is a thin orchestrator: every step is the numbered script it always was, so
# a failure stops the same place it always did. Steps whose outputs already
# exist are skipped unless --force; the 27 GB image is only built when missing
# (it is stock firmware and never changes).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

cd "$SCOURGE_ROOT"    # every relative path below (PYTHON, NUDL_BIN, OUT_DIR) is repo-rooted

FORCE=0
FROM_DIR=""
WITH_AGENT=0
VERB="${1:-}"
shift || true
case "$VERB" in
    doctor|prep|rig|sd) ;;
    -h|--help|"") sed -n '4,16p' "$0"; [ -n "$VERB" ] && exit 0 || exit 2 ;;
    *) echo "unknown verb: $VERB (doctor|prep|rig|sd)" >&2; exit 2 ;;
esac

SD_DEV=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --force) FORCE=1; shift ;;
        --from-dir) FROM_DIR="${2:?--from-dir needs a path}"; shift 2 ;;
        --agent) WITH_AGENT=1; shift ;;
        /dev/*) SD_DEV="$1"; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

step() { echo; echo "########## $*"; }
have() { [ -e "$1" ]; }
rig() { ssh -o BatchMode=yes -o ConnectTimeout=6 "$PI_USER@$PI_HOST" "$@" 2>/dev/null; }

check() {  # check <label> <ok?> <fix>
    if [ "${2:-0}" = "1" ]; then printf '  \033[32mok\033[0m   %s\n' "$1"
    else printf '  \033[33mTODO\033[0m %s\n        %s\n' "$1" "$3"; fi
}

doctor() {
    echo "== host"
    local venv=0 deps=0 nudl=0 key=0 float=0 pizero=0
    [ -x "$PYTHON" ] && venv=1
    [ -x "$PYTHON" ] && "$PYTHON" -c 'import Crypto, deflate, cryptography' 2>/dev/null && deps=1
    [ -x "$NUDL_BIN" ] && nudl=1
    [ -f "$SCOURGE_ROOT/config/key.local" ] && key=1
    [ -d "$TOCTOU_REPO/pizero" ] && pizero=1
    check "python venv ($PYTHON)" "$venv" "scripts/00_env.sh"
    check "build deps (pycryptodome, deflate, cryptography)" "$deps" "scripts/00_env.sh"
    check "nudl downloader (lock-pinned release)" "$nudl" "scripts/00_fetch_third_party.sh --only nudl"
    check "TE2 content key (config/key.local)" "$key" "scripts/00_fetch_third_party.sh --only key"
    check "pinned upstream tools ($TOCTOU_REPO)" "$pizero" "scripts/00_fetch_third_party.sh"
    echo "== inputs and artifacts"
    check "official package ($INPUT_DIR/mango-rootfs.tar.gz)" \
        "$(have "$INPUT_DIR/mango-rootfs.tar.gz" && echo 1 || echo 0)" \
        "scripts/01_stage_inputs.sh            # or: 00_toctou.sh prep --from-dir DIR"
    check "decrypted rootfs (work/stock-plain.tar.gz)" \
        "$(have "$WORK_DIR/stock-plain.tar.gz" && echo 1 || echo 0)" "scripts/02_decrypt_rootfs.sh"
    check "patched envelope (out/stage1.te2)" \
        "$(have "$OUT_DIR/stage1.te2" && echo 1 || echo 0)" "scripts/build_all.sh"
    check "USB image (out/usb.img, 27 GB, stock)" \
        "$(have "$OUT_DIR/usb.img" && echo 1 || echo 0)" "scripts/build_all.sh  (once: it never changes)"
    echo "== rig"
    if [ -z "$PI_HOST" ]; then
        check "PI_HOST configured" 0 "set PI_HOST/PI_USER in config/paths.local.env"
    elif udc="$(rig 'ls /sys/class/udc')" && [ -n "$udc" ]; then
        check "rig reachable with a UDC ($udc)" 1 ""
        check "hardware watchdog off" \
            "$(rig 'systemctl show -p RuntimeWatchdogUSec --value' | grep -qx '0' && echo 1 || echo 0)" \
            "scripts/10_provision_pi.sh (on the board) + reboot"
    else
        check "rig reachable ($PI_USER@$PI_HOST)" 0 "power it, or fix PI_HOST"
    fi
}

prep() {
    step "0. venv"
    have "$PYTHON" || bash "$HERE/00_env.sh"
    step "1. pinned dependencies"
    if [ -d "$TOCTOU_REPO/pizero" ] && [ -f "$SCOURGE_ROOT/config/key.local" ] && [ -x "$NUDL_BIN" ]; then
        echo "already fetched — skipping"
    else
        bash "$HERE/00_fetch_third_party.sh"
    fi
    step "2. stage the official package"
    if have "$INPUT_DIR/mango-rootfs.tar.gz" && [ "$FORCE" -eq 0 ]; then
        echo "already staged — skipping"
    elif [ -n "$FROM_DIR" ]; then
        bash "$HERE/01_stage_inputs.sh" --from-dir "$FROM_DIR"
    else
        bash "$HERE/01_stage_inputs.sh"
    fi
    step "3. decrypt the rootfs"
    have "$WORK_DIR/stock-plain.tar.gz" || bash "$HERE/02_decrypt_rootfs.sh"
    step "4. patch, repack, verify, image, preflight"
    if have "$OUT_DIR/stage1.te2" && have "$OUT_DIR/usb.img" && [ "$FORCE" -eq 0 ]; then
        echo "artifacts present — skipping (use --force to rebuild)"
    else
        bash "$HERE/build_all.sh"
    fi
    echo
    echo "prep done. Next: scripts/00_toctou.sh rig"
}

#: 10 runs ON the board. Copy what it needs, then hand it to sudo — the same
#: shape the dashboard's "Run on the rig" uses. Needs SUDO_PASSWORD unless the
#: board was built with passwordless sudo, in which case sudo -n works.
provision_board() {
    local local_env="$SCOURGE_ROOT/config/rig.local.env"
    [ -f "$local_env" ] && { source "$local_env"; } || true
    rsync -a --relative -e 'ssh -o BatchMode=yes' \
        scripts/lib.sh config/firmware_locks.env third_party/ioniq5-usb-toctou/pizero/system \
        scripts/10_provision_pi.sh scripts/17_provision_wifi_fallback.sh \
        tools/rigdash/pi \
        "$PI_USER@$PI_HOST:/home/$PI_USER/$PI_DIR/"
    if [ -n "${SUDO_PASSWORD:-}" ]; then
        printf '%s\n' "$SUDO_PASSWORD" | \
            ssh "$PI_USER@$PI_HOST" "cd ~/$PI_DIR && sudo -S -p '' bash scripts/10_provision_pi.sh"
    else
        echo "  no SUDO_PASSWORD in config/rig.local.env — run this on the board:"
        echo "    ssh $PI_USER@$PI_HOST 'cd ~/$PI_DIR && sudo bash scripts/10_provision_pi.sh'"
    fi
}

rig_steps() {
    [ -n "$PI_HOST" ] || { echo "set PI_HOST/PI_USER in config/paths.local.env" >&2; exit 1; }
    step "provision the board (10)"
    provision_board
    step "deploy the image (11)"
    bash "$HERE/11_deploy_pi.sh"
    step "verify (12)"
    bash "$HERE/12_verify_pi.sh"
    if [ "$WITH_AGENT" -eq 1 ]; then
        step "control page (18)"
        bash "$HERE/18_deploy_rig_agent.sh"
    fi
}

case "$VERB" in
    doctor) doctor ;;
    prep) prep ;;
    rig) rig_steps ;;
    sd)
        [ -n "$SD_DEV" ] || { echo "usage: $0 sd /dev/sdX" >&2; exit 2; }
        step "flash the card"
        bash "$HERE/13_copy_image_to_sd.sh"
        step "first boot on the card (16)"
        bash "$HERE/16_provision_sd_boot.sh" --card "$SD_DEV"
        ;;
esac
