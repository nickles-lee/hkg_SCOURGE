#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Synthetic micom CAN smoke. Runs only inside the disposable vendor-rootfs VM.
# This is a deterministic interface/contract test, not real CAN timing.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin

fail() {
    echo "HARNESS_FAIL $1"
    exit 1
}

mkdir -p /rw_data/var/run/dbus /media/payload
modprobe ehci-pci 2>/dev/null || true
modprobe usb-storage 2>/dev/null || true
modprobe sd_mod 2>/dev/null || true
sleep 2
cp /etc/passwd.legacy /tmp/passwd 2>/dev/null || true
ln -sf /etc/shadow.Global /tmp/shadow 2>/dev/null || true
ln -sfn /rw_data/var/run/dbus /run/dbus 2>/dev/null
rm -f /rw_data/var/run/dbus/system_bus_socket /rw_data/var/run/dbus/pid

dbus-daemon --system --fork >/tmp/can-dbus.log 2>&1 || fail 'cannot start system bus'
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/var/run/dbus/system_bus_socket

PAYLOAD=
for dev in /dev/sd?; do
    [ -b "$dev" ] || continue
    mount -o ro "$dev" /media/payload 2>/dev/null || continue
    if [ -f /media/payload/tweaks-micom-can-sim ]; then
        PAYLOAD=/media/payload
        break
    fi
    umount /media/payload 2>/dev/null || true
done
[ -n "$PAYLOAD" ] || { echo 'HARNESS_BLOCKED CAN payload stick missing'; exit 77; }

cp "$PAYLOAD/tweaks-micom-can-sim" /tmp/can-sim
chmod +x /tmp/can-sim
/tmp/can-sim >/tmp/can-sim.log 2>&1 &
SIM_PID=$!
trap 'kill "$SIM_PID" 2>/dev/null || true; wait "$SIM_PID" 2>/dev/null || true' EXIT

for _ in 1 2 3 4 5 6 7 8 9 10; do
    grep -q 'TWEAKS_CAN_SIM_READY' /tmp/can-sim.log 2>/dev/null && break
    sleep 0.5
done
grep -q 'TWEAKS_CAN_SIM_READY' /tmp/can-sim.log || fail 'can-ready'

FIXED_OUT=/tmp/can-fixed.out
/usr/bin/dbus-send --system --print-reply --reply-timeout=8000 \
    --dest=com.lge.car.micom /Can com.lge.car.micom.can.SetCANFixedMicomRecv \
    array:byte:0x06,0x3f int32:26 int32:2 int32:2 >"$FIXED_OUT" 2>&1
FIXED_RC=$?
[ "$FIXED_RC" = 0 ] && grep -q 'method return' "$FIXED_OUT" || fail 'can-fixed-dbus'

BULK_OUT=/tmp/can-bulk.out
/usr/bin/dbus-send --system --print-reply --reply-timeout=8000 \
    --dest=com.lge.car.micom /Can com.lge.car.micom.can.SetCANMicomRecv \
    array:byte:0x06,0x3f >"$BULK_OUT" 2>&1
BULK_RC=$?
[ "$BULK_RC" = 0 ] && grep -q 'method return' "$BULK_OUT" || fail 'can-bulk-dbus'

grep -q 'TWEAKS_CAN_SIM_CALL.*SetCANFixedMicomRecv' /tmp/can-sim.log || fail 'can-fixed-call'
grep -q 'TWEAKS_CAN_SIM_ACK member=SetCANFixedMicomRecv' /tmp/can-sim.log || fail 'can-fixed-ack'
grep -q 'TWEAKS_CAN_SIM_CALL.*SetCANMicomRecv' /tmp/can-sim.log || fail 'can-bulk-call'
grep -q 'TWEAKS_CAN_SIM_ACK member=SetCANMicomRecv' /tmp/can-sim.log || fail 'can-bulk-ack'

echo 'HARNESS_CHECK can-ready PASS'
echo 'HARNESS_CHECK can-fixed PASS'
echo 'HARNESS_CHECK can-bulk PASS'
echo '=== CAN SIM LOG ==='
tail -80 /tmp/can-sim.log
echo 'HARNESS_CHECK can-smoke PASS'
