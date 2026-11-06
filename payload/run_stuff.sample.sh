#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# run_stuff.sh — SCOURGE USB failsafe sample.
#
# Put this file at the ROOT of a FAT/exFAT USB stick, plug it into the head
# unit. The initial-root failsafe service runs it as root within ~8 s of a
# content change (the loader md5-gates repeats: change the file to re-run).
# It writes RUN_STUFF_OK.txt back to the stick. Use it as a template for your
# own payloads — see docs/06-run-stuff.md for the full contract.
#
# Optional stick contents:
#   hello.txt            copied to /rw_data and echoed back (read/write proof)
#   stock/AppUpgrade     a plaintext STOCK /app/bin/AppUpgrade; when present
#                        the installed updater is restored to stock first
#   stock/KEEP_STOCK     when present, the updater guard is NOT re-applied
set -u
USB_DIR="${USB_DIR:-/media/usb}"
OUT="$USB_DIR/RUN_STUFF_OK.txt"
RW=/rw_data/RUN_STUFF_OK.txt
STOCK="$USB_DIR/stock/AppUpgrade"
TARGET=/app/bin/AppUpgrade

# The automounter normally mounts rw; make sure.
mount -o remount,rw "$USB_DIR" 2>/dev/null || true

{
    echo "RUN_STUFF_OK"
    echo "time=$(date -u 2>/dev/null)"
    echo "usb=$USB_DIR"
    echo "uid=$(id -u 2>/dev/null)"
    echo "id=$(id 2>/dev/null)"
    echo "uname=$(uname -a 2>/dev/null)"
    echo "smack=$(cat /proc/self/attr/current 2>/dev/null)"
    echo "mounts:"
    grep -E ' /(rw_data|media|tmp|update|log_data)(/| )' /proc/mounts 2>/dev/null
} > "$OUT" 2>&1

# read proof: copy hello.txt from the stick to /rw_data
if [ -f "$USB_DIR/hello.txt" ]; then
    cp -f "$USB_DIR/hello.txt" /rw_data/hello_from_usb.txt 2>/dev/null
    echo "hello=$(cat "$USB_DIR/hello.txt" 2>/dev/null)" >> "$OUT"
    echo "hello_copied=/rw_data/hello_from_usb.txt" >> "$OUT"
else
    echo "hello=(no hello.txt on stick)" >> "$OUT"
fi

# write proof: a file from the HU back to the stick
echo "hello from the head unit at $(date -u 2>/dev/null)" \
    > "$USB_DIR/hello_from_hu.txt" 2>/dev/null \
    && echo "wrote=$USB_DIR/hello_from_hu.txt" >> "$OUT"

# optional stock-updater restore
if [ -f "$STOCK" ]; then
    was_ro=0
    grep -q ' / .* ro[, ]' /proc/mounts && was_ro=1
    [ "$was_ro" = 1 ] && mount -o remount,rw / 2>/dev/null
    [ -f "$TARGET" ] && cp -f "$TARGET" "$USB_DIR/AppUpgrade.before-restore" 2>/dev/null
    cp -f "$STOCK" "$TARGET" 2>/dev/null
    rc=$?
    sync
    [ "$was_ro" = 1 ] && mount -o remount,ro / 2>/dev/null
    echo "restore_rc=$rc" >> "$OUT"
    echo "restore_md5=$(md5sum "$TARGET" 2>/dev/null | cut -d' ' -f1)" >> "$OUT"
else
    echo "restore=skipped (no stock/AppUpgrade)" >> "$OUT"
fi

# re-apply the updater stubs unless KEEP_STOCK is present
if [ -f "$USB_DIR/stock/KEEP_STOCK" ]; then
    echo "updater_patch=skipped (KEEP_STOCK)" >> "$OUT"
else
    [ -f /etc/initial_root/updater_guard.sh ] && \
        /bin/sh /etc/initial_root/updater_guard.sh >> /rw_data/initial_root.log 2>&1
    echo "updater_guard=re-applied (see /rw_data/initial_root.log)" >> "$OUT"
fi

cp -f "$OUT" "$RW" 2>/dev/null
cp -f "$OUT" "$USB_DIR/run_stuff_result.txt" 2>/dev/null
sync
exit 0
