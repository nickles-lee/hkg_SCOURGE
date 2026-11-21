#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 00_fetch_third_party.sh — fetch + verify the pinned third-party dependencies.
#
# SCOURGE ships no third-party code. This script clones the pinned upstreams
# into third_party/ (gitignored) and extracts the TE2 content key from the
# public upstream tooling into config/key.local (gitignored). Pins live in
# config/third_party.lock; a failed pin is a hard stop.
#
# Takedown/offline mode:
#   --from-archive <dir-or-tarball>   seed third_party/ from a local copy
#   (an existing third_party/ cache is verified and reused as-is)
#
# Usage: 00_fetch_third_party.sh [--from-archive PATH]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

FROM_ARCHIVE=""
[ "${1:-}" = "--from-archive" ] && FROM_ARCHIVE="${2:?--from-archive needs a path}"

mkdir -p "$THIRD_PARTY_DIR" "$SCOURGE_ROOT/config"

# --- 1. ioniq5-usb-toctou (TE2 tools, nbdkit plugin, Pi gadget scripts) ----------
TOCTOU_PIN="4b343d0c7c8ce469d2bb752c1c335d0378c42ebe"
TOCTOU_URL="https://github.com/CloudPapi/ioniq5-usb-toctou"

fetch_toctou() {
    local dst="$TOCTOU_REPO"
    if [ -d "$dst/.git" ] || [ -f "$dst/pizero/te2_encrypt.py" ]; then
        echo "== $dst already present (using local cache)"
        return 0
    fi
    if [ -n "$FROM_ARCHIVE" ] && [ -e "$FROM_ARCHIVE/ioniq5-usb-toctou" ]; then
        echo "== seeding toctou from $FROM_ARCHIVE"
        cp -a "$FROM_ARCHIVE/ioniq5-usb-toctou" "$dst"
        return 0
    fi
    echo "== cloning $TOCTOU_URL @ $TOCTOU_PIN"
    git clone --quiet "$TOCTOU_URL" "$dst"
    ( cd "$dst" && [ "$(git rev-parse HEAD)" = "$TOCTOU_PIN" ] ) || {
        echo "ERROR: upstream HEAD does not match the pinned commit." >&2
        echo "  Upstream may have moved. Do NOT use an unpinned clone;" >&2
        echo "  restore from a local archive (--from-archive) instead." >&2
        exit 1
    }
}

# --- 2. hkmcavn-tools (public TE2 format reference + content key) ---------------
HKG_PIN="2570e130d4f4cc586b30e30a146f457ed7053f2b"
HKG_URL="https://github.com/galaxy4public/hkmcavn-tools"

fetch_key() {
    local dst="$HKG_TOOLS_REPO" xcrypt key
    if [ -f "$SCOURGE_ROOT/config/key.local" ] && \
       [ "$(head -1 "$SCOURGE_ROOT/config/key.local" | tr -d '[:space:]')" != \
         "00000000000000000000000000000000" ]; then
        echo "== config/key.local already populated"
        return 0
    fi
    if [ ! -d "$dst" ]; then
        if [ -n "$FROM_ARCHIVE" ] && [ -e "$FROM_ARCHIVE/hkmcavn-tools" ]; then
            echo "== seeding hkmcavn-tools from $FROM_ARCHIVE"
            cp -a "$FROM_ARCHIVE/hkmcavn-tools" "$dst"
        else
            echo "== cloning $HKG_URL @ $HKG_PIN (public TE2 tooling; the"
            echo "   content key is the KEY constant in xcrypt/xcrypt.py)"
            git clone --quiet "$HKG_URL" "$dst"
            ( cd "$dst" && git checkout --quiet "$HKG_PIN" ) || {
                echo "ERROR: could not check out the pinned hkmcavn-tools commit." >&2
                exit 1
            }
        fi
    fi
    xcrypt="$dst/xcrypt/xcrypt.py"
    [ -f "$xcrypt" ] || { echo "ERROR: $xcrypt missing" >&2; exit 1; }
    key="$(python3 - "$xcrypt" <<'PY'
import re, sys
src = open(sys.argv[1]).read()
m = re.search(r"^KEY\s*=\s*['\"]([0-9a-fA-F]{32})['\"]", src, re.M)
print(m.group(1) if m else "")
PY
)"
    [ -n "$key" ] || {
        echo "ERROR: could not extract the TE2 content key from $xcrypt." >&2
        echo "  The upstream layout may have changed. Put the 32-hex key in" >&2
        echo "  config/key.local manually (see config/key.local.example)." >&2
        exit 1
    }
    printf '%s\n' "$key" > "$SCOURGE_ROOT/config/key.local"
    chmod 600 "$SCOURGE_ROOT/config/key.local"
    echo "== wrote config/key.local (gitignored)"
}

