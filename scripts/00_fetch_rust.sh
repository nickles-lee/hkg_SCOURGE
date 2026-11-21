#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 00_fetch_rust.sh — fetch the pinned HOST Rust toolchain for static-musl
# apps/daemons. This is a builder dependency only: nothing here is installed
# on the head unit. Pins live in config/third_party.lock [rust-toolchain];
# a failed hash is a hard stop (AGENTS.md rule 4). No rustup, no third-party
# source in git: the downloaded trees stay under gitignored third_party/.
#
# Usage:
#   scripts/00_fetch_rust.sh [--check] [--force] [--from-archive DIR]
#
# --check          verify an existing install and exit (no network)
# --force          reinstall the components over the existing prefix
# --from-archive   use already-downloaded dist tarballs from DIR (offline)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$HERE/lib.sh"

CHECK=0
FORCE=0
FROM_ARCHIVE=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --check) CHECK=1 ;;
        --force) FORCE=1 ;;
        --from-archive)
            shift
            FROM_ARCHIVE="${1:?--from-archive needs a directory}"
            [ -d "$FROM_ARCHIVE" ] || {
                echo "ERROR: --from-archive is not a directory: $FROM_ARCHIVE" >&2
                exit 1
            }
            ;;
        -h|--help)
            sed -n '4,15p' "$0"
            exit 0
            ;;
        *)
            echo "ERROR: unknown argument: $1" >&2
            exit 2
            ;;
    esac
    shift
done

