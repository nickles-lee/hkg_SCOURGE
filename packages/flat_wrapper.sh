#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU 260128
# Build-time substitutions bind this loader to one complete tarball.
# COMBO_FLAT_WRAPPER_REV=10
EXPECTED_SHA256="@TARBALL_SHA256@"
set -u
PATH="${COMBO_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-$(CDPATH= cd -- "$(dirname "$0")" && pwd)}"
RESULT="$USB_DIR/run_stuff_result.txt"
LOG="$USB_DIR/appcombo_install.log"
RW_DIR="${COMBO_RW_DIR:-/rw_data/combo}"
mkdir -p "$RW_DIR" 2>/dev/null || exit 1
MODE=all
if [ -f "$USB_DIR/MODE" ]; then MODE=$(head -n 1 "$USB_DIR/MODE" | tr -d '\r'); fi
[ ! -f "$USB_DIR/RESTORE" ] || MODE=restore
WORK=""
finish() {
    [ -z "$WORK" ] || rm -rf "$WORK"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
fail() {
    printf 'COMBO_FLAT_WRAPPER\nstatus=FAIL\nerror=%s\n' "$*" > "$USB_DIR/wrapper_result.txt"
    cp -f "$USB_DIR/wrapper_result.txt" "$RESULT" 2>/dev/null || true
    printf '%s\n' "$*" >> "$LOG"
    # The inner master never ran; signal this failure using the proven call.
    # Label the copy like the master's beep_init so the micom floor rule
    # applies; best-effort only.
    beep="$RW_DIR/bin/dbus-send"
    if [ ! -x "$beep" ]; then
        mkdir -p "$RW_DIR/bin" 2>/dev/null || true
        helper=$(command -v dbus-send 2>/dev/null || true)
        [ -z "$helper" ] || cp -f "$helper" "$beep" 2>/dev/null || true
        chmod 755 "$beep" 2>/dev/null || true
        label=$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')
        [ -z "$label" ] || chsmack -e "$label" "$beep" 2>/dev/null || true
    fi
    if [ -x "$beep" ]; then
        for n in 1 2; do
            "$beep" --system --print-reply --reply-timeout=1000 \
                --dest=com.lge.car.micom /com/lge/car/micom \
                com.lge.car.micom.audio.SetBeepPlay array:byte:0x00 >> "$LOG" 2>&1 || true
            sleep 0.5
        done
    fi
    sync
    exit 1
}
: > "$LOG"
rm -f "$RESULT" "$USB_DIR/wrapper_result.txt"
# A stage that dies before writing its result must not leave the previous
# run's per-stage result looking current on the stick.
rm -f "$USB_DIR"/*_result.txt 2>/dev/null || true
[ -f "$USB_DIR/appcombo.tar.gz" ] || fail "missing appcombo.tar.gz"
actual=$(sha256sum "$USB_DIR/appcombo.tar.gz" | cut -d' ' -f1)
[ "$actual" = "$EXPECTED_SHA256" ] || fail "tarball SHA256 mismatch; copy the matching pair"
WORK=$(mktemp -d /tmp/appcombo_run.XXXXXX) || fail "cannot create extraction directory"
tar -xzf "$USB_DIR/appcombo.tar.gz" -C "$WORK" >> "$LOG" 2>&1 || fail "cannot extract tarball"
[ -f "$WORK/run_stuff.sh" ] || fail "missing inner master"
for input in wifi.txt authorized_keys MDNSD_ROOT_ARM; do
    [ ! -f "$USB_DIR/$input" ] || cp -f "$USB_DIR/$input" "$WORK/$input"
done
SCOURGE_MODE="$MODE" USB_DIR="$WORK" COMBO_RW_DIR="$RW_DIR" /bin/sh "$WORK/run_stuff.sh" >> "$LOG" 2>&1
rc=$?
for src in "$WORK"/*_result.txt; do
    [ ! -f "$src" ] || cp -f "$src" "$USB_DIR/$(basename "$src")"
done
for app in battery wifi tweaks; do
    app_rw="${COMBO_RUNTIME_ROOT:-/rw_data}/$app"
    for name in install.log restore.log result.txt status.json; do
        [ ! -f "$app_rw/$name" ] || cp -f "$app_rw/$name" "$USB_DIR/${app}_$name"
    done
done
if [ -f "$RW_DIR/combo.log" ]; then
    cp -f "$RW_DIR/combo.log" "$USB_DIR/combo.log"
    cat "$RW_DIR/combo.log" >> "$LOG"
fi
if [ -f "$WORK/README.txt" ]; then cp -f "$WORK/README.txt" "$USB_DIR/README_COMBO.txt"; fi
status=$(sed -n 's/^status=\([^ ]*\).*/\1/p' "$RESULT" 2>/dev/null | tail -n 1)
verdict=FAIL
if [ "$rc" = 0 ] && [ "$status" = OK ]; then verdict=OK; fi
printf 'COMBO_FLAT_WRAPPER\nstatus=%s\ninner_rc=%s\ntarball_sha256=%s\n' "$verdict" "$rc" "$actual" > "$USB_DIR/wrapper_result.txt"
sync
[ "$verdict" = OK ]
