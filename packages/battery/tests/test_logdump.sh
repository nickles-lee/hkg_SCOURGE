#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Offline test for --dump-logs: bundle contents, pruning, and no-USB failure.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DAEMON="$HERE/src/battery_daemon"
TMP=$(mktemp -d /tmp/battery_logdump.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

[ -x "$DAEMON" ] || make -C "$HERE/src" >/dev/null

RW="$TMP/rw"
USB="$TMP/usb"
mkdir -p "$RW" "$USB"
printf 'battery_daemon: line one\nbattery_daemon: line two\n' > "$RW/daemon.log"
printf '{"ok":true,"ver":"test"}\n' > "$RW/status.json"
printf 'install line\n' > "$RW/install.log"
printf 'restore line\n' > "$RW/restore.log"
printf 'result=OK\n' > "$RW/result.txt"
printf '_\n' > "$RW/smack_expected"

echo "== dump bundle =="
"$DAEMON" --dump-logs --usb "$USB" --status-file "$RW/status.json" \
    --reason test >/dev/null 2>&1
BUNDLE=$(ls -d "$USB"/battery_logs_* 2>/dev/null | head -n 1)
[ -n "$BUNDLE" ] || { echo "no bundle created" >&2; exit 1; }
test -f "$BUNDLE/state.txt"
test -f "$BUNDLE/status.json"
test -f "$BUNDLE/daemon.log"
test -f "$BUNDLE/install.log"
test -f "$BUNDLE/restore.log"
test -f "$BUNDLE/result.txt"
test -f "$BUNDLE/smack_expected"
grep -q 'battery log dump' "$BUNDLE/state.txt"
grep -q 'reason=test' "$BUNDLE/state.txt"
grep -q 'line two' "$BUNDLE/daemon.log"
grep -q 'install line' "$BUNDLE/install.log"
grep -q 'state=ok' "$RW/dump_result"
grep -q "dir=$BUNDLE" "$RW/dump_result"
echo "bundle OK: $BUNDLE"

echo "== pruning =="
for i in 1 2 3 4 5; do
    mkdir "$USB/battery_logs_2026010100000$i"
done
"$DAEMON" --dump-logs --usb "$USB" --status-file "$RW/status.json" \
    --reason prune >/dev/null 2>&1
COUNT=$(find "$USB" -maxdepth 1 -type d -name 'battery_logs_*' | wc -l)
[ "$COUNT" -eq 5 ] || { echo "expected 5 bundles, got $COUNT" >&2; exit 1; }
[ ! -d "$USB/battery_logs_20260101000001" ] || { echo "oldest bundle was not pruned" >&2; exit 1; }
echo "pruning OK: $COUNT bundles kept"

echo "== no USB =="
: > "$TMP/empty-mounts"
if BATTERY_MOUNTS_FILE="$TMP/empty-mounts" \
   "$DAEMON" --dump-logs --status-file "$RW/status.json" \
       --reason no-usb >/dev/null 2>&1; then
    echo "dump succeeded without a USB mount" >&2
    exit 1
fi
grep -q 'state=error' "$RW/dump_result"
grep -q 'no USB mount found' "$RW/dump_result"
echo "no-USB failure OK"

echo "ALL LOG DUMP TESTS PASSED"
