#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU 260128
# Build-time substitutions bind this loader to one complete tarball and the
# canonical inline cleanup helper definition.
# COMBO_FLAT_WRAPPER_REV=13
EXPECTED_SHA256="@TARBALL_SHA256@"
set -u
PATH="${COMBO_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
USB_DIR="${USB_DIR:-$(CDPATH= cd -- "$(dirname "$0")" && pwd)}"
RESULT="$USB_DIR/run_stuff_result.txt"
LOG="$USB_DIR/appcombo_install.log"
RW_DIR="${COMBO_RW_DIR:-/rw_data/combo}"
RUNTIME_ROOT="${COMBO_RUNTIME_ROOT:-/rw_data}"
QML_BASE="${COMBO_QML_DIR:-/app/share/AppHomeScreen/qml_eco3.0}"
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
    # applies; best-effort only.  Never execute or overwrite through a
    # symlinked helper leaf.
    beep="$RW_DIR/bin/dbus-send"
    beep_safe=0
    if [ -f "$beep" ] && [ ! -L "$beep" ]; then
        beep_safe=1
    elif [ ! -e "$beep" ] && [ ! -L "$beep" ]; then
        mkdir -p "$RW_DIR/bin" 2>/dev/null || true
        helper=$(command -v dbus-send 2>/dev/null || true)
        if [ -n "$helper" ] && [ ! -L "$RW_DIR/bin" ]; then
            cp -f "$helper" "$RW_DIR/bin/.dbus-send.$$" 2>/dev/null &&
                mv -f "$RW_DIR/bin/.dbus-send.$$" "$beep" 2>/dev/null &&
                beep_safe=1
            rm -f "$RW_DIR/bin/.dbus-send.$$" 2>/dev/null || true
        fi
        chmod 755 "$beep" 2>/dev/null || true
        label=$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')
        [ -z "$label" ] || chsmack -e "$label" "$beep" 2>/dev/null || true
    fi
    if [ "$beep_safe" = 1 ] && [ -x "$beep" ]; then
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

# Early output/RW path guard.  This runs before any report/log write so a
# symlinked COMBO_RW_DIR or report path cannot redirect owner data.
unsafe_symlink_path() {
    _usp_path=$1
    case "$_usp_path" in /*) : ;; *) return 1 ;; esac
    case "$_usp_path" in *'/../'*|*/..|*'/./'*|*/.|*'//'*) return 1 ;; esac
    _usp_walk=$_usp_path
    while [ -n "$_usp_walk" ] && [ "$_usp_walk" != "/" ]; do
        [ ! -L "$_usp_walk" ] || return 1
        _usp_walk=$(dirname "$_usp_walk")
    done
    _usp_walk=$(dirname "$_usp_path")
    while [ -n "$_usp_walk" ] && [ "$_usp_walk" != "/" ]; do
        [ ! -e "$_usp_walk" ] || [ -d "$_usp_walk" ] || return 1
        _usp_walk=$(dirname "$_usp_walk")
    done
    return 0
}
unsafe_dir_path() {
    unsafe_symlink_path "$1" || return 1
    [ ! -e "$1" ] || [ -d "$1" ] || return 1
    return 0
}
unsafe_file_path() {
    unsafe_symlink_path "$1" || return 1
    if [ -e "$1" ]; then
        [ -f "$1" ] || return 1
        _uf_links=$(stat -c %h "$1" 2>/dev/null || true)
        case "$_uf_links" in
            ''|*[!0-9]*) return 1 ;;
            1) : ;;
            *) return 1 ;;
        esac
    fi
    return 0
}
# Validate a complete snapshot tree without reading file contents.  Only real
# directories and single-link regular files are allowed; links, FIFOs, sockets,
# devices, unreadable entries and multilink aliases fail closed.
validate_snapshot_tree() {
    _vst=$1
    [ -d "$_vst" ] && [ ! -L "$_vst" ] || return 1
    case "$_vst" in *"
"*|*"	"*) return 1 ;; esac
    # Name validation applies to the root and every file/directory entry,
    # including empty directories and file-free subtrees.  It is separate
    # from the file-only readability/link checks below.
    if ! find "$_vst" -exec sh -c '
        for f do
            case "$f" in *"
"*|*"	"*) exit 1 ;; esac
        done' sh {} + 2>/dev/null; then
        return 1
    fi
    if ! _vst_bad=$(find "$_vst" \
        \( -type l -o -type p -o -type s -o -type b -o -type c \) \
        -print 2>/dev/null); then
        return 1
    fi
    [ -z "$_vst_bad" ] || return 1
    if ! _vst_bad=$(find "$_vst" -type f -links +1 -print 2>/dev/null); then
        return 1
    fi
    [ -z "$_vst_bad" ] || return 1
    if ! find "$_vst" -type f -exec sh -c '
        for f do
            [ -r "$f" ] || exit 1
            h=$(stat -c %h "$f" 2>/dev/null) || exit 1
            [ "$h" = 1 ] || exit 1
            m=$(stat -c %a "$f" 2>/dev/null) || exit 1
            [ $((0$m & 0444)) -ne 0 ] || exit 1
        done' sh {} + 2>/dev/null; then
        return 1
    fi
    return 0
}
_usb_safe=1
unsafe_dir_path "$USB_DIR" || _usb_safe=0
for _usp in "$RESULT" "$LOG" "$USB_DIR/wrapper_result.txt" \
            "$USB_DIR/appcombo.tar.gz" "$USB_DIR/combo.log" \
            "$USB_DIR/README_COMBO.txt"; do
    unsafe_file_path "$_usp" || _usb_safe=0
