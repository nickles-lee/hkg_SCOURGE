#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Preparation phase for the vendor-systemd-dbus scenario.
#
# The reconstructed vendor rootfs expects the missing vendor initramfs to
# populate /var and /tmp/passwd. This lab fixture creates only the runtime
# layout needed to exercise systemd's D-Bus activation path; it is not the
# vendor initramfs and does not claim a stock boot.
set -eu
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

mkdir -p /rw_data/var /etc/tmpfiles.d /var/volatile/log /var/volatile/tmp \
         /var/log /var/cache /var/lib /var/spool /run/lock

# /var -> rw_data/var in this rootfs; D-Bus listens on /var/run/dbus.
# State reuse could leave real directories behind; normally these are links.
for link in /rw_data/var/run /rw_data/var/lock; do
    if [ -d "$link" ] && [ ! -L "$link" ]; then
        rm -rf "$link" || { echo "P28_PREP_FAIL cleanup $link"; exit 1; }
    fi
done
ln -sfn /run /rw_data/var/run || { echo 'P28_PREP_FAIL run-link'; exit 1; }
ln -sfn /run/lock /rw_data/var/lock || { echo 'P28_PREP_FAIL lock-link'; exit 1; }

# /etc/passwd and /etc/shadow point at /tmp and are normally populated by the
# vendor initramfs. Other probes use passwd.legacy; keep the same lab identity
# database. messagebus is required by dbus-daemon --system.
if [ ! -f /tmp/passwd ]; then
    cp /etc/passwd.legacy /tmp/passwd || {
        echo 'P28_PREP_FAIL passwd'
        exit 1
    }
fi
grep -q '^messagebus:' /tmp/passwd 2>/dev/null || \
    echo 'messagebus:x:998:998:messagebus:/run/dbus:/bin/false' >> /tmp/passwd

# systemd mounts a fresh tmpfs on /run; tmpfiles creates the socket parent.
printf 'd /run/dbus 0755 - - -\n' > /etc/tmpfiles.d/zz-p28-dbus.conf || {
    echo 'P28_PREP_FAIL tmpfiles'
    exit 1
}

sync
echo 'P28_PREP_OK'
