#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L AU 260319 (layout bases from the car snapshot of 2026-11-27)
# PROJECTION_PAYLOAD_REV=1
# Projection+ installer.  WIP: car-run once on one AU 260319 car (install, tile, status); restore VM-only.
#
# Two ways to run it (both as root, SMACK label "_"):
#   USB autorun:  copy build/usb/ to the root of a stick; the initial-root
#                 failsafe runs this file with USB_DIR set.  An empty file
#                 named DRY_RUN next to it turns the run into a dry run.
#   SSH:          upload build/usb/ to a directory on the car, then
#                   sh run_stuff.sh --from-dir DIR [--dry-run]
#
# Installs (every file is listed with its sha256 in manifest.txt):
#   /app/share/AppHomeScreen/qml_eco3.0/...   page, components, patched layouts
#   /app/share/AppHomeScreen/projection/*.png icons
#   /rw_data/projection/projection_daemon     (SMACK64EXEC "_")
#   /etc/systemd/system/projection-daemon.service (enabled, restarted)
#   /rw_data/projection/tools/                restore script + its libraries
#
# Gates, all checked before anything on the car is written:
#   every payload file matches its manifest sha256;
#   every layout file on the car matches the manifest base sha256 (or is
#   already the installed payload); new files are absent or ours;
#   the installer runs as root with SMACK label "_".
# Originals are backed up to /rw_data/projection/backup/<stamp>/ (+ latest)
# before each write; a failure rolls back, an interruption is undone by
# run_stuff_restore.sh.  Only projection-daemon and AppHomeScreen are
# restarted.  /rw_data/hudnav (the HUD Nav kit) is never written.
set -u
umask 022
PATH="${PROJECTION_PATH:-/bin:/sbin:/usr/bin:/usr/sbin}"
# Test-only prefix for every car path (fake root); empty on the car.
R="${PROJECTION_FAKE_ROOT:-}"

usage() {
    echo "usage: sh run_stuff.sh [--from-dir DIR] [--dry-run] [--restart-ui]"
}

DRY_RUN=0
FROM_DIR=""
while [ $# -gt 0 ]; do
    case "$1" in
        --from-dir) [ $# -ge 2 ] || { usage; exit 2; }; FROM_DIR=$2; shift 2 ;;
        --from-dir=*) FROM_DIR=${1#--from-dir=}; shift ;;
        --restart-ui) PROJECTION_RESTART_UI=1; export PROJECTION_RESTART_UI; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done
[ "${PROJECTION_DRY_RUN:-0}" = 1 ] && DRY_RUN=1

if [ -n "$FROM_DIR" ]; then
    MODE=ssh
    SRC=$(cd "$FROM_DIR" 2>/dev/null && pwd) || { echo "status=FAIL error=cannot enter $FROM_DIR"; exit 1; }
elif [ -n "${USB_DIR:-}" ]; then
    MODE=usb
    SRC=$USB_DIR
    mount -o remount,rw "$USB_DIR" 2>/dev/null || true
else
    MODE=ssh
    SRC=$(cd "$(dirname "$0")" 2>/dev/null && pwd) || { echo "status=FAIL error=cannot find payload dir"; exit 1; }
fi
[ -f "$SRC/DRY_RUN" ] && DRY_RUN=1

PAYLOAD="$SRC/payload"
MANIFEST="$SRC/manifest.txt"
QML_ROOT="$R/app/share/AppHomeScreen"
QML_DIR="$QML_ROOT/qml_eco3.0"
REF_QML="$QML_DIR/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
RW_DIR="$R/rw_data/projection"
HUDNAV_DIR="$R/rw_data/hudnav"
UNIT=projection-daemon.service
SERVICE="$R/etc/systemd/system/$UNIT"
OUT="$SRC/run_stuff_result.txt"
LATEST="$RW_DIR/backup/latest"

# Until the gates pass nothing is written on the car: the log lives next to
# the payload (or in /tmp when that is read-only) and moves to $RW_DIR later.
LOG="$SRC/projection_install.log"
{ : > "$LOG"; } 2>/dev/null || { LOG="/tmp/projection_install.log"; : > "$LOG" 2>/dev/null; }