done
_readback_safe=1
if [ -e "$USB_DIR/qml_readback_$$" ] || [ -L "$USB_DIR/qml_readback_$$" ]; then
    _readback_safe=0
else
    unsafe_dir_path "$USB_DIR/qml_readback_$$" || _readback_safe=0
fi
for _usp_app in battery wifi tweaks wifi1b migrate; do
    for _usp_name in "${_usp_app}_result.txt" "restore_${_usp_app}_result.txt" \
                      "${_usp_app}_install.log" "${_usp_app}_restore.log" \
                      "${_usp_app}_status.json" \
                      "runtime_snapshot_${_usp_app}_status.json"; do
        unsafe_file_path "$USB_DIR/$_usp_name" || _usb_safe=0
    done
done
_rw_safe=1
unsafe_dir_path "$RW_DIR" || _rw_safe=0
unsafe_dir_path "$RW_DIR/bin" || _rw_safe=0
for _usp in "$RW_DIR/combo.log" "$RW_DIR/result.txt" \
            "$RW_DIR/bin/dbus-send"; do
    unsafe_file_path "$_usp" || _rw_safe=0
done
if [ "$_usb_safe" = 0 ] || [ "$_readback_safe" = 0 ] || [ "$_rw_safe" = 0 ]; then
    if [ "$_usb_safe" = 1 ]; then
        printf 'COMBO_FLAT_WRAPPER\nstatus=FAIL\nerror=unsafe_symlink_destination\n' \
            > "$USB_DIR/wrapper_result.txt" 2>/dev/null || true
        cp -f "$USB_DIR/wrapper_result.txt" "$RESULT" 2>/dev/null || true
    else
        printf 'COMBO_FLAT_WRAPPER\nstatus=FAIL\nerror=unsafe_symlink_destination\n' >&2
    fi
    exit 1
fi

# Invalidate only known report names, never an unknown owner file.  This runs
# before any early failure so the current wrapper_result is authoritative.
invalidate_reports() {
    rm -f "$USB_DIR/wrapper_result.txt" "$USB_DIR/run_stuff_result.txt" \
          "$USB_DIR/combo.log" "$USB_DIR/appcombo_install.log" 2>/dev/null || true
    for _ir_app in battery wifi tweaks wifi1b migrate; do
        for _ir_name in "${_ir_app}_result.txt" "restore_${_ir_app}_result.txt" \
                        "${_ir_app}_install.log" "${_ir_app}_restore.log" \
                        "${_ir_app}_status.json" \
                        "runtime_snapshot_${_ir_app}_status.json"; do
            _ir_path="$USB_DIR/$_ir_name"
            if [ -e "$_ir_path" ] || [ -L "$_ir_path" ]; then
                rm -f "$_ir_path" 2>/dev/null || true
            fi
        done
    done
}
invalidate_reports

