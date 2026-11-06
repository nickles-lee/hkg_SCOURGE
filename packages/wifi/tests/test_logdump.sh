#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Log-dump test: --dump-logs one-shot against a fake USB root.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
BIN="$ROOT/src/wifi_daemon"
[ -x "$BIN" ] || make -C "$ROOT/src"

USB=$(mktemp -d /tmp/wifi-logdump.XXXXXX)
trap 'rm -rf "$USB"' EXIT

out=$(WIFI_USB_ROOT="$USB" "$BIN" --dump-logs 2>/dev/null)
echo "$out" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] is True, d'

bundle=$(ls -d "$USB"/wifi_logs_* 2>/dev/null | head -1)
[ -n "$bundle" ] || { echo "no wifi_logs_ bundle created" >&2; exit 1; }
test -f "$bundle/state.txt" || { echo "state.txt missing" >&2; exit 1; }
grep -q 'wifi log dump' "$bundle/state.txt" || { echo "state.txt header missing" >&2; exit 1; }
grep -q 'uname -a' "$bundle/state.txt" || { echo "snapshot commands missing" >&2; exit 1; }

# config.conf must be copied redacted when a config exists.
if [ -f /rw_data/wifi/config.conf ]; then
    test -f "$bundle/config.conf" || { echo "config copy missing" >&2; exit 1; }
    if grep -E '^(psk|carplay_psk)=' "$bundle/config.conf" |
         grep -qv '<redacted>'; then
        echo "secrets not redacted" >&2
        exit 1
    fi
    grep -q 'psk=<redacted>' "$bundle/config.conf" ||
        { echo "redaction marker missing" >&2; exit 1; }
fi

# Pruning: 6 old bundles + 1 new one must leave only the newest 5.
i=0
while [ $i -lt 6 ]; do
    mkdir "$USB/wifi_logs_202001010$i"0
    i=$((i + 1))
done
WIFI_USB_ROOT="$USB" "$BIN" --dump-logs >/dev/null 2>&1
n=$(ls -1d "$USB"/wifi_logs_* 2>/dev/null | wc -l)
[ "$n" -eq 5 ] || { echo "pruning failed: $n bundles left (want 5)" >&2; exit 1; }
ls -d "$USB"/wifi_logs_2020010101000 >/dev/null 2>&1 &&
    { echo "pruning removed the wrong bundles" >&2; exit 1; }

# The car mounts exFAT as Tuxera "texfat" (and FUSE sticks as fuseblk).
# find_usb_dir must accept those, using a synthetic /proc/mounts.
USB2=$(mktemp -d /tmp/wifi-texfat.XXXXXX)
MOUNTS=$(mktemp /tmp/wifi-mounts.XXXXXX)
printf '/dev/sdb1 %s texfat rw,relatime 0 0\n' "$USB2" > "$MOUNTS"
out2=$(WIFI_MOUNTS_FILE="$MOUNTS" "$BIN" --dump-logs 2>/dev/null)
echo "$out2" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] is True, d'
ls -d "$USB2"/wifi_logs_* >/dev/null 2>&1 ||
    { echo "texfat mount not accepted" >&2; exit 1; }
printf '/dev/sdb1 %s fuseblk rw,relatime 0 0\n' "$USB2" > "$MOUNTS"
out3=$(WIFI_MOUNTS_FILE="$MOUNTS" "$BIN" --dump-logs 2>/dev/null)
echo "$out3" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["ok"] is True, d'
rm -rf "$USB2" "$MOUNTS"

# An invalid target must fail cleanly instead of writing anywhere.
touch "$USB/notadir"
if WIFI_USB_ROOT="$USB/notadir/sub" "$BIN" --dump-logs >/dev/null 2>&1; then
    echo "dump to an invalid path should fail" >&2
    exit 1
fi

echo "PASS: log dump to USB"
