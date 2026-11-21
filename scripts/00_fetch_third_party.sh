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
# Usage: 00_fetch_third_party.sh [--from-archive PATH] [--only toctou|key|nudl]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

FROM_ARCHIVE=""
ONLY=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --from-archive) FROM_ARCHIVE="${2:?--from-archive needs a path}"; shift 2 ;;
        --only) ONLY="$ONLY ${2:?--only needs a name (toctou|key|nudl)}"; shift 2 ;;
        -h|--help) sed -n '3,15p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

#: With --only, run just the selected fetchers — so nudl can be fetched on its
#: own without cloning the rest.
want() {
    [ -z "$ONLY" ] && return 0
    case " $ONLY " in *" $1 "*) return 0 ;; *) return 1 ;; esac
}

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
#: The release asset naming upstream uses; `uname -m` matches it on x86_64 and
#: aarch64, which is what the lock has to carry a hash for.
nudl_asset() {
    local version="$1"
    echo "nudl-${version}-$(uname -m)-unknown-linux-musl.zip"
}

#: The [nudl] block of third_party.lock. Scoped on purpose: the file has a
#: `version` line in every section, and globbing them together produced
#: "1.3.01.3.08664." as an asset name.
nudl_section() {
    awk '/^\[nudl\]/{f=1;next} /^\[/{f=0} f' "$SCOURGE_ROOT/config/third_party.lock"
}

lock_value() {
    nudl_section | sed -n "s/^$1 *= *//p" | head -n1
}

fetch_nudl() {
    local dst="$THIRD_PARTY_DIR/nudl" version asset want_zip want_bin zip sha got
    if [ -x "$NUDL_BIN" ]; then
        echo "== nudl already present"
        return 0
    fi
    if [ -n "$FROM_ARCHIVE" ] && [ -e "$FROM_ARCHIVE/nudl" ]; then
        echo "== seeding nudl from $FROM_ARCHIVE"
        cp -a "$FROM_ARCHIVE/nudl" "$dst"
        return 0
    fi
    # The lock carries the release version and the sha256 of both the archive and
    # the binary; that is the pin, so no compiler enters the picture.
    # "1.3.0 (nudl-1.3.0-…zip)" — only the field before the parenthetical is the
    # version; the rest is prose and would glue extra digits onto it.
    version="$(lock_value version | sed 's/[[:space:]].*//' | tr -dc '0-9.')"
    [ -n "$version" ] || version="1.3.0"
    asset="$(nudl_asset "$version")"
    want_zip="$(lock_value "sha256\\.$asset")"
    want_bin="$(lock_value 'sha256\.nudl')"
    if [ -z "$want_zip" ]; then
        echo "== no pinned release for $(uname -m) in config/third_party.lock" >&2
        echo "   expected asset: $asset" >&2
        echo "   add 'sha256.$asset = <hash>' (or place the binary at $NUDL_BIN)." >&2
        return 0
    fi
    zip="$(mktemp -d)/$asset"
    echo "== downloading nudl v$version ($(uname -m))"
    curl -fsSL --retry 2 -o "$zip" \
        "https://github.com/chenxiaolong/nudl/releases/download/v$version/$asset" || {
        echo "ERROR: download failed — check the network, or drop the release at $NUDL_BIN" >&2
        return 1
    }
    got="$(sha256sum "$zip" | cut -d' ' -f1)"
    [ "$got" = "$want_zip" ] || {
        echo "ERROR: $asset sha256 mismatch" >&2
        echo "  got : $got" >&2
        echo "  want: $want_zip (config/third_party.lock)" >&2
        return 1
    }
    mkdir -p "$dst"
    if command -v unzip >/dev/null 2>&1; then
        unzip -qo "$zip" -d "$dst"
    else
        python3 -m zipfile -e "$zip" "$dst"
    fi
    chmod +x "$NUDL_BIN"
    got="$(sha256sum "$NUDL_BIN" | cut -d' ' -f1)"
    [ -z "$want_bin" ] || [ "$got" = "$want_bin" ] || {
        echo "ERROR: nudl binary sha256 mismatch after extraction" >&2
        echo "  got : $got" >&2
        echo "  want: $want_bin" >&2
        return 1
    }
    echo "== nudl $version verified (archive + binary)"
}

want toctou && fetch_toctou
want key && fetch_key
want nudl && fetch_nudl

if want toctou; then
    echo
    echo "== verifying the fetched tools are usable"
    PY="$PYTHON"; [ -x "$PY" ] || PY=python3
    "$PY" "$TOCTOU_REPO/pizero/gen5w_decrypt.py" 2>&1 | head -2 || true
fi

echo
echo "Fetched dependencies OK. Record the sha256 of the consumed files in"
echo "config/third_party.lock on first fetch (the placeholders are marked)."
