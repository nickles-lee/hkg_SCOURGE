#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# car_snapshot.sh - READ-ONLY gzip tar of the car's HUD-relevant state to STDOUT; messages to STDERR.
# Delivered over SSH on stdin (nothing is copied to the car):
#   ssh -p 2222 car@HOST 'sh -s' > snapshot-YYYYMMDD.tgz
# POSIX sh + car tools only: date, cat, sha256sum, cut, ls, dmesg, ps, cp, mkdir, rm, tar, command.
# R and TMPDIR are test hooks (R="" on the car). The single scratch folder is removed on exit.
# Nothing on the car is modified: only reads plus copies into ${TMPDIR:-/tmp}/hudnav_snap.$$
set -u
R="${HUDNAV_ROOT:-}"
PRE="$R/etc/ld.so.preload"
AL="$R/app/bin/AppLauncher.sh"
CPU="$R/lib/systemd/system/carplay_iAP2.service"
T="${TMPDIR:-/tmp}/hudnav_snap.$$"
trap 'rm -rf "$T"' EXIT HUP INT TERM

mkdir -p "$T" || { echo "car_snapshot: cannot create $T" >&2; exit 1; }
echo "car_snapshot: collecting state into $T" >&2

# (a) status.txt, generated inside the scratch folder so tar can include it as a relative path
{
  echo "== date =="; date 2>&1
  echo "== cat /proc/uptime =="; cat /proc/uptime 2>&1
  echo "== sha256 /etc/ld.so.preload =="
  if [ -f "$PRE" ]; then sha256sum "$PRE" 2>&1; else echo absent; fi
  echo "== sha256 /app/bin/AppLauncher.sh =="
  if [ -f "$AL" ]; then sha256sum "$AL" 2>&1; else echo absent; fi
  echo "== sha256 /lib/systemd/system/carplay_iAP2.service =="
  if [ -f "$CPU" ]; then sha256sum "$CPU" 2>&1; else echo absent; fi
  echo "== ls -la /rw_data =="; ls -la "$R/rw_data" 2>&1
  echo "== ls -la /rw_data/tweaks =="; ls -la "$R/rw_data/tweaks" 2>&1
  echo "== ls -la /Data/mnt-wt/shared/public =="; ls -la "$R/Data/mnt-wt/shared/public" 2>&1
  echo "== ls -la /Data/mnt-backup =="; ls -la "$R/Data/mnt-backup" 2>&1
  if command -v dmesg >/dev/null 2>&1; then
    echo "== dmesg | tail -300 =="; dmesg 2>&1 | tail -300
  fi
  if command -v ps >/dev/null 2>&1; then
    echo "== ps =="; ps 2>&1
  fi
} > "$T/status.txt" 2>&1

# (b) copies: whole settings-store folders, then the four files we ever touch (+ tweaks config)
mkdir -p "$T/Data" "$T/etc" "$T/app/bin" "$T/lib/systemd/system" "$T/rw_data/tweaks"
if [ -d "$R/Data/mnt-wt" ]; then
  cp -R "$R/Data/mnt-wt" "$T/Data/" || echo "car_snapshot: copy of Data/mnt-wt failed" >&2
fi
if [ -d "$R/Data/mnt-backup" ]; then
  cp -R "$R/Data/mnt-backup" "$T/Data/" || echo "car_snapshot: copy of Data/mnt-backup failed" >&2
fi
if [ -f "$PRE" ]; then cp "$PRE" "$T/etc/"; fi
if [ -f "$AL" ]; then cp "$AL" "$T/app/bin/"; fi
if [ -f "$CPU" ]; then cp "$CPU" "$T/lib/systemd/system/"; fi
if [ -f "$R/rw_data/tweaks/config.conf" ]; then cp "$R/rw_data/tweaks/config.conf" "$T/rw_data/tweaks/"; fi

# tar from the scratch folder: archive paths are relative (status.txt, Data/mnt-wt/..., etc.)
ITEMS=status.txt
for p in Data etc app lib rw_data; do
  [ -e "$T/$p" ] && ITEMS="$ITEMS $p"
done
echo "car_snapshot: writing tar to stdout" >&2
tar -czf - -C "$T" $ITEMS
rc=$?
echo "car_snapshot: finished (tar rc=$rc)" >&2
exit $rc
