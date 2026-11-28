#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
#
# Offline preflight: does every external command the car-kit scripts call exist in the
# decrypted 260319 rootfs? (The real unit has no `timeout`; that lesson cost a script
# rewrite. Run this after editing any kit script.) Reads the rootfs only.
#
# Usage: sh scripts/check-kit-commands.sh [rootfs-dir] [command...]
# Exit 0 = all present, 1 = something missing.
R="${1:-${ROOTFS:-}}"
[ "$#" -gt 0 ] && shift
if [ "$#" -gt 0 ]; then
    CMDS="$*"
else
    CMDS="id mkdir cp rm chmod sha256sum cmp cat grep getfattr chsmack date tr sed mktemp \
pgrep pidof readlink mount ls dirname basename tee tail head sort reboot systemctl ln mv \
awk wc sleep nohup setsid mkfifo dbus-monitor dbus-send ps cut stat sync uname od"
fi
missing=0
for c in $CMDS; do
    found=""
    for d in bin sbin usr/bin usr/sbin; do
        if [ -e "$R/$d/$c" ] || [ -L "$R/$d/$c" ]; then found="$d/$c"; break; fi
    done
    if [ -n "$found" ]; then
        echo "ok       $c  ($found)"
    else
        echo "MISSING  $c"
        missing=1
    fi
done
exit $missing