# Canonical first-party cleanup helper, embedded verbatim at build time.
# The wrapper identity changes with the helper digest; no external helper file
# is required at runtime.  The plain two-file USB pair stays deployable.
RUNTIME_CLEANUP_DIGEST="@RUNTIME_CLEANUP_SHA256@"
SHA256SUM_BIN=sha256sum
command -v "$SHA256SUM_BIN" >/dev/null 2>&1 || SHA256SUM_BIN=sha256sum.coreutils
# @RUNTIME_CLEANUP_SHELL@
_cleanup_id_bad=0
case "$RUNTIME_CLEANUP_DIGEST" in
    ''|*[!0-9a-f]*) _cleanup_id_bad=1 ;;
esac
[ "${#RUNTIME_CLEANUP_DIGEST}" -eq 64 ] || _cleanup_id_bad=1
if [ "$_cleanup_id_bad" = 1 ] ||
   ! command -v cleanup_bounded_logs >/dev/null 2>&1 ||
   ! command -v cleanup_removed_wifi >/dev/null 2>&1; then
    printf 'COMBO_FLAT_WRAPPER\nstatus=FAIL\nerror=unexpanded_cleanup_helper\n' \
        > "$USB_DIR/wrapper_result.txt" 2>/dev/null || true
    cp -f "$USB_DIR/wrapper_result.txt" "$RESULT" 2>/dev/null || true
    exit 1
fi
RUNTIME_CLEANUP_SHA256="$SHA256SUM_BIN"

# Read restore intent before touching owned logs; restore defers bounded log
# cleanup to the Wifi1b stage after its read-only foreign-service gate.  If no
# pre-extract MODE/RESTORE marker exists the wrapper cannot know restore
# intent: bounded product logs are then the only runtime paths this wrapper
# may reclaim before extraction; keys, credentials and backups are never
# touched by that cleanup.
MODE=all
if [ -f "$USB_DIR/MODE" ]; then MODE=$(head -n 1 "$USB_DIR/MODE" | tr -d '\r'); fi
[ ! -f "$USB_DIR/RESTORE" ] || MODE=restore

# FIRST: reclaim bounded runtime log space before any extraction or runtime
# write.  A full /rw_data makes every later write (including this installer's
# own state) fail.  Unknown files, symlinks and foreign paths are left alone.
RUNTIME_CLEANUP_LOG=""
[ "$MODE" = restore ] || cleanup_bounded_logs "$RUNTIME_ROOT" || true

# The tarball is hashed read-only before any recursive deletion.  A corrupt
# or missing bundle must never delete credentials or backups.
if [ ! -f "$USB_DIR/appcombo.tar.gz" ] || [ -L "$USB_DIR/appcombo.tar.gz" ]; then
    fail "missing or unsafe appcombo.tar.gz"
fi
actual=$("$SHA256SUM_BIN" "$USB_DIR/appcombo.tar.gz" | cut -d' ' -f1)
[ "$actual" = "$EXPECTED_SHA256" ] || fail "tarball SHA256 mismatch; copy the matching pair"
# Hash binding alone is not enough: verify the archive is readable and really
# contains the inner master before anything is deleted.
if ! tar -tzf "$USB_DIR/appcombo.tar.gz" >/dev/null 2>&1; then
    fail "tarball unreadable or corrupt; nothing was deleted"
fi
if ! tar -tzf "$USB_DIR/appcombo.tar.gz" 2>/dev/null | grep -Eq '(^|/)run_stuff\.sh$'; then
    fail "tarball has no inner master; nothing was deleted"
