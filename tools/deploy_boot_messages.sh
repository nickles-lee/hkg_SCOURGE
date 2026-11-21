#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# deploy_boot_messages.sh — install a staged set of patched AppStandbyClock
# .qm files over SSH on a rooted car.
#
# Runs ON THE CAR as root (the wifi1b account is UID 0). The caller stages
# translation_*.qm plus MANIFEST.sha256 under $STAGE (default
# /rw_data/scourge/bootmsg.new), then runs:
#
#   STAGE=/rw_data/scourge/bootmsg.new sh /rw_data/scourge/deploy_boot_messages.sh
#
# The script verifies every staged hash and size against the live file before
# writing anything, remounts / read-write only for the copy, backs up the old
# files under /rw_data/scourge/, installs by atomic rename, syncs, restores
# the original mount mode, and rolls back on any failure. It never reboots.
#
# TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).
set -eu

STAGE="${STAGE:-/rw_data/scourge/bootmsg.new}"
TARGET="${TARGET:-/app/share/AppStandbyClock/localization/qm}"
BACKUP_ROOT="${BACKUP_ROOT:-/rw_data/scourge}"
EXPECTED_COUNT="${EXPECTED_COUNT:-}"
MOUNTS_FILE="${MOUNTS_FILE:-/proc/mounts}"
MOUNT_BIN="${MOUNT_BIN:-mount}"
SYNC_BIN="${SYNC_BIN:-sync}"

status=0
root_opts=""
backup_dir=""
installed=""

log() { echo "bootmsg: $*"; }
die() { status=1; log "ERROR: $*"; exit 1; }
on_signal() { status=1; log "interrupted"; exit 1; }

read_root_opts() {
    while read -r _dev mnt _fstype opts _rest; do
        if [ "$mnt" = "/" ]; then
            root_opts=$opts
            return 0
        fi
    done < "$MOUNTS_FILE"
    return 1
}

restore_root_mode() {
    case ",${root_opts}," in
        *,ro,*) "$MOUNT_BIN" -o remount,ro / >/dev/null 2>&1 ||
            log "WARNING: remount ro failed" ;;
    esac
}

read_label() {
    if command -v getfattr >/dev/null 2>&1; then
        value=$(getfattr -n security.SMACK64 --only-values "$1" 2>/dev/null) &&
            { printf '%s' "$value"; return 0; }
    fi
    if command -v chsmack >/dev/null 2>&1; then
        out=$(chsmack "$1" 2>/dev/null) &&
            { printf '%s' "$out" |
                sed 's/^security\.SMACK64=//; s/^[^=]*: //; s/^"//; s/"$//'; return 0; }
    fi
    return 1
}

apply_label() {
    [ -n "$2" ] || return 0
    if command -v setfattr >/dev/null 2>&1; then
        setfattr -n security.SMACK64 -v "$2" "$1" 2>/dev/null && return 0
    fi
    if command -v chsmack >/dev/null 2>&1; then
        chsmack -a "$2" "$1" 2>/dev/null && return 0
    fi
    return 1
}

rollback() {
    [ -n "$backup_dir" ] || return 0
    for name in $installed; do
        [ -f "$backup_dir/$name" ] || continue
        cp -p "$backup_dir/$name" "$TARGET/$name" 2>/dev/null || true
        label=$(cat "$backup_dir/$name.label" 2>/dev/null || true)
        apply_label "$TARGET/$name" "$label" || true
    done
    "$SYNC_BIN" 2>/dev/null || true
}

cleanup() {
    rc=$?
    if [ "$status" -ne 0 ] || [ "$rc" -ne 0 ]; then
        log "rolling back installed files"
        rollback
    fi
    rm -f "$TARGET"/*.qm.new 2>/dev/null || true
    restore_root_mode
}
trap on_signal INT TERM HUP
trap cleanup EXIT

[ -d "$STAGE" ] || die "stage directory missing: $STAGE"
[ -f "$STAGE/MANIFEST.sha256" ] || die "manifest missing: $STAGE/MANIFEST.sha256"
[ -d "$TARGET" ] || die "target directory missing: $TARGET"

read_root_opts || die "cannot find / in /proc/mounts"
log "root mount options: $root_opts"

count=0
names=""
while read -r want name; do
    [ -n "$want" ] || continue
    case "$name" in
        translation_*.qm) ;;
        *) die "unexpected staged name: $name" ;;
    esac
    case "$name" in
        */*) die "path separators are not allowed: $name" ;;
    esac
    count=$((count + 1))
    names="$names $name"
    [ -f "$STAGE/$name" ] || die "staged file missing: $name"
    [ -f "$TARGET/$name" ] || die "live file missing: $name"
    got=$(sha256sum "$STAGE/$name" | cut -d' ' -f1)
    [ "$got" = "$want" ] || die "hash mismatch for $name: $got != $want"
    staged_size=$(stat -c%s "$STAGE/$name")
    live_size=$(stat -c%s "$TARGET/$name")
    [ "$staged_size" = "$live_size" ] ||
        die "size mismatch for $name: $staged_size != $live_size"
done < "$STAGE/MANIFEST.sha256"

[ "$count" -gt 0 ] || die "manifest is empty"
if [ -n "$EXPECTED_COUNT" ] && [ "$count" -ne "$EXPECTED_COUNT" ]; then
    die "expected $EXPECTED_COUNT staged files, found $count"
fi
log "verified $count staged file(s)"

case ",${root_opts}," in
    *,rw,*) ;;
    *) "$MOUNT_BIN" -o remount,rw / || die "remount rw failed" ;;
esac

backup_dir="$BACKUP_ROOT/bootmsg-backup-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$backup_dir" || die "cannot create backup dir: $backup_dir"
log "backup: $backup_dir"

for name in $names; do
    if live_label=$(read_label "$TARGET/$name"); then
        :
    elif command -v getfattr >/dev/null 2>&1 ||
         command -v chsmack >/dev/null 2>&1; then
        die "cannot read SMACK label: $name"
    else
        live_label=""
    fi
    printf '%s\n' "$live_label" > "$backup_dir/$name.label" ||
        die "cannot record label: $name"
    cp -p "$TARGET/$name" "$backup_dir/$name" || die "backup failed: $name"
    apply_label "$backup_dir/$name" "$live_label" ||
        die "backup label failed: $name"
done
"$SYNC_BIN" || die "sync failed"

for name in $names; do
    live_label=$(cat "$backup_dir/$name.label" 2>/dev/null || true)
    cp "$STAGE/$name" "$TARGET/$name.new" || die "stage copy failed: $name"
    chmod 644 "$TARGET/$name.new" || die "chmod failed: $name"
    chown 0:0 "$TARGET/$name.new" || die "chown failed: $name"
    apply_label "$TARGET/$name.new" "$live_label" ||
        die "label apply failed: $name"
    installed="$installed $name"
    mv -f "$TARGET/$name.new" "$TARGET/$name" || die "install failed: $name"
done

"$SYNC_BIN" || die "sync failed"
for name in $names; do
    want=$(sha256sum "$STAGE/$name" | cut -d' ' -f1)
    got=$(sha256sum "$TARGET/$name" | cut -d' ' -f1)
    [ "$got" = "$want" ] || die "post-install hash mismatch: $name"
done

log "installed $count boot-message .qm file(s); reboot to show the new text"
log "rollback files: $backup_dir"
