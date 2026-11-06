#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# lib.sh — shared config resolution for SCOURGE scripts. Source, don't run.
#
# Resolution order for every variable: environment > config/paths.local.env
# > config/paths.env.example defaults. Firmware constants always come from
# config/firmware_locks.env (single source of truth).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[1]}")/.." && pwd)"
SCOURGE_ROOT="$HERE"

# --- paths ---------------------------------------------------------------------
if [ -f "$SCOURGE_ROOT/config/paths.local.env" ]; then
    source "$SCOURGE_ROOT/config/paths.local.env"
elif [ -f "$SCOURGE_ROOT/config/paths.env.example" ]; then
    source "$SCOURGE_ROOT/config/paths.env.example"
fi

PYTHON="${PYTHON:-.venv/bin/python}"
INPUT_DIR="${INPUT_DIR:-input}"
STOCK_PACKAGE="${STOCK_PACKAGE:-input/package_stock}"
THIRD_PARTY_DIR="${THIRD_PARTY_DIR:-third_party}"
TOCTOU_REPO="${TOCTOU_REPO:-third_party/ioniq5-usb-toctou}"
HKG_TOOLS_REPO="${HKG_TOOLS_REPO:-third_party/hkmcavn-tools}"
NUDL_BIN="${NUDL_BIN:-third_party/nudl/nudl}"
WORK_DIR="${WORK_DIR:-work}"
OUT_DIR="${OUT_DIR:-out}"
PI_HOST="${PI_HOST:-}"
PI_USER="${PI_USER:-pi}"
PI_DIR="${PI_DIR:-scourge}"
PI_ROOTFS_LABEL="${PI_ROOTFS_LABEL:-rootfs}"
PI_ROOTFS_PARTUUID="${PI_ROOTFS_PARTUUID:-}"

# Resolve relative paths against the repo root.
for _v in PYTHON INPUT_DIR STOCK_PACKAGE THIRD_PARTY_DIR TOCTOU_REPO \
          HKG_TOOLS_REPO NUDL_BIN WORK_DIR OUT_DIR; do
    _val="${!_v}"
    case "$_val" in
        /*) ;;
        *) eval "$_v=\"\$SCOURGE_ROOT/\$_val\"" ;;
    esac
done

# --- firmware locks -------------------------------------------------------------
source "$SCOURGE_ROOT/config/firmware_locks.env"

# --- TE2 content key -------------------------------------------------------------
# NEVER hardcoded here. config/key.local (gitignored) or $TE2_CONTENT_KEY.
load_te2_key() {
    if [ -z "${TE2_CONTENT_KEY:-}" ]; then
        if [ -f "$SCOURGE_ROOT/config/key.local" ]; then
            TE2_CONTENT_KEY="$(head -1 "$SCOURGE_ROOT/config/key.local" | tr -d '[:space:]')"
        fi
    fi
    if [ -z "${TE2_CONTENT_KEY:-}" ]; then
        echo "ERROR: TE2 content key not set." >&2
        echo "  Run scripts/00_fetch_third_party.sh (extracts it from the" >&2
        echo "  public upstream tooling), or put it in config/key.local, or" >&2
        echo "  export TE2_CONTENT_KEY=<32 hex chars>. See config/key.local.example." >&2
        return 1
    fi
    export TE2_CONTENT_KEY
}

# --- helpers ---------------------------------------------------------------------
need_venv() {
    [ -x "${PYTHON}" ] || {
        echo "missing $PYTHON — run scripts/00_env.sh first" >&2
        return 1
    }
}

need_fetched() {
    [ -d "$TOCTOU_REPO/pizero" ] || {
        echo "missing $TOCTOU_REPO — run scripts/00_fetch_third_party.sh first" >&2
        return 1
    }
}

lock_check_or_die() {
    # lock_check_or_die <file> <pinned_sha256> <what>
    local got
    got="$(sha256sum "$1" 2>/dev/null | cut -d' ' -f1)" || true
    if [ "$got" != "$2" ]; then
        echo "ERROR: $3 hash mismatch" >&2
        echo "  got : $got" >&2
        echo "  want: $2" >&2
        echo "  This input is not the firmware SCOURGE's patch tables were" >&2
        echo "  derived for. See docs/09-porting.md before proceeding." >&2
        return 1
    fi
}