# --- 3. nudl (official-firmware downloader) ---------------------------------------
# Signed release binary, pinned by size/hash in config/third_party.lock. The
# release channel is SSH-signed; the sha256 pins are the mechanical gate the
# build enforces (verify the signature per upstream when you update the pin).
NUDL_VERSION="1.3.0"
NUDL_ASSET="nudl-${NUDL_VERSION}-x86_64-unknown-linux-musl.zip"
NUDL_URL="https://github.com/chenxiaolong/nudl/releases/download/v${NUDL_VERSION}/${NUDL_ASSET}"
NUDL_ZIP_SHA256="66d0e1c6ab35a5ca372ed47125bbb633d9a5c995347651910b535caff94ad028"
NUDL_BIN_SHA256="7c0c022013ef0449d5cc13b657e93983eb792e0e279ce947b2135e031498aecc"

fetch_nudl() {
    local dst="$THIRD_PARTY_DIR/nudl" tmp
    if [ -x "$NUDL_BIN" ]; then
        if [ "$(sha256sum "$NUDL_BIN" | cut -d' ' -f1)" = "$NUDL_BIN_SHA256" ]; then
            echo "== nudl $NUDL_VERSION already present (pinned sha256)"
            return 0
        fi
        echo "ERROR: $NUDL_BIN does not match the pinned sha256." >&2
        echo "  Delete it and rerun this script to restore the pin." >&2
        return 1
    fi
    if [ -n "$FROM_ARCHIVE" ] && [ -e "$FROM_ARCHIVE/nudl" ]; then
        echo "== seeding nudl from $FROM_ARCHIVE"
        mkdir -p "$dst"
        if [ -d "$FROM_ARCHIVE/nudl" ]; then
            cp -a "$FROM_ARCHIVE/nudl/." "$dst/"
        else
            cp -a "$FROM_ARCHIVE/nudl" "$NUDL_BIN"
        fi
        chmod +x "$NUDL_BIN"
        echo "$NUDL_BIN_SHA256  $NUDL_BIN" | sha256sum -c - || return 1
        return 0
    fi
    command -v curl >/dev/null 2>&1 || {
        echo "ERROR: curl is required to fetch the pinned nudl release." >&2
        echo "  Download $NUDL_URL manually, verify its sha256" >&2
        echo "  $NUDL_ZIP_SHA256, extract nudl to $NUDL_BIN, and rerun." >&2
        return 1
    }
    command -v unzip >/dev/null 2>&1 || {
        echo "ERROR: unzip is required to extract the pinned nudl release." >&2
        return 1
    }
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/nudl-fetch.XXXXXX")"
    echo "== fetching nudl $NUDL_VERSION"
    if ! curl -fsSL --retry 3 -o "$tmp/$NUDL_ASSET" "$NUDL_URL"; then
        rm -rf "$tmp"
        echo "ERROR: could not download $NUDL_URL" >&2
        echo "  Offline? Seed third_party/ with --from-archive <dir>." >&2
        return 1
    fi
    echo "$NUDL_ZIP_SHA256  $tmp/$NUDL_ASSET" | sha256sum -c - || {
        rm -rf "$tmp"; return 1; }
    mkdir -p "$dst"
    unzip -q -o "$tmp/$NUDL_ASSET" -d "$dst" || { rm -rf "$tmp"; return 1; }
    chmod +x "$NUDL_BIN"
    echo "$NUDL_BIN_SHA256  $NUDL_BIN" | sha256sum -c - || {
        rm -rf "$tmp"; return 1; }
    rm -rf "$tmp"
    echo "== nudl $NUDL_VERSION ready at $NUDL_BIN"
}

fetch_toctou
fetch_key
fetch_nudl

echo
echo "== verifying the fetched tools are usable"
PY="$PYTHON"; [ -x "$PY" ] || PY=python3
"$PY" "$TOCTOU_REPO/pizero/gen5w_decrypt.py" 2>&1 | head -2 || true

echo
echo "Fetched dependencies OK. Record the sha256 of the consumed files in"
echo "config/third_party.lock on first fetch (the placeholders are marked)."
