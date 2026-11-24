#!/system/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Arm the real flipping detector. Run ONLY at the car, with the phone already
# unplugged from the PC: PC enumeration can look like a read pass and consume
# the single flip credit. See docs/21 before using this.
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
. "$HERE/common.sh" || exit 1
need_root || exit 1

# Retire every prior watcher, including a log-only dry run.
stop_watchers

# No boot-time re-arm: a reboot mid-run must not start a detector unattended.
rm -f "$AUTO_FLAG"

: > "$LOG"
: > "$RIG_DIR/events.jsonl"
"$BIN" flip -c "$CFG" -mode genuine >> "$LOG" 2>&1 || {
    echo "FATAL: could not set the genuine table" >&2; exit 1;
}
setsid "$BIN" watchtrace -c "$CFG" >> "$LOG" 2>&1 &
sleep 1

# Only claim ARMED when every status gate passes.
if sh "$HERE/status.sh" --live; then
    echo "=== LIVE ARMED $(date) ===" >> "$LOG"
else
    echo "=== ARM FAILED - gates did not pass $(date) ===" >> "$LOG"
    exit 1
fi
