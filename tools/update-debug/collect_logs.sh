#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Collect the second-stage update logs as root. Read-only on the HU.
#
# Copy this file to the root of a FAT/exFAT USB stick as "run_stuff.sh".
# The first-stage failsafe runs it as root within ~8-20 s.
# It writes scourge_logs.tar.gz and scourge_logs_result.txt to the
# stick root. It does not modify the head unit.
#
# The tar includes the DLT logs, the AppUpgrade log, the update-drive
# preflight report, and the failsafe log. The DLT directory is root-only, so
# this must run via the failsafe (or sudo over SSH), not as the car user.

set -u
PATH=/bin:/sbin:/usr/bin:/usr/sbin
USB_DIR="${USB_DIR:-$(dirname "$0")}"
OUT="$USB_DIR/scourge_logs.tar.gz"
ERR="$USB_DIR/scourge_logs.err"
RESULT="$USB_DIR/scourge_logs_result.txt"

tar -czf "$OUT" \
    /log_data/AppUpgrade \
    /log_data/dltlog \
    /log_data/standarddlt \
    /log_data/kernel \
    /rw_data/second_stage_preflight.txt \
    /rw_data/initial_root.log \
    /rw_data/dlt \
    /update \
    2>"$ERR"
rc=$?

{
    echo "SECOND_STAGE_LOGS"
    echo "time=$(date -u 2>/dev/null)"
    echo "usb=$USB_DIR"
    echo "uid=$(id -u 2>/dev/null)"
    echo "rc=$rc"
    ls -lh "$OUT" 2>/dev/null
    echo "--- tar stderr tail ---"
    tail -30 "$ERR" 2>/dev/null
} > "$RESULT" 2>&1

sync
exit 0
