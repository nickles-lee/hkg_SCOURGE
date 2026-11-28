#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# TARGET: AU Linux Gen5W 260319 head unit, run as root:  sh uninstall-persist.sh [--yes]
# Undo install-persist.sh: disable the feature now and remove the five units, including
# hudnav-premount.service. Deletes nothing else (the kit in /rw_data/hudnav and the guards'
# state folders stay; the guards are disarmed). The CP library gate cp-rg-enable created by
# the premount unit is removed here; cp-enabled itself is left for the owner.
# Without --yes: report only. Test hooks: HUDNAV_ROOT, HUDNAV_PATH_PREPEND.
set -u
PATH="${HUDNAV_PATH_PREPEND:+$HUDNAV_PATH_PREPEND:}/bin:/sbin:/usr/bin:/usr/sbin"
R="${HUDNAV_ROOT:-}"
KIT="$R/rw_data/hudnav"
BIN="$KIT/bin"
SD="$R/etc/systemd/system"
WANTS="$SD/multi-user.target.wants"
UNITS="hudnav-storeguard.service hudnav-bootguard.service hudnav-bootok.service hudnav-premount.service hudnav-aa.service"
YES=0; [ "${1:-}" = "--yes" ] && YES=1

for u in $UNITS; do printf '%s: unit=%s link=%s\n' "$u" "$([ -e "$SD/$u" ] && echo yes || echo no)" "$([ -L "$WANTS/$u" ] && echo yes || echo no)"; done
echo "enabled flag: $([ -f "$KIT/enabled" ] && echo present || echo absent)"
[ "$YES" = 1 ] || { echo "REPORT ONLY (add --yes)"; exit 0; }

rm -f "$KIT/enabled" "$KIT/aa-nav-enable" "$KIT/cp-rg-enable" \
      "$R/rw_data/hudnav-bootguard/armed" "$R/rw_data/hudnav-storeguard/armed"
echo "switches off, guards disarmed, premount CP gate removed"
[ -f "$BIN/hudnav-run.sh" ] && sh "$BIN/hudnav-run.sh" stop 2>&1 | tail -1
[ -f "$BIN/aa-session.sh" ] && sh "$BIN/aa-session.sh" stop --yes 2>&1 | tail -1

if [ -z "$R" ]; then mount -o remount,rw / || echo "WARNING: remount rw failed"; fi
for u in $UNITS; do rm -f "$WANTS/$u" "$SD/$u"; done
sync
if [ -z "$R" ]; then mount -o remount,ro / || echo "WARNING: remount ro failed"; systemctl daemon-reload; fi
left=0; for u in $UNITS; do [ -e "$SD/$u" ] || [ -L "$WANTS/$u" ] && left=1; done
[ "$left" = 0 ] && echo "UNINSTALLED: units removed" || { echo "WARNING: some unit files remain"; exit 1; }