RUST_BASE="${RUST_BASE:-$THIRD_PARTY_DIR/rust}"
case "$RUST_BASE" in
    /*) ;;
    *) RUST_BASE="$SCOURGE_ROOT/$RUST_BASE" ;;
esac
RUST_TOOLCHAIN_DIR="${RUST_TOOLCHAIN_DIR:-$RUST_BASE/toolchain}"
case "$RUST_TOOLCHAIN_DIR" in
    /*) ;;
    *) RUST_TOOLCHAIN_DIR="$SCOURGE_ROOT/$RUST_TOOLCHAIN_DIR" ;;
esac
RUST_DL_DIR="${RUST_DL_DIR:-$RUST_BASE/dl}"
case "$RUST_DL_DIR" in
    /*) ;;
    *) RUST_DL_DIR="$SCOURGE_ROOT/$RUST_DL_DIR" ;;
esac
RUST_CARGO_HOME="${RUST_CARGO_HOME:-$RUST_BASE/cargo-home}"
case "$RUST_CARGO_HOME" in
    /*) ;;
    *) RUST_CARGO_HOME="$SCOURGE_ROOT/$RUST_CARGO_HOME" ;;
esac

LOCK="$SCOURGE_ROOT/config/third_party.lock"

lock_value() {
    # lock_value KEY — value from the [rust-toolchain] section.
    sed -n '/^\[rust-toolchain\]/,/^\[/p' "$LOCK" \
        | awk -F'[[:space:]]*=[[:space:]]*' -v key="$1" '$1 == key { print $2; exit }'
}

need_lock() {
    local value
    value="$(lock_value "$1")"
    [ -n "$value" ] || {
        echo "ERROR: config/third_party.lock [rust-toolchain] is missing $1" >&2
        exit 1
    }
    case "$value" in
        \<*\>*) echo "ERROR: $1 is still a placeholder in config/third_party.lock" >&2; exit 1 ;;
    esac
    printf '%s\n' "$value"
}

RUST_VERSION="$(need_lock version)"
RUST_COMMIT="$(need_lock commit)"
RUST_HOST="$(need_lock host)"
RUST_TARGET="$(need_lock target)"

sha256_of() {
    sha256sum "$1" | cut -d' ' -f1
}

fetch_one() {
    # fetch_one NAME URL SHA256 [ARCHIVE_NAME]
    local name="$1" url="$2" want="$3" archive_name="${4:-}"
    [ -n "$archive_name" ] || archive_name="$(basename "$url")"
    local dst="$RUST_DL_DIR/$archive_name" src=""
    mkdir -p "$RUST_DL_DIR"
    if [ -n "$FROM_ARCHIVE" ]; then
        src="$FROM_ARCHIVE/$archive_name"
        [ -f "$src" ] || {
            echo "ERROR: --from-archive is missing $archive_name" >&2
            exit 1
        }
        if [ "$(sha256_of "$src")" != "$want" ]; then
            echo "ERROR: $src does not match the pinned sha256" >&2
            echo "  got : $(sha256_of "$src")" >&2
            echo "  want: $want" >&2
            exit 1
        fi
        if [ "$src" != "$dst" ]; then
            cp -f "$src" "$dst"
        fi
    elif [ -f "$dst" ]; then
        if [ "$(sha256_of "$dst")" != "$want" ]; then
            echo "== cached $archive_name is corrupt; re-downloading"
            rm -f "$dst"
        fi
    fi
    if [ ! -f "$dst" ]; then
        echo "== downloading $name: $(basename "$url")"
        if command -v curl >/dev/null 2>&1; then
            curl -fL --retry 3 --progress-bar -o "$dst.part" "$url"
        elif command -v wget >/dev/null 2>&1; then
            wget -q -O "$dst.part" "$url"
        else
            echo "ERROR: need curl or wget to download the Rust toolchain" >&2
            exit 1
        fi
        mv -f "$dst.part" "$dst"
    fi
    local got
    got="$(sha256_of "$dst")"
    if [ "$got" != "$want" ]; then
        echo "ERROR: $archive_name hash mismatch" >&2
        echo "  got : $got" >&2
        echo "  want: $want" >&2
        echo "  Do NOT use an unpinned toolchain. Remove $dst and retry, or" >&2
        echo "  restore the pinned tarballs with --from-archive." >&2
        exit 1
    fi
    echo "   verified $archive_name ($(du -h "$dst" | cut -f1))"
}

install_one() {
    # install_one TARBALL
    local tarball="$1" tmp
    tmp="$(mktemp -d "$RUST_BASE/.install.XXXXXX")"
    trap 'rm -rf "$tmp"' RETURN
    echo "== installing $(basename "$tarball")"
    tar -xf "$tarball" -C "$tmp"
    local top
    top="$(find "$tmp" -mindepth 1 -maxdepth 1 -type d | head -1)"
    [ -n "$top" ] && [ -f "$top/install.sh" ] || {
        echo "ERROR: $tarball has no top-level install.sh" >&2
        exit 1
    }
    ( cd "$top" && sh ./install.sh --prefix="$RUST_TOOLCHAIN_DIR" --disable-ldconfig )
}

verify_install() {
    local rustc="$RUST_TOOLCHAIN_DIR/bin/rustc" cargo="$RUST_TOOLCHAIN_DIR/bin/cargo"
    [ -x "$rustc" ] || return 1
    [ -x "$cargo" ] || return 1
    "$rustc" --version | grep "rustc $RUST_VERSION " >/dev/null || return 1
    "$cargo" --version | grep "cargo " >/dev/null || return 1
    "$rustc" -vV | grep "commit-hash: $RUST_COMMIT" >/dev/null || return 1
    "$rustc" -vV | grep "host: $RUST_HOST" >/dev/null || return 1
    [ -n "$(find "$RUST_TOOLCHAIN_DIR/lib/rustlib/$RUST_TARGET/lib" \
        -maxdepth 1 -name 'libstd-*.rlib' -print -quit 2>/dev/null)" ] || return 1
    [ -x "$RUST_TOOLCHAIN_DIR/lib/rustlib/$RUST_HOST/bin/rust-lld" ] || return 1
    return 0
}

if [ "$CHECK" = 1 ]; then
    if verify_install; then
        echo "== Rust $RUST_VERSION toolchain OK: $RUST_TOOLCHAIN_DIR"
        exit 0
    fi
    echo "ERROR: no valid pinned Rust $RUST_VERSION toolchain at $RUST_TOOLCHAIN_DIR" >&2
    echo "  Run: bash scripts/00_fetch_rust.sh" >&2
    exit 1
fi

RUSTC_URL="$(need_lock rustc.url)"
RUSTC_SHA="$(need_lock rustc.sha256)"
CARGO_URL="$(need_lock cargo.url)"
CARGO_SHA="$(need_lock cargo.sha256)"
STD_GNU_URL="$(need_lock std_gnu.url)"
STD_GNU_SHA="$(need_lock std_gnu.sha256)"
STD_MUSL_URL="$(need_lock std_musl.url)"
STD_MUSL_SHA="$(need_lock std_musl.sha256)"

if [ "$FORCE" = 0 ] && verify_install; then
    echo "== Rust $RUST_VERSION toolchain already present: $RUST_TOOLCHAIN_DIR"
else
    mkdir -p "$RUST_BASE" "$RUST_DL_DIR"
    fetch_one rustc "$RUSTC_URL" "$RUSTC_SHA"
    fetch_one cargo "$CARGO_URL" "$CARGO_SHA"
    fetch_one "rust-std $RUST_HOST" "$STD_GNU_URL" "$STD_GNU_SHA"
    fetch_one "rust-std $RUST_TARGET" "$STD_MUSL_URL" "$STD_MUSL_SHA"
    if [ -d "$RUST_TOOLCHAIN_DIR" ]; then
        echo "== replacing existing toolchain prefix (invalid or --force)"
        rm -rf "$RUST_TOOLCHAIN_DIR"
    fi
    mkdir -p "$RUST_TOOLCHAIN_DIR"
    install_one "$RUST_DL_DIR/$(basename "$RUSTC_URL")"
    install_one "$RUST_DL_DIR/$(basename "$CARGO_URL")"
    install_one "$RUST_DL_DIR/$(basename "$STD_GNU_URL")"
    install_one "$RUST_DL_DIR/$(basename "$STD_MUSL_URL")"
    verify_install || {
        echo "ERROR: the installed toolchain does not match the pin" >&2
        echo "  version: $RUST_VERSION  commit: $RUST_COMMIT  host: $RUST_HOST" >&2
        echo "  prefix:  $RUST_TOOLCHAIN_DIR" >&2
        exit 1
    }
fi

mkdir -p "$RUST_CARGO_HOME"
ENV_FILE="$RUST_BASE/env.sh"
cat > "$ENV_FILE" <<EOF
# Generated by scripts/00_fetch_rust.sh — gitignored; safe to source.
export RUST_TOOLCHAIN_DIR="$RUST_TOOLCHAIN_DIR"
export RUST_TARGET="$RUST_TARGET"
export CARGO_HOME="\${CARGO_HOME:-$RUST_CARGO_HOME}"
case ":\$PATH:" in
    *":\$RUST_TOOLCHAIN_DIR/bin:"*) ;;
    *) export PATH="\$RUST_TOOLCHAIN_DIR/bin:\$PATH" ;;
esac
EOF
chmod 0644 "$ENV_FILE"
echo
echo "Rust $RUST_VERSION host toolchain ready."
echo "  toolchain: $RUST_TOOLCHAIN_DIR"
echo "  cargo:     $RUST_TOOLCHAIN_DIR/bin/cargo"
echo "  target:    $RUST_TARGET"
echo "  env:       . $ENV_FILE"
echo "Next: bash tools/rust/scripts/package_app.sh tools/rust/examples/scourge-statusd --fetch"
