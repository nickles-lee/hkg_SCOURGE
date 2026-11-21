#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Offline test for tools/rust/scripts/package_app.sh layout and gates.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
WORK=$(mktemp -d /tmp/rust_app_layout.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

APP="$WORK/app"
OUT="$WORK/out"
mkdir -p "$APP/src" "$APP/service"
cat > "$APP/Cargo.toml" <<'EOF'
[package]
name = "test-app"
version = "0.1.0"
edition = "2021"
EOF
cat > "$APP/scourge-app.env" <<'EOF'
APP_NAME=test-app
APP_VERSION=0.1.0
APP_BIN=test-app
APP_SERVICE=test-app.service
APP_DATA_DIR=/rw_data/apps/test-app
APP_DESCRIPTION="layout test"
EOF
cat > "$APP/service/test-app.service" <<'EOF'
[Unit]
Description=test-app
[Service]
ExecStart=/rw_data/apps/test-app/bin/test-app
[Install]
WantedBy=multi-user.target
EOF
printf 'fn main() {}\n' > "$APP/src/main.rs"

cat > "$WORK/fixture.c" <<'EOF'
#include <stdio.h>
int main(void) { puts("fixture"); return 0; }
EOF
gcc -static -O2 -o "$WORK/fixture" "$WORK/fixture.c"

echo "== package a static binary =="
sh "$HERE/scripts/package_app.sh" "$APP" --no-build --binary "$WORK/fixture" --out "$OUT"
test -f "$OUT/run_stuff.sh"
test -f "$OUT/run_stuff_restore.sh"
test -x "$OUT/payload/bin/test-app"
test -f "$OUT/payload/systemd/test-app.service"
test -f "$OUT/payload/app.env"
test -f "$OUT/payload/app.sha256"
test -f "$OUT/manifest.md5"
( cd "$OUT" && md5sum -c manifest.md5 >/dev/null )
[ "$(sha256sum "$OUT/payload/bin/test-app" | cut -d' ' -f1)" = \
  "$(cat "$OUT/payload/app.sha256")" ]
echo "layout OK"

echo "== dynamic binaries are rejected =="
if sh "$HERE/scripts/package_app.sh" "$APP" --no-build --binary /bin/true \
        --out "$WORK/out-dynamic" >"$WORK/dynamic.log" 2>&1; then
    echo "package_app accepted a dynamic binary" >&2
    exit 1
fi
grep -q 'dynamic' "$WORK/dynamic.log"
echo "dynamic gate OK"

echo "== unsafe app names are rejected =="
cp "$APP/scourge-app.env" "$WORK/app.env.good"
sed "s/^APP_NAME=.*/APP_NAME='Bad Name'/" "$WORK/app.env.good" > "$APP/scourge-app.env"
if sh "$HERE/scripts/package_app.sh" "$APP" --no-build --binary "$WORK/fixture" \
        --out "$WORK/out-bad" >"$WORK/bad.log" 2>&1; then
    echo "package_app accepted an unsafe APP_NAME" >&2
    exit 1
fi
grep -q 'APP_NAME' "$WORK/bad.log"
echo "name gate OK"

echo "== app data outside /rw_data is rejected =="
sed 's|^APP_DATA_DIR=.*|APP_DATA_DIR=/etc/test-app|' "$WORK/app.env.good" > "$APP/scourge-app.env"
if sh "$HERE/scripts/package_app.sh" "$APP" --no-build --binary "$WORK/fixture" \
        --out "$WORK/out-data" >"$WORK/data.log" 2>&1; then
    echo "package_app accepted APP_DATA_DIR outside /rw_data" >&2
    exit 1
fi
grep -q 'APP_DATA_DIR' "$WORK/data.log"
echo "data-dir gate OK"

echo "== all package layout tests passed =="