log() {
    echo "$(date -u 2>/dev/null) $*" >> "$LOG" 2>/dev/null
    echo "$*"
}

result() {
    {
        echo "PROJECTION_PAYLOAD"
        echo "time=$(date -u 2>/dev/null)"
        echo "mode=$MODE"
        echo "src=$SRC"
        echo "dry_run=$DRY_RUN"
        echo "uid=$(id -u 2>/dev/null)"
        for line in "$@"; do echo "$line"; done
        echo "log=$LOG"
    } > "$OUT.tmp" 2>/dev/null
    mv -f "$OUT.tmp" "$OUT" 2>/dev/null || true
    if [ "$DRY_RUN" != 1 ] && [ -d "$RW_DIR" ]; then
        cp -f "$OUT" "$RW_DIR/result.txt" 2>/dev/null || true
    fi
    echo "---- result ----"
    cat "$OUT" 2>/dev/null || for line in "$@"; do echo "$line"; done
}

TXN_ACTIVE=0
UNIT_STARTED=0
IN_FAIL=0
fail() {
    [ "$IN_FAIL" = 0 ] || exit 1
    IN_FAIL=1
    log "FAIL: $*"
    rollback=none
    if [ "$TXN_ACTIVE" = 1 ]; then
        log "rolling back incomplete install from $BK"
        if [ "$UNIT_STARTED" = 1 ] && command -v systemctl >/dev/null 2>&1; then
            systemctl stop "$UNIT" >> "$LOG" 2>&1 || true
            systemctl disable "$UNIT" >> "$LOG" 2>&1 || true
        fi
        if txn_restore "$BK"; then
            txn_set_state "$BK" restored
            rollback=ok
        else
            rollback="FAILED (run run_stuff_restore.sh; backup $BK kept)"
        fi
        command -v systemctl >/dev/null 2>&1 && systemctl daemon-reload >> "$LOG" 2>&1 || true
        TXN_ACTIVE=0
    fi
    result "status=FAIL" "error=$*" "rollback=$rollback"
    exit 1
}

log "=== projection payload start mode=$MODE src=$SRC dry_run=$DRY_RUN ==="

# ---- tools -----------------------------------------------------------------
# BusyBox may ship the coreutils hashers only under *.coreutils names.
MD5SUM_BIN=md5sum
command -v md5sum >/dev/null 2>&1 || MD5SUM_BIN=md5sum.coreutils
SHA256SUM_BIN=sha256sum
command -v sha256sum >/dev/null 2>&1 || SHA256SUM_BIN=sha256sum.coreutils
command -v "$SHA256SUM_BIN" >/dev/null 2>&1 || fail "sha256sum not found"
export MD5SUM_BIN SHA256SUM_BIN
sha() { "$SHA256SUM_BIN" "$1" 2>/dev/null | cut -d' ' -f1; }

if [ "$(id -u 2>/dev/null)" != 0 ] && [ "${PROJECTION_ALLOW_NONROOT:-0}" != 1 ]; then
    fail "must run as root"
fi

# ---- gate 1: payload present and every file matches the manifest -----------
[ -f "$MANIFEST" ] || fail "manifest.txt missing in $SRC"
[ -d "$PAYLOAD" ] || fail "payload directory missing: $PAYLOAD"

is_sha() {
    [ "${#1}" -eq 64 ] || return 1
    case "$1" in *[!0-9a-f]*) return 1 ;; esac
    return 0
}

