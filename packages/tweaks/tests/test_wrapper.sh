#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Wrapper unit test: neutral reset rewrite vs passthrough.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BIN="$ROOT/src/wrandr_wrapper"
[ -x "$BIN" ] || make -C "$ROOT/src"
TMP=$(mktemp -d /tmp/tweaks-wrapper-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/real" <<EOF
#!/bin/sh
echo "\$@" >> "$TMP/log"
exit 0
EOF
chmod +x "$TMP/real"

run() {
    TWEAKS_RW_DIR="$TMP" TWEAKS_REAL_WRANDR="$TMP/real" \
    TWEAKS_CLUSTER_CRTC=1 "$BIN" "$@"
}

echo "128:115:102" > "$TMP/cluster_gamma"
run --crtc=1 --gamma=128:128:128 --brightness=128:128:128
grep -q -- '--crtc=1 --gamma=128:115:102' "$TMP/log"

: > "$TMP/log"
run --crtc=1 --gamma=128:112:96 --brightness=128:128:128
grep -q -- '--crtc=1 --gamma=128:112:96' "$TMP/log"

: > "$TMP/log"
run --crtc=0 --gamma=128:128:128 --brightness=128:128:128
grep -q -- '--crtc=0 --gamma=128:128:128' "$TMP/log"

: > "$TMP/log"
echo off > "$TMP/cluster_gamma"
run --crtc=1 --gamma=128:128:128 --brightness=128:128:128
grep -q -- '--crtc=1 --gamma=128:128:128' "$TMP/log"

: > "$TMP/log"
echo "128:115:102" > "$TMP/cluster_gamma"
# A command without --crtc= must pass through untouched (regression).
run --gamma=128:128:128 --brightness=128:128:128
grep -q -- '--gamma=128:128:128' "$TMP/log"

echo "PASS: wrapper test"
