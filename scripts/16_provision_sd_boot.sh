#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# 16_provision_sd_boot.sh — put the rig's first-boot script and hook on an
# already-flashed SD card, without re-flashing the image.
#
#   scripts/16_provision_sd_boot.sh [--card /dev/sdb] [--secrets FILE] [--dry-run]
#
# Reads the secrets from config/rig.local.env (gitignored, see the .example),
# renders scripts/firstrun.sh.tmpl into it, installs the result on the card's
# boot partition, and adds the systemd.run hook plus the WiFi regulatory domain
# to cmdline.txt. The card is left unmounted, as it was found.
#
# This is the "startup script goes into the flashing" step: 13_copy_image_to_sd.sh
# writes the 27 GB payload image, this one makes an image of Raspberry Pi OS
# boot itself headless.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/lib.sh"

TMPL="$HERE/firstrun.sh.tmpl"
SECRETS="$SCOURGE_ROOT/config/rig.local.env"
LABEL="${PI_BOOTFS_LABEL:-bootfs}"
CARD=""
DRY_RUN=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --card) CARD="${2:?--card needs a device}"; shift 2 ;;
        --secrets) SECRETS="${2:?--secrets needs a path}"; shift 2 ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) sed -n '4,15p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ -f "$TMPL" ] || { echo "missing $TMPL" >&2; exit 1; }
[ -f "$SECRETS" ] || {
    echo "missing $SECRETS — copy config/rig.local.env.example and fill it in" >&2
    exit 1
}
# shellcheck disable=SC1090
source "$SECRETS"

missing=""
for v in RIG_HOSTNAME RIG_USER RIG_PASSWORD SSH_PUBKEY WIFI_COUNTRY WIFI_SSIDS WIFI_PSK; do
    [ -n "${!v:-}" ] || missing="$missing $v"
done
[ -z "$missing" ] || {
    echo "config/rig.local.env is incomplete:$missing" >&2
    exit 1
}
for v in RIG_PASSWORD SSH_PUBKEY WIFI_PSK WIFI_SSIDS; do
    case "${!v}" in
        *"<"*|*"@"*">"*) echo "$v still holds the example placeholder" >&2; exit 1 ;;
    esac
done

# --- locate the card's boot partition -----------------------------------------
if [ -n "$CARD" ]; then
    [ -b "$CARD" ] || { echo "not a block device: $CARD" >&2; exit 2; }
    BOOTPART=""
    for part in "${CARD}"1 "${CARD}"?*; do
        [ -b "$part" ] || continue
        if [ "$(blkid -o value -s TYPE "$part" 2>/dev/null || true)" = "vfat" ]; then
            BOOTPART="$part"; break
        fi
    done
else
    BOOTPART="$(readlink -f "/dev/disk/by-label/$LABEL" 2>/dev/null || true)"
fi
if [ -n "$BOOTPART" ] && [ ! -b "$BOOTPART" ]; then
    BOOTPART=""   # readlink -f resolves dangling symlinks; trust only a device.
fi
[ -n "$BOOTPART" ] || {
    echo "no FAT boot partition found (label '$LABEL') — insert the card" >&2
    [ "$DRY_RUN" -eq 1 ] || {
        lsblk -o NAME,SIZE,RM,TYPE,FSTYPE,LABEL,MOUNTPOINT
        exit 1
    }
}
[ -z "$BOOTPART" ] || echo "boot partition: $BOOTPART"

# --- render the first-boot script ---------------------------------------------
RENDERED="$(mktemp)"
trap 'rm -f "$RENDERED"' EXIT
# Bash-only substitution: no secret ever reaches argv (visible in `ps`) or the
# environment of a child process.
text="$(cat "$TMPL")"
for v in RIG_HOSTNAME RIG_USER RIG_PASSWORD SSH_PUBKEY WIFI_COUNTRY WIFI_SSIDS WIFI_PSK; do
    text="${text//@${v}@/${!v}}"
done
# grep exits 1 when nothing is left — that is the good case, not a failure.
left="$(printf '%s' "$text" | grep -o '@[A-Z_]\+@' | sort -u | tr '\n' ' ' || true)"
[ -z "$left" ] || { echo "template placeholders left unfilled: $left" >&2; exit 1; }
rendered_umask="$(umask)"
umask 077
printf '%s\n' "$text" >"$RENDERED"
umask "$rendered_umask"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "rendered $(wc -l <"$RENDERED") lines (dry run — nothing written)"
    if [ -n "$BOOTPART" ]; then
        echo "would install: $BOOTPART/firstrun.sh"
    else
        echo "card not inserted — the install target is unresolved"
    fi
    echo "would append: systemd.run=/boot/firstrun.sh ... cfg80211.ieee80211_regdom=$WIFI_COUNTRY"
    exit 0
fi

# --- mount, write, verify, unmount --------------------------------------------
MNT="$(findmnt -no TARGET "$BOOTPART" 2>/dev/null || true)"
MOUNTED_BY_US=0
if [ -z "$MNT" ]; then
    MNT="$(mktemp -d)"
    if [ "$(id -u)" -eq 0 ]; then
        mount "$BOOTPART" "$MNT"
    else
        # Unprivileged, desktop path: udisks mounts with the caller as owner.
        udisksctl mount -b "$BOOTPART" -o rw --no-user-interaction >/dev/null
        rmdir "$MNT"
        MNT="$(findmnt -no TARGET "$BOOTPART")"
    fi
    MOUNTED_BY_US=1
fi

if [ ! -w "$MNT" ]; then
    echo "$MNT is not writable by $(id -un) — run this with sudo, or remount it rw" >&2
    exit 1
fi

[ -f "$MNT/cmdline.txt" ] || { echo "no cmdline.txt on $MNT" >&2; exit 1; }
[ -e "$MNT/cmdline.txt.pre-provision" ] ||
    cp "$MNT/cmdline.txt" "$MNT/cmdline.txt.pre-provision"

CMD="$(tr -d '\r\n' <"$MNT/cmdline.txt")"
case "$CMD" in
    *systemd.run=*) : ;;
    *) CMD="$CMD systemd.run=/boot/firstrun.sh systemd.run_success_action=reboot systemd.unit=kernel-command-line.target" ;;
esac
case "$CMD" in
    *cfg80211.ieee80211_regdom=*) : ;;
    *) CMD="$CMD cfg80211.ieee80211_regdom=$WIFI_COUNTRY" ;;
esac
printf '%s\n' "$CMD" >"$MNT/cmdline.txt"
install -m 0644 "$RENDERED" "$MNT/firstrun.sh"
sync

cmp -s "$RENDERED" "$MNT/firstrun.sh" || { echo "read-back mismatch on firstrun.sh" >&2; exit 1; }
echo "installed: $MNT/firstrun.sh"
echo "cmdline  : $CMD"

if [ "$MOUNTED_BY_US" -eq 1 ]; then
    if [ "$(id -u)" -eq 0 ]; then umount "$MNT"; else udisksctl unmount -b "$BOOTPART" >/dev/null; fi
    echo "unmounted $BOOTPART"
fi
