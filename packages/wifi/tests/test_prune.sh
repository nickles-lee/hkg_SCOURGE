#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Offline test for the USB log-bundle prune path (wifi_daemon.c).
# Drives the real binary through --dump-logs against a fake USB root via
# the WIFI_USB_ROOT hook; no VM, no car, no root required (on a non-root
# dev host the daemon's /rw_data writes fail harmlessly, exactly like
# test_daemon.sh).
#
# Asserts (fix for the rm -rf injection):
#   1. NO shell is used to prune: planted injection names must not run.
#      Payloads are slash-free (FAT/exFAT names cannot contain '/') and
#      would land their marker in the daemon's cwd ($RUN).
#   2. Anything not exactly wifi_logs_<14 digits> is skipped, not deleted.
#   3. Only the oldest bundles beyond LOG_DUMP_KEEP are removed.

set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="${WIFI_DAEMON_BIN:-$ROOT/src/wifi_daemon}"
[ -x "$BIN" ] || make -C "$ROOT/src"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/wifi-prune.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
USB="$TMP/usb"
RUN="$TMP/run"
mkdir -p "$USB" "$RUN"

# 7 benign bundles (oldest first, 14-digit stamps like the daemon writes).
for stamp in 20200101000000 20200201000000 20200301000000 20200401000000 \
             20200501000000 20200601000000 20200701000000; do
    mkdir "$USB/wifi_logs_$stamp"
    echo "bundle" > "$USB/wifi_logs_$stamp/state.txt"
done

# A benign stray FILE with a valid bundle name (newer than all bundles):
# same semantics as before — it is a candidate, unlinked not rmdir'd, and
# new enough to be kept.
echo "stray file" > "$USB/wifi_logs_20250101000000"

# Decoys: skipped untouched.  These are the injection payloads the old
# `sh("rm -rf %s/%s")` form would have executed as the daemon (root).
mkdir "$USB/wifi_logs_a'; touch pwned_quote; #"
mkdir "$USB/wifi_logs_20200101000000_extra; touch pwned_suffix; #"
mkdir "$USB/wifi_logs_\`touch pwned_backtick\`"
mkdir "$USB/wifi_logs_\$(touch pwned_dollar)"
mkdir "$USB/wifi_logs_a | touch pwned_pipe"
mkdir "$USB/wifi_logs_short"
mkdir "$USB/wifi_logs_202001010000009"
mkdir "$USB/wifi_logs_2020010100000a"
mkdir "$USB/not_wifi_logs_20200101000000"

cd "$RUN"
WIFI_USB_ROOT="$USB" "$BIN" --dump-logs >/dev/null

# 1. No shell ran: no injection marker anywhere it could land.
for marker in pwned_quote pwned_suffix pwned_backtick pwned_dollar pwned_pipe; do
    if [ -e "$RUN/$marker" ] || [ -e "$USB/$marker" ] || [ -e "/$marker" ]; then
        echo "FAIL: injection marker exists: $marker — a shell ran the name" >&2
        exit 1
    fi
done

# 2. Decoys untouched.
for d in "wifi_logs_a'; touch pwned_quote; #" \
         "wifi_logs_20200101000000_extra; touch pwned_suffix; #" \
         'wifi_logs_`touch pwned_backtick`' \
         'wifi_logs_$(touch pwned_dollar)' \
         "wifi_logs_a | touch pwned_pipe" \
         wifi_logs_short wifi_logs_202001010000009 wifi_logs_2020010100000a \
         not_wifi_logs_20200101000000; do
    if [ ! -e "$USB/$d" ]; then
        echo "FAIL: decoy was deleted: $d" >&2
        exit 1
    fi
done

# 3. Exactly the oldest 4 bundles pruned (7 bundles + stray file + fresh
#    dump = 9 candidates; newest 5 kept: fresh, stray, 202005..202007).
for stamp in 20200101000000 20200201000000 20200301000000 20200401000000; do
    if [ -e "$USB/wifi_logs_$stamp" ]; then
        echo "FAIL: old bundle survived: $stamp" >&2
        exit 1
    fi
done
for stamp in 20200501000000 20200601000000 20200701000000; do
    if [ ! -d "$USB/wifi_logs_$stamp" ]; then
        echo "FAIL: kept bundle missing: $stamp" >&2
        exit 1
    fi
done
if [ ! -f "$USB/wifi_logs_20250101000000" ]; then
    echo "FAIL: kept stray file missing" >&2
    exit 1
fi
NEW=$(find "$USB" -maxdepth 1 -type d -name 'wifi_logs_2026*' | wc -l)
if [ "$NEW" -lt 1 ]; then
    echo "FAIL: fresh dump bundle missing" >&2
    exit 1
fi

rm -rf "$TMP"
echo "PASS: prune is shell-free, name-allowlisted, keeps the newest 5"