fi
_arc_list=$(tar -tzf "$USB_DIR/appcombo.tar.gz" 2>/dev/null) || \
    fail "tarball unreadable or corrupt; nothing was deleted"
[ -n "$_arc_list" ] || fail "tarball is empty; nothing was deleted"
_arc_old_ifs=$IFS
IFS='
'
_arc_bad=0
for _arc_member in $_arc_list; do
    case "$_arc_member" in
        ./*) _arc_member=${_arc_member#./} ;;
    esac
    _arc_member=${_arc_member%/}
    [ -n "$_arc_member" ] || continue
    case "$_arc_member" in
        ''|/*|..|../*|*/../*|*/..|.) _arc_bad=1; break ;;
    esac
    case "/$_arc_member/" in
        *'/./'*|*'//'*) _arc_bad=1; break ;;
    esac
done
IFS=$_arc_old_ifs
[ "$_arc_bad" = 0 ] || fail "tarball member path rejected; nothing was deleted"
_arc_verbose=$(tar -tvzf "$USB_DIR/appcombo.tar.gz" 2>/dev/null) || \
    fail "tarball unreadable or corrupt; nothing was deleted"
[ -n "$_arc_verbose" ] || fail "tarball is empty; nothing was deleted"
_arc_old_ifs=$IFS
IFS='
'
_arc_bad=0
for _arc_line in $_arc_verbose; do
    _arc_modes=$(printf '%s\n' "$_arc_line" | cut -c1-10)
    case "$_arc_modes" in
        -?*|d?*) : ;;
        *) _arc_bad=1; break ;;
    esac
    case "$_arc_modes" in
        ?????w*|????????w*|???[sS]*|??????[sS]*|?????????[tT]*) _arc_bad=1; break ;;
    esac
done
IFS=$_arc_old_ifs
[ "$_arc_bad" = 0 ] || fail "tarball member type/permission rejected; nothing was deleted"

MODE=all
if [ -f "$USB_DIR/MODE" ]; then MODE=$(head -n 1 "$USB_DIR/MODE" | tr -d '\r'); fi
[ ! -f "$USB_DIR/RESTORE" ] || MODE=restore
if [ "$MODE" = all ]; then
    # Full removed-WiFi cleanup is evidence-gated inside the helper: no
    # manual WiFi Off step, no deletion on a missing marker/page alone.
    cleanup_removed_wifi "$RUNTIME_ROOT" "$QML_BASE" "$USB_DIR" \
        "$USB_DIR/appcombo.tar.gz" "$actual" \
        "${CLEANUP_WIFI_SERVICE:-/etc/systemd/system/wifi-daemon.service}" || true
fi
mkdir -p "$RW_DIR" 2>/dev/null || exit 1
: > "$LOG" 2>/dev/null
RUNTIME_CLEANUP_LOG="$LOG"
[ -z "$RUNTIME_CLEANUP_NOTES" ] || printf 'cleanup: %s\n' "$RUNTIME_CLEANUP_NOTES" >> "$LOG" 2>/dev/null

# Known stale report/log names were invalidated before any early failure.
WORK=$(mktemp -d "${TMPDIR:-/tmp}/appcombo_run.XXXXXX") || fail "cannot create extraction directory"
tar -xzf "$USB_DIR/appcombo.tar.gz" -C "$WORK" >> "$LOG" 2>&1 || fail "cannot extract tarball"
[ -f "$WORK/run_stuff.sh" ] || fail "missing inner master"
for input in wifi.txt iphone_wifi.txt authorized_keys MDNSD_ROOT_ARM; do
    [ ! -f "$USB_DIR/$input" ] || cp -f "$USB_DIR/$input" "$WORK/$input"
done
SCOURGE_MODE="$MODE" USB_DIR="$WORK" SCOURGE_STICK_DIR="$USB_DIR" \
    COMBO_RW_DIR="$RW_DIR" COMBO_RUNTIME_ROOT="$RUNTIME_ROOT" \
    COMBO_QML_DIR="$QML_BASE" SCOURGE_ARCHIVE="$USB_DIR/appcombo.tar.gz" \
    SCOURGE_TARBALL_SHA256="$actual" \
    /bin/sh "$WORK/run_stuff.sh" >> "$LOG" 2>&1
