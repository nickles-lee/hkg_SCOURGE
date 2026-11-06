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
fetch_nudl() {
    local dst="$THIRD_PARTY_DIR/nudl"
    if [ -x "$NUDL_BIN" ]; then
        echo "== nudl already present"
        return 0
    fi
    if [ -n "$FROM_ARCHIVE" ] && [ -e "$FROM_ARCHIVE/nudl" ]; then
        echo "== seeding nudl from $FROM_ARCHIVE"
        cp -a "$FROM_ARCHIVE/nudl" "$dst"
        return 0
    fi
    if command -v cargo >/dev/null 2>&1; then
        echo "== building nudl from source (pinned via Cargo; see third_party.lock)"
        git clone --quiet https://github.com/chenxiaolong/nudl "$dst"
        ( cd "$dst" && cargo build --release --quiet )
    else
        echo "== cargo not found; download a nudl release binary manually:"
        echo "   https://github.com/chenxiaolong/nudl/releases"
        echo "   (verify its SSH signature per the upstream instructions),"
        echo "   place it at $NUDL_BIN and rerun this script."
        return 0
    fi
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
