#!/system/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# On-device bench for the quiet-window trigger. Simulates one full-region read
# pass followed by a read-free gap, for both observed read shapes: with a small
# tail probe first, and with no probe. Does not touch any vehicle.
# Both scripts and tests are deployed flat into the rig directory.
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
. "$HERE/common.sh" || exit 1
LOG=$RIG_DIR/bench.log
: > "$LOG"

# Supply these from your own config.json / image geometry.
DM_NODE=${DM_NODE:?set DM_NODE}
REGION_OFF=${REGION_OFF:?set REGION_OFF in sectors}
REGION_LEN=${REGION_LEN:?set REGION_LEN in sectors}
# Default probe: last 16 sectors of the region, derived from the geometry.
PROBE_OFF=${PROBE_OFF:-$((REGION_OFF + REGION_LEN - 16))}
DM_NAME=$(sed -n 's/.*"dmname"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$CFG" | head -1)

stopall() {
  for c in /proc/[0-9]*/cmdline; do
    if grep -aq 'phone-ri[g]' "$c" 2>/dev/null; then
      p=${c#/proc/}; p=${p%/cmdline}; kill "$p" 2>/dev/null
    fi
  done
  sleep 1
}

probe() {
  echo 1 > /proc/sys/vm/drop_caches
  dd if="$DM_NODE" of=/dev/null bs=512 skip="$PROBE_OFF" count=16 status=none 2>>"$LOG"
}

sweep() {
  echo 1 > /proc/sys/vm/drop_caches
  dd if="$DM_NODE" of=/dev/null bs=512 skip="$REGION_OFF" count="$REGION_LEN" status=none 2>>"$LOG"
}

arm() {
  stopall
  "$BIN" flip -c "$CFG" -mode genuine >>"$LOG" 2>&1
  setsid "$BIN" watchtrace -c "$CFG" >>"$LOG" 2>&1 &
  sleep 1
}

echo "===== CASE A: tail probe + full sweep + quiet gap =====" >> "$LOG"
arm
probe
sweep
sleep 6
echo "-- table A --" >> "$LOG"
"$BIN" status -n "$DM_NAME" >>"$LOG" 2>&1

echo "===== CASE B: full sweep only + quiet gap (no probe) =====" >> "$LOG"
arm
sweep
sleep 6
echo "-- table B --" >> "$LOG"
"$BIN" status -n "$DM_NAME" >>"$LOG" 2>&1

stopall

# The bench flips to evil; always restore the safe hand-off state before exit.
"$BIN" flip -c "$CFG" -mode genuine >>"$LOG" 2>&1 || {
    echo "FATAL: restore genuine failed" >&2; exit 1;
}
echo "-- table after restore --" >> "$LOG"
"$BIN" status -n "$DM_NAME" >>"$LOG" 2>&1

echo "=== RESULT ==="
grep -E 'CASE|QUIET|PASS|SWITCH|ARMED|SKIP|flip |targets|ERROR|table [AB]' "$LOG"