rc=$?
for src in "$WORK"/*_result.txt; do
    [ ! -f "$src" ] || cp -f "$src" "$USB_DIR/$(basename "$src")"
done
readback_root="$USB_DIR/qml_readback_$$"
readback_ready=0
readback_failed=0
readback_invalid=0
for app in battery wifi tweaks wifi1b migrate; do
    app_rw="${COMBO_RUNTIME_ROOT:-/rw_data}/$app"
    # Stage result files above are authoritative. Never overwrite them with
    # possibly historical /rw_data/result.txt after an early stage failure.
    action=install
    stage_name=$app
    if [ "$MODE" = restore ]; then action=restore; stage_name=restore_$app; fi
    if [ -f "$WORK/${stage_name}_result.txt" ]; then
        [ ! -f "$app_rw/$action.log" ] || cp -f "$app_rw/$action.log" "$USB_DIR/${app}_$action.log"
    fi
done
# Prevalidate every app/phase snapshot before copying even the first one.
for app in battery wifi tweaks wifi1b migrate; do
    for phase in before after; do
        snapshot="$WORK/combo/$app/qml_$phase"
        [ -e "$snapshot" ] || [ -L "$snapshot" ] || continue
        validate_snapshot_tree "$snapshot" || readback_invalid=1
    done
done
if [ "$readback_invalid" = 0 ]; then
    for app in battery wifi tweaks wifi1b migrate; do
        for phase in before after; do
            snapshot="$WORK/combo/$app/qml_$phase"
            [ -e "$snapshot" ] || [ -L "$snapshot" ] || continue
            if ! validate_snapshot_tree "$snapshot"; then
                readback_failed=1
                continue
            fi
            if [ "$readback_ready" = 0 ]; then
                if [ -e "$readback_root" ] || [ -L "$readback_root" ] ||
                   ! mkdir "$readback_root" 2>/dev/null; then
                    readback_failed=1
                    break 2
                fi
                readback_ready=1
            fi
            app_dir="$readback_root/$app"
            if [ ! -d "$app_dir" ] || [ -L "$app_dir" ]; then
                mkdir "$app_dir" 2>/dev/null || { readback_failed=1; continue; }
            fi
            dest="$app_dir/qml_$phase"
            if [ -e "$dest" ] || [ -L "$dest" ] || ! mkdir "$dest" 2>/dev/null; then
                readback_failed=1
                continue
            fi
            cp -R "$snapshot/." "$dest/" || { readback_failed=1; continue; }
        done
    done
else
    readback_failed=1
fi
if [ "$readback_failed" = 1 ] && [ -d "$readback_root" ] &&
   [ ! -L "$readback_root" ]; then
    printf 'partial readback: validated product snapshots only\n' \
        > "$readback_root/README.partial" 2>/dev/null || true
fi
if [ -f "$RW_DIR/combo.log" ]; then
    cp -f "$RW_DIR/combo.log" "$USB_DIR/combo.log"
    cat "$RW_DIR/combo.log" >> "$LOG"
fi
if [ -f "$WORK/README.txt" ]; then cp -f "$WORK/README.txt" "$USB_DIR/README_COMBO.txt"; fi
status=$(sed -n 's/^status=\([^ ]*\).*/\1/p' "$RESULT" 2>/dev/null | tail -n 1)
verdict=FAIL
if [ "$rc" = 0 ] && [ "$status" = OK ] && [ "$readback_failed" = 0 ]; then verdict=OK; fi
printf 'COMBO_FLAT_WRAPPER\nstatus=%s\ninner_rc=%s\ntarball_sha256=%s\ncleanup_helper_sha256=%s\n' "$verdict" "$rc" "$actual" "$RUNTIME_CLEANUP_DIGEST" > "$USB_DIR/wrapper_result.txt"
sync
[ "$verdict" = OK ]