# Manifest path -> destination on the car.  Anything else is refused, which
# also keeps every write out of /rw_data/hudnav.
dest_for() {
    case "$1" in
        *..*|/*|*'|'*|*' '*|*//*) return 1 ;;
        qml_eco3.0/*.qml) echo "$QML_ROOT/$1" ;;
        projection/*.png) echo "$QML_ROOT/$1" ;;
        bin/projection_daemon) echo "$RW_DIR/projection_daemon" ;;
        systemd/projection-daemon.service) echo "$SERVICE" ;;
        tools/install_io.sh|tools/projection_txn.sh|tools/run_stuff_restore.sh) echo "$RW_DIR/$1" ;;
        *) return 1 ;;
    esac
}

n_entries=0
while read -r rel base pay extra; do
    case "$rel" in ''|'#'*) continue ;; esac
    [ -n "$pay" ] && [ -z "$extra" ] || fail "malformed manifest line: $rel"
    is_sha "$pay" || fail "bad payload sha in manifest: $rel"
    [ "$base" = new ] || is_sha "$base" || fail "bad base sha in manifest: $rel"
    dest=$(dest_for "$rel") || fail "unsupported manifest path: $rel"
    case "$dest" in "$HUDNAV_DIR"|"$HUDNAV_DIR"/*) fail "refusing to write into $HUDNAV_DIR: $rel" ;; esac
    case "$rel" in
        qml_eco3.0/*) : ;;
        *) [ "$base" = new ] || fail "only qml_eco3.0 files may replace a base: $rel" ;;
    esac
    [ -f "$PAYLOAD/$rel" ] || fail "payload file missing: $rel"
    [ "$(sha "$PAYLOAD/$rel")" = "$pay" ] || fail "payload file corrupt (sha256 mismatch): $rel"
    n_entries=$((n_entries + 1))
done < "$MANIFEST"
[ "$n_entries" -gt 0 ] || fail "manifest.txt lists no files"
for req in bin/projection_daemon systemd/projection-daemon.service \
           tools/install_io.sh tools/projection_txn.sh tools/run_stuff_restore.sh; do
    grep -q "^$req " "$MANIFEST" || fail "manifest lacks required entry: $req"
done
log "manifest OK ($n_entries files, sha256 verified)"

# Only now are the shipped libraries trusted enough to source.
. "$PAYLOAD/tools/install_io.sh"
. "$PAYLOAD/tools/projection_txn.sh"

# ---- gate 2: SMACK ---------------------------------------------------------
# Same rule as Tweaks: the daemon is stamped with the installer's own floor
# label "_" and the installer refuses any other context.  Over SSH the shell
# also runs as "_" (car readback 2026-11-27), so the rule is identical.
smack_init() {
    if [ -n "${PROJECTION_SMACK_SELF:-}" ]; then
        SMACK_SELF=$PROJECTION_SMACK_SELF
        log "SMACK: using test override label '$SMACK_SELF'"
    else
        grep -q ' smackfs ' /proc/mounts 2>/dev/null || fail "smackfs is not mounted (SMACK is required)"
        command -v chsmack >/dev/null 2>&1 || fail "chsmack missing (SMACK is required)"
        SMACK_SELF=$(cat /proc/self/attr/current 2>/dev/null | tr -d '\r\n')
        [ -n "$SMACK_SELF" ] || fail "cannot read /proc/self/attr/current"
    fi
    [ "$SMACK_SELF" = "_" ] || fail "expected SMACK floor label '_', got '$SMACK_SELF' (run from the USB failsafe or a root SSH shell)"
    log "SMACK: installer label '$SMACK_SELF'"
}
smack_init

set_exec_label() {
    _bin=$1
    command -v chsmack >/dev/null 2>&1 || fail "chsmack missing (SMACK is required)"
    chsmack -e "$SMACK_SELF" "$_bin" >> "$LOG" 2>&1 || fail "exec label failed: $_bin"
    _actual=$(getfattr -n security.SMACK64EXEC --only-values "$_bin" 2>/dev/null || true)
    # Unreadable is a warning (car-proven onlycap behaviour, see Tweaks); a
    # readable wrong label is fatal; the running label is checked at the end.
    if [ -z "$_actual" ]; then
        log "WARNING: cannot read back SMACK64EXEC for $_bin; trusting chsmack"
    elif [ "$_actual" != "$SMACK_SELF" ]; then
        fail "exec label readback failed: $_bin (expected '$SMACK_SELF', got '$_actual')"
    fi
    log "exec label ok: $_bin = ${_actual:-unverified}"
}

# ---- gate 3: the car's files ----------------------------------------------
[ -d "$QML_DIR" ] || fail "expected QML directory missing: $QML_DIR"
[ -f "$REF_QML" ] || fail "reference layout missing: $REF_QML"

ACTIVE=""
if [ -d "$LATEST" ]; then
    case "$(txn_state "$LATEST")" in
        pending|complete) ACTIVE=$(cd "$LATEST" && pwd -P) ;;
    esac
fi

# Plan: one line per transactional entry, "action|rel|dest|base|pay".
PLAN="${TMPDIR:-/tmp}/projection-plan.$$"
: > "$PLAN" || fail "cannot write plan file $PLAN"
trap 'rm -f "$PLAN"' EXIT
n_change=0
n_same=0
while read -r rel base pay extra; do
    case "$rel" in ''|'#'*|tools/*) continue ;; esac
    dest=$(dest_for "$rel")
    if [ -e "$dest" ]; then cur=$(sha "$dest"); else cur=absent; fi
    entry=""
    [ -z "$ACTIVE" ] || entry=$(txn_lookup "$ACTIVE" "$rel")
    rec_existed=$(printf '%s' "$entry" | cut -d'|' -f2)
    rec_sha=$(printf '%s' "$entry" | cut -d'|' -f4)
    if [ "$base" != new ]; then
        if [ -n "$entry" ] && { [ "$rec_existed" != 1 ] || [ "$rec_sha" != "$base" ]; }; then
            fail "backup $ACTIVE records $rel with sha ${rec_sha:-none}, not the manifest base; run run_stuff_restore.sh first"
        fi
        if [ "$cur" = "$pay" ]; then
            [ -n "$entry" ] || fail "$rel is already patched but no backup holds its original; refusing (no restore point)"
            action=same
        elif [ "$cur" = "$base" ]; then
            action=install
        elif [ "$cur" = absent ]; then
            fail "layout file missing on the car: $dest"
        else
            fail "layout file $rel does not match the expected base (car sha256=$cur, expected $base). Other packages changed it, or another Projection+ build is installed: run run_stuff_restore.sh first."
        fi
    else
        if [ "$cur" = "$pay" ]; then
            if [ -n "$entry" ]; then action=same; else action=adopt; fi
        elif [ "$cur" = absent ]; then
            action=install
        elif [ -n "$entry" ] && [ "$rec_existed" = 0 ]; then
            action=upgrade
        else
            fail "unexpected existing file at $dest (sha256=$cur); not installed by Projection+, refusing to overwrite"
        fi
    fi
    case "$action" in same) n_same=$((n_same + 1)) ;; *) n_change=$((n_change + 1)) ;; esac
    printf '%s|%s|%s|%s|%s\n' "$action" "$rel" "$dest" "$base" "$pay" >> "$PLAN"
    log "plan: $action $rel"
done < "$MANIFEST"
log "gates OK: change=$n_change unchanged=$n_same"

if [ "$DRY_RUN" = 1 ]; then
    log "dry run: nothing written to the car"
    result "status=DRY_RUN" "would_change=$n_change" "unchanged=$n_same" \
           "backup=$( [ -n "$ACTIVE" ] && echo "reuse $ACTIVE" || { [ "$n_change" -gt 0 ] && echo new || echo none; } )" \
           "would_restart=$UNIT" "then=reboot the head unit"
    exit 0
fi

# ---- from here on the car is written --------------------------------------
mkdir -p "$RW_DIR/backup" || fail "cannot create $RW_DIR"
cat "$LOG" >> "$RW_DIR/install.log" 2>/dev/null
[ "$LOG" = "$RW_DIR/install.log" ] || rm -f "$LOG" 2>/dev/null
LOG="$RW_DIR/install.log"

io_init || fail "cannot identify root mount"
trap 'rm -f "$PLAN"; io_restore_root || true' EXIT
trap 'fail "interrupted (SIGINT)"' INT
trap 'fail "interrupted (SIGTERM/SIGHUP)"' TERM HUP
io_settle_root || fail "root filesystem is not stably writable"

# Restore tooling first, so an interrupted install can always be undone with
# sh /rw_data/projection/tools/run_stuff_restore.sh.
io_mkdir "$RW_DIR/tools" || fail "cannot create $RW_DIR/tools"
for t in install_io.sh projection_txn.sh run_stuff_restore.sh; do
    io_copy "$PAYLOAD/tools/$t" "$RW_DIR/tools/$t" || fail "tool copy failed: $t"
    chmod 644 "$RW_DIR/tools/$t" 2>/dev/null || true
done

BK=""
if [ "$n_change" -gt 0 ]; then
    if [ -n "$ACTIVE" ]; then
        BK=$ACTIVE
        log "reusing backup $BK (state $(txn_state "$BK"))"
    else
        STAMP=$(date -u +%Y%m%dT%H%M%SZ 2>/dev/null)
        [ -n "$STAMP" ] || STAMP=manual
        BK="$RW_DIR/backup/$STAMP"
        [ -e "$BK" ] && BK="$BK.$$"
        mkdir -p "$BK/files" || fail "cannot create $BK"
        log "new backup $BK"
    fi
    txn_set_state "$BK" pending || fail "cannot write backup state"
    ln -sfn "$BK" "$LATEST" || fail "cannot point backup/latest at $BK"
    TXN_ACTIVE=1
fi

label_of() {
    getfattr -n security.SMACK64 --only-values "$1" 2>/dev/null || true
}

install_entry() {
    action=$1; rel=$2; dest=$3; pay=$4
    if [ "$action" = adopt ]; then
        # Our exact payload is already there but unrecorded: record it as
        # "absent before install" so restore removes it.
        printf '%s|0|%s||||%s\n' "$rel" "$dest" "$pay" >> "$BK/index" || fail "cannot record $rel"
        sync
        log "adopted: $rel"
        return 0
    fi
    if [ -e "$dest" ]; then label=$(label_of "$dest"); else label=$(label_of "$REF_QML"); fi
    txn_remember "$BK" "$rel" "$dest" "$pay" || fail "backup failed: $dest"
    txn_mkdir "$BK" "$(dirname "$dest")" || fail "cannot create dir for $dest"
    case "$rel" in
        bin/*)
            # Remove first: cp onto an existing inode keeps a stale SMACK64EXEC.
            io_remove "$dest" || fail "cannot replace $dest"
            io_copy "$PAYLOAD/$rel" "$dest" || fail "copy failed: $dest"
            chmod 755 "$dest" 2>/dev/null || true
            chown 0:0 "$dest" 2>/dev/null || true
            set_exec_label "$dest"
            ;;
        systemd/*)
            io_copy "$PAYLOAD/$rel" "$dest" || fail "copy failed: $dest"
            chmod 644 "$dest" 2>/dev/null || true
            chown 0:0 "$dest" 2>/dev/null || true
            UNIT_STARTED=1
            ;;
        *)
            io_copy "$PAYLOAD/$rel" "$dest" || fail "copy failed: $dest"
            chmod 644 "$dest" 2>/dev/null || true
            chown 0:0 "$dest" 2>/dev/null || true
            [ -z "$label" ] || setfattr -n security.SMACK64 -v "$label" "$dest" 2>/dev/null || true
            ;;
    esac
    [ "$(sha "$dest")" = "$pay" ] || fail "installed file does not verify: $dest"
    log "installed: $rel"
    # @TEST_INTERRUPT_POINT
}

# Order: new QML (pages/components), icons, daemon, unit, then the layouts
# that reference them, so a half-done install never points at missing files.
n_qml=0
for pass in qmlnew png bin unit layout; do
    while IFS='|' read -r action rel dest base pay; do
        case "$pass:$rel:$base" in
            qmlnew:qml_eco3.0/*:new|png:projection/*|bin:bin/*|unit:systemd/*) : ;;
            layout:qml_eco3.0/*) [ "$base" != new ] || continue ;;
            *) continue ;;
        esac
        [ "$action" != same ] || continue
        install_entry "$action" "$rel" "$dest" "$pay"
        case "$rel" in qml_eco3.0/*|projection/*) n_qml=$((n_qml + 1)) ;; esac
    done < "$PLAN"
done

# The binary is ours either way; re-assert its exec label on every run.
[ "$n_change" -gt 0 ] || set_exec_label "$RW_DIR/projection_daemon"
DAEMON_SMACK=$(getfattr -n security.SMACK64EXEC --only-values "$RW_DIR/projection_daemon" 2>/dev/null || true)
[ -n "$DAEMON_SMACK" ] || DAEMON_SMACK=unverified

if [ -n "$BK" ]; then
    cp -f "$MANIFEST" "$BK/manifest.txt" 2>/dev/null || true
    txn_set_state "$BK" complete || fail "cannot mark backup complete"
fi
TXN_ACTIVE=0
cp -f "$MANIFEST" "$RW_DIR/installed.manifest" 2>/dev/null || true
log "files done: changed=$n_change qml_changed=$n_qml backup=${BK:-none}"

if [ "$n_qml" -gt 0 ]; then
    rm -rf "$R/root/.cache/AppHomeScreen" "$R/root/.cache/qtshadercache" \
           "$R/tmp/qmlcache" "$R/tmp/qt-qml-cache" 2>/dev/null || true
    log "QML caches cleared"
fi
sync

# ---- daemon ----------------------------------------------------------------
daemon_status="not-started"
if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >> "$LOG" 2>&1 || true
    if systemctl enable "$UNIT" >> "$LOG" 2>&1; then
        if systemctl restart "$UNIT" >> "$LOG" 2>&1; then
            daemon_status="systemd-ok"
        else
            daemon_status="systemd-restart-failed"
        fi
    else
        daemon_status="systemd-enable-failed"
    fi
fi
if [ "$daemon_status" != "systemd-ok" ] && [ -z "$R" ]; then
    if [ -x "$RW_DIR/projection_daemon" ] && ! pidof projection_daemon >/dev/null 2>&1; then
        ( "$RW_DIR/projection_daemon" --port 8092 >> "$RW_DIR/daemon.log" 2>&1 & ) 2>/dev/null || true
        sleep 1
    fi
    pidof projection_daemon >/dev/null 2>&1 && daemon_status="$daemon_status+manual"
fi
log "daemon=$daemon_status"

io_restore_root || fail "cannot restore root mount state"

# ---- AppHomeScreen (only when its QML changed) -----------------------------
if [ "$n_qml" -gt 0 ]; then
    restart_homescreen
else
    restart="skipped-unchanged"
fi
log "restart=$restart"
sync

# ---- verify the running daemon's SMACK label -------------------------------
# /proc/<pid>/comm is truncated to 15 characters: "projection_daem".
RUNNING_SMACK="${PROJECTION_DAEMON_SMACK:-}"
if [ -z "$RUNNING_SMACK" ]; then
    for _try in 1 2 3 4 5; do
        for p in /proc/[0-9]*; do
            [ -r "$p/comm" ] || continue
            case "$(cat "$p/comm" 2>/dev/null)" in
                projection_daem*) RUNNING_SMACK=$(cat "$p/attr/current" 2>/dev/null | tr -d '\r\n'); break ;;
            esac
        done
        [ -n "$RUNNING_SMACK" ] && break
        sleep 1
    done
fi
if [ -z "$RUNNING_SMACK" ]; then
    smack_check=daemon-not-running
elif [ "$RUNNING_SMACK" = "$SMACK_SELF" ]; then
    smack_check=ok
else
    smack_check=mismatch
fi
case "$DAEMON_SMACK" in "$SMACK_SELF"|unverified) : ;; *) smack_check=mismatch ;; esac
log "smack_check=$smack_check file=$DAEMON_SMACK running=${RUNNING_SMACK:-none}"

final_status=OK
[ "$smack_check" = ok ] || final_status=FAIL
case "$daemon_status" in systemd-ok|*+manual) : ;; *) final_status=FAIL ;; esac

result "status=$final_status" "changed=$n_change" "unchanged=$n_same" "qml_changed=$n_qml" \
       "backup=${BK:-${ACTIVE:-none}}" "daemon=$daemon_status" "restart=$restart" \
       "smack_self=$SMACK_SELF" "daemon_file_smack=$DAEMON_SMACK" \
       "daemon_smack=${RUNNING_SMACK:-none}" "smack_check=$smack_check" \
       "restore=sh $RW_DIR/tools/run_stuff_restore.sh"
log "=== projection payload done status=$final_status ==="
[ "$restart" = reboot-required ] && [ "$final_status" = OK ] && \
    log "NEXT: reboot the head unit (Reboot tile) to load the new home screen."
[ "$final_status" = OK ] || exit 1
exit 0
