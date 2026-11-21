#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# package_app.sh — build a Rust Cargo project for the head unit and emit a
# docs/06-compliant USB package: a static-musl binary, a systemd unit, the
# generic SMACK-aware installer, hashes and a manifest.
#
# The toolchain and Cargo run on the BUILDER, never on the head unit. The
# default build is offline against a vendored crate cache; --fetch is the
# explicit online step. Shared libstd is not supported (Rust has no stable
# ABI); the deployed binary must be self-contained.
#
# Usage:
#   tools/rust/scripts/package_app.sh <crate-dir> [--out DIR] [--fetch]
#                                     [--no-build --binary PATH]
#
#   <crate-dir>      project containing Cargo.toml + scourge-app.env
#   --out DIR        output USB tree (default: third_party/rust/out/<app>-usb)
#   --fetch          run `cargo vendor` (network) once, then build offline
#   --no-build       package an existing static binary instead of building
#   --binary PATH    binary for --no-build
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"

# paths: same precedence as scripts/lib.sh, without assuming scripts/.
if [ -f "$ROOT/config/paths.local.env" ]; then
    # shellcheck disable=SC1091
    . "$ROOT/config/paths.local.env"
elif [ -f "$ROOT/config/paths.env.example" ]; then
    # shellcheck disable=SC1091
    . "$ROOT/config/paths.env.example"
