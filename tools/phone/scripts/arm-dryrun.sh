#!/system/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Zero-risk validation: the detector logs where it WOULD flip but cannot change
# the served bytes. Use this on the car before any real run.
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
. "$HERE/common.sh" || exit 1
need_root || exit 1

stop_watchers
rm -f "$AUTO_FLAG"

: > "$LOG"
: > "$RIG_DIR/events.jsonl"
"$BIN" flip -c "$CFG" -mode genuine >> "$LOG" 2>&1 || {
    echo "FATAL: could not set the genuine table" >&2; exit 1;
}
setsid "$BIN" watchtrace -c "$CFG" -logonly >> "$LOG" 2>&1 &
sleep 1
if sh "$HERE/status.sh" --dryrun; then
    echo "=== DRY RUN ARMED $(date) ===" >> "$LOG"
else
    echo "=== DRY RUN ARM FAILED - gates did not pass $(date) ===" >> "$LOG"
    exit 1
fi
