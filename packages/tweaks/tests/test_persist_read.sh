#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host test for the PCL read-app fallback (no vendor PCL required).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
TMP=$(mktemp -d /tmp/tweaks-persist-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

${CC:-cc} -O2 -DTWEAKS_PERSIST_TEST -c "$ROOT/src/tweaks_persist.c" \
    -o "$TMP/persist.o"
${CC:-cc} -O2 "$HERE/test_persist_read.c" "$TMP/persist.o" \
    -o "$TMP/test_persist_read"
"$TMP/test_persist_read" >/dev/null
echo "PASS: PCL read app fallback"