fi
THIRD_PARTY_DIR="${THIRD_PARTY_DIR:-third_party}"
case "$THIRD_PARTY_DIR" in
    /*) ;;
    *) THIRD_PARTY_DIR="$ROOT/$THIRD_PARTY_DIR" ;;
esac
RUST_TOOLCHAIN_DIR="${RUST_TOOLCHAIN_DIR:-$THIRD_PARTY_DIR/rust/toolchain}"
case "$RUST_TOOLCHAIN_DIR" in
    /*) ;;
    *) RUST_TOOLCHAIN_DIR="$ROOT/$RUST_TOOLCHAIN_DIR" ;;
esac

TARGET=x86_64-unknown-linux-musl
CRATE=""
OUT=""
FETCH=0
NO_BUILD=0
BINARY=""

usage() {
    cat <<'EOF'
usage: tools/rust/scripts/package_app.sh <crate-dir> [--out DIR] [--fetch]
                                          [--no-build --binary PATH]

  <crate-dir>      project containing Cargo.toml + scourge-app.env
  --out DIR        output USB tree (default: third_party/rust/out/<app>-usb)
  --fetch          run `cargo vendor` (network) once, then build offline
  --no-build       package an existing static binary instead of building
  --binary PATH    binary for --no-build
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --out) shift; OUT="${1:?--out needs a directory}" ;;
        --fetch) FETCH=1 ;;
        --no-build) NO_BUILD=1 ;;
        --binary) shift; BINARY="${1:?--binary needs a path}" ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "ERROR: unknown option: $1" >&2; usage >&2; exit 2 ;;
        *)
            [ -z "$CRATE" ] || { echo "ERROR: only one crate directory is accepted" >&2; exit 2; }
            CRATE="$1"
            ;;
    esac
    shift
done
[ -n "$CRATE" ] || { usage >&2; exit 2; }
CRATE="$(cd "$CRATE" && pwd)"
[ -f "$CRATE/Cargo.toml" ] || { echo "ERROR: no Cargo.toml in $CRATE" >&2; exit 1; }
[ -f "$CRATE/scourge-app.env" ] || {
    echo "ERROR: no scourge-app.env in $CRATE" >&2
    echo "  Copy tools/rust/template/scourge-app.env and fill it in." >&2
    exit 1
}

# app metadata (shell-safe key=value; verified parts are installed from here)
set -a
# shellcheck disable=SC1091
. "$CRATE/scourge-app.env"
set +a
APP_NAME="${APP_NAME:-}"
APP_VERSION="${APP_VERSION:-}"
APP_BIN="${APP_BIN:-}"
APP_SERVICE="${APP_SERVICE:-}"
APP_DATA_DIR="${APP_DATA_DIR:-/rw_data/apps/$APP_NAME}"
APP_DESCRIPTION="${APP_DESCRIPTION:-$APP_NAME}"
printf '%s' "$APP_NAME" | grep -Eq '^[a-z0-9][a-z0-9-]*$' || {
    echo "ERROR: APP_NAME must be lowercase a-z/0-9/-, got: '$APP_NAME'" >&2; exit 1; }
printf '%s' "$APP_BIN" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9._-]*$' || {
    echo "ERROR: APP_BIN is not a safe file name: '$APP_BIN'" >&2; exit 1; }
printf '%s' "$APP_SERVICE" | grep -Eq '^[A-Za-z0-9][A-Za-z0-9._@-]*\.service$' || {
    echo "ERROR: APP_SERVICE must be a systemd unit name ending in .service" >&2; exit 1; }
[ -n "$APP_VERSION" ] || { echo "ERROR: APP_VERSION is empty" >&2; exit 1; }
case "$APP_DATA_DIR" in
    /rw_data/?*)
        ;;
    *)
        echo "ERROR: APP_DATA_DIR must be under /rw_data/ (got: $APP_DATA_DIR)" >&2
        exit 1
        ;;
esac
SERVICE_FILE="$CRATE/service/$APP_SERVICE"
[ -f "$SERVICE_FILE" ] || {
    echo "ERROR: missing service unit: $SERVICE_FILE" >&2
    exit 1
}

BUILD_DIR="$THIRD_PARTY_DIR/rust/build/$APP_NAME"
VENDOR_DIR="$THIRD_PARTY_DIR/rust/vendor/$APP_NAME"
OUT="${OUT:-$THIRD_PARTY_DIR/rust/out/$APP_NAME-usb}"
case "$OUT" in
    /*) ;;
    *) OUT="$ROOT/$OUT" ;;
esac

if [ "$NO_BUILD" = 1 ]; then
    [ -n "$BINARY" ] || { echo "ERROR: --no-build needs --binary PATH" >&2; exit 2; }
    [ -f "$BINARY" ] || { echo "ERROR: binary not found: $BINARY" >&2; exit 1; }
    BINARY="$(cd "$(dirname "$BINARY")" && pwd)/$(basename "$BINARY")"
    mkdir -p "$BUILD_DIR"
    BIN="$BUILD_DIR/$APP_BIN"
    cp -f "$BINARY" "$BIN"
    chmod 0755 "$BIN"
else
    CARGO="$RUST_TOOLCHAIN_DIR/bin/cargo"
    RUSTC="$RUST_TOOLCHAIN_DIR/bin/rustc"
    RUST_LLD="$RUST_TOOLCHAIN_DIR/lib/rustlib/x86_64-unknown-linux-gnu/bin/rust-lld"
    [ -x "$CARGO" ] || {
        echo "ERROR: pinned Rust toolchain not found at $RUST_TOOLCHAIN_DIR" >&2
        echo "  Run: bash scripts/00_fetch_rust.sh" >&2
        exit 1
    }
    [ -x "$RUSTC" ] && [ -x "$RUST_LLD" ] || {
        echo "ERROR: toolchain is incomplete (rustc or rust-lld missing)" >&2
        echo "  Run: bash scripts/00_fetch_rust.sh --force" >&2
        exit 1
    }
    mkdir -p "$BUILD_DIR"
    if [ "$FETCH" = 1 ]; then
        echo "== fetching crate sources into $VENDOR_DIR (network step)"
        FETCH_HOME="$BUILD_DIR/fetch-cargo-home"
        mkdir -p "$FETCH_HOME"
        if [ ! -f "$CRATE/Cargo.lock" ]; then
            CARGO_HOME="$FETCH_HOME" RUSTC="$RUSTC" \
                "$CARGO" generate-lockfile --manifest-path "$CRATE/Cargo.toml"
            echo "   wrote $CRATE/Cargo.lock — commit it for reproducible builds"
        fi
        CARGO_HOME="$FETCH_HOME" RUSTC="$RUSTC" \
            "$CARGO" vendor --locked "$VENDOR_DIR" \
                --manifest-path "$CRATE/Cargo.toml"
    elif [ ! -d "$VENDOR_DIR" ]; then
        echo "ERROR: no vendored crate cache at $VENDOR_DIR" >&2
        echo "  Run once with --fetch (network), then rebuild offline." >&2
        exit 1
    fi

    CARGO_HOME="$BUILD_DIR/cargo-home"
    TARGET_DIR="$BUILD_DIR/target"
    mkdir -p "$CARGO_HOME"
    cat > "$CARGO_HOME/config.toml" <<EOF
[build]
target = "$TARGET"

[target.$TARGET]
linker = "$RUST_LLD"

[source.crates-io]
replace-with = "vendored-sources"

[source.vendored-sources]
directory = "$VENDOR_DIR"
EOF
    echo "== building $APP_NAME $APP_VERSION for $TARGET (offline)"
    CARGO_HOME="$CARGO_HOME" CARGO_TARGET_DIR="$TARGET_DIR" RUSTC="$RUSTC" \
        "$CARGO" build --release --locked --offline --target "$TARGET" \
            --manifest-path "$CRATE/Cargo.toml" \
            --config "target.$TARGET.linker=\"$RUST_LLD\""
    BIN="$TARGET_DIR/$TARGET/release/$APP_BIN"
    [ -f "$BIN" ] || {
        echo "ERROR: cargo did not produce $BIN" >&2
        echo "  Check APP_BIN in scourge-app.env." >&2
        exit 1
    }
fi

# The head unit has no Rust runtime; the binary must be self-contained.
if readelf -l "$BIN" 2>/dev/null | grep 'INTERP' >/dev/null; then
    echo "ERROR: $BIN has a program interpreter (dynamic, not static)" >&2
    exit 1
fi
if readelf -d "$BIN" 2>/dev/null | grep 'NEEDED' >/dev/null; then
    echo "ERROR: $BIN has dynamic library dependencies:" >&2
    readelf -d "$BIN" | grep 'NEEDED' >&2
    exit 1
fi
file "$BIN" | grep 'static' >/dev/null || {
    echo "ERROR: $BIN does not look static; refusing to package it" >&2
    file "$BIN" >&2
    exit 1
}
BIN_BYTES="$(stat -c '%s' "$BIN")"
BIN_SHA="$(sha256sum "$BIN" | cut -d' ' -f1)"

rm -rf "$OUT"
mkdir -p "$OUT/payload/bin" "$OUT/payload/systemd" "$OUT/payload/lib" \
         "$OUT/backup_current"
install -m 0755 "$BIN" "$OUT/payload/bin/$APP_BIN"
install -m 0644 "$SERVICE_FILE" "$OUT/payload/systemd/$APP_SERVICE"
install -m 0644 "$CRATE/scourge-app.env" "$OUT/payload/app.env"
install -m 0644 "$ROOT/packages/common/install_io.sh" "$OUT/payload/lib/install_io.sh"
printf '%s\n' "$BIN_SHA" > "$OUT/payload/app.sha256"
install -m 0755 "$HERE/../assets/install_app.sh" "$OUT/run_stuff.sh"
install -m 0755 "$HERE/../assets/restore_app.sh" "$OUT/run_stuff_restore.sh"

cat > "$OUT/README.txt" <<EOF
$APP_NAME $APP_VERSION — SCOURGE Rust app/daemon package

What this is
  A host-built, static-musl Rust binary. The head unit needs no Rust
  toolchain and no Rust runtime; the binary is self-contained.

Install
  1. Copy the whole contents of this USB tree to a FAT/exFAT stick.
  2. Plug the stick into the head unit; the initial_root failsafe runs
     run_stuff.sh as root within ~8 seconds.
  3. Read run_stuff_result.txt on the stick.
  Installs:
    $APP_DATA_DIR/bin/$APP_BIN
    /etc/systemd/system/$APP_SERVICE

Restore
  Copy run_stuff_restore.sh over run_stuff.sh on the stick and plug it in.
  App data under $APP_DATA_DIR is preserved unless APP_PURGE=1.

After a firmware update, /etc may be replaced. Re-run this installer.

$APP_DESCRIPTION

This does not flash firmware and does not touch the updater, bootloader
or partitions.
EOF

(
    cd "$OUT"
    find payload -type f -print0 | sort -z | xargs -0 md5sum > manifest.md5
)

echo
echo "== packaged $APP_NAME $APP_VERSION"
echo "   binary: $APP_BIN  ${BIN_BYTES} bytes  sha256=$BIN_SHA"
echo "   tree:   $OUT"
echo "   manifest entries: $(wc -l < "$OUT/manifest.md5")"
