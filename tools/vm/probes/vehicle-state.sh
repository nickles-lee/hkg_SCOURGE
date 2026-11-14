#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Deterministic vehicle-state model on the synthetic micom CAN interface.
# The frame mapping is lab-defined (see guest/tweaks_micom_can_sim.c); it is
# not a vendor DBC and does not claim stock vehicle fidelity.
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

dbus-daemon --system --fork >/tmp/vehicle-dbus.log 2>&1 || fail 'cannot start system bus'
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
[ -n "$PAYLOAD" ] || { echo 'HARNESS_BLOCKED vehicle-state payload stick missing'; exit 77; }

cp "$PAYLOAD/tweaks-micom-can-sim" /tmp/can-sim
chmod +x /tmp/can-sim
/tmp/can-sim >/tmp/can-sim.log 2>&1 &
SIM_PID=$!
trap 'kill "$SIM_PID" 2>/dev/null || true; wait "$SIM_PID" 2>/dev/null || true' EXIT

for _ in 1 2 3 4 5 6 7 8 9 10; do
    grep -q 'TWEAKS_CAN_SIM_READY' /tmp/can-sim.log 2>/dev/null && break
    sleep 0.5
done
grep -q 'TWEAKS_CAN_SIM_READY' /tmp/can-sim.log || fail 'vehicle-ready'
echo 'HARNESS_CHECK vehicle-ready PASS'

# fixed <outfile> <canid-array> <startbit> <size> <data>
fixed() {
    local out=$1
    shift
    /usr/bin/dbus-send --system --print-reply --reply-timeout=8000 \
        --dest=com.lge.car.micom /Can com.lge.car.micom.can.SetCANFixedMicomRecv \
        "$@" >"$out" 2>&1 || return 1
    grep -q 'method return' "$out"
}

# state_line <label>: make a GetState call and return the newest log line.
state_line() {
    local label=$1
    local before after
    before=$(grep -c '^TWEAKS_VEHICLE_STATE ' /tmp/can-sim.log 2>/dev/null || true)
    [ -n "$before" ] || before=0
    /usr/bin/dbus-send --system --print-reply --reply-timeout=8000 \
        --dest=com.lge.car.micom /Vehicle \
        org.scourge.vehicle.State.GetState >"/tmp/vehicle-${label}.out" 2>&1 || return 1
    grep -q 'method return' "/tmp/vehicle-${label}.out" || return 1
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        after=$(grep -c '^TWEAKS_VEHICLE_STATE ' /tmp/can-sim.log 2>/dev/null || true)
        [ -n "$after" ] || after=0
        [ "$after" -gt "$before" ] && break
        sleep 0.2
    done
    [ "$after" -gt "$before" ] || return 1
    grep '^TWEAKS_VEHICLE_STATE ' /tmp/can-sim.log | tail -1
}

# Lab-defined frames: 0x0100 speed/gear/ignition; 0x0101 battery/doors/
# climate (half-degrees) / outside (int8).
fixed /tmp/vehicle-f1.out array:byte:0x01,0x00 int32:0 int32:16 int32:42 \
    || fail 'vehicle-speed-write'
fixed /tmp/vehicle-f2.out array:byte:0x01,0x00 int32:16 int32:2 int32:3 \
    || fail 'vehicle-gear-write'
fixed /tmp/vehicle-f3.out array:byte:0x01,0x00 int32:18 int32:2 int32:2 \
    || fail 'vehicle-ignition-write'
fixed /tmp/vehicle-f4.out array:byte:0x01,0x01 int32:0 int32:8 int32:77 \
    || fail 'vehicle-battery-write'
fixed /tmp/vehicle-f5.out array:byte:0x01,0x01 int32:8 int32:4 int32:5 \
    || fail 'vehicle-doors-write'
fixed /tmp/vehicle-f6.out array:byte:0x01,0x01 int32:16 int32:8 int32:44 \
    || fail 'vehicle-climate-write'
fixed /tmp/vehicle-f7.out array:byte:0x01,0x01 int32:24 int32:8 int32:253 \
    || fail 'vehicle-outside-write'

STATE=$(state_line decode) || fail 'vehicle-state-read'
case "$STATE" in
    *'speed=42 gear=3 ignition=2 battery=77 doors=5 climate=22.0 outside=-3 frames=0100,0101'*)
        ;;
    *)
        echo "unexpected decoded state: $STATE"
        exit 1
        ;;
esac
echo 'HARNESS_CHECK vehicle-decode PASS'

# Out-of-range writes must clamp and leave a clamp marker.
fixed /tmp/vehicle-c1.out array:byte:0x01,0x00 int32:0 int32:16 int32:1000 \
    || fail 'vehicle-speed-clamp-write'
fixed /tmp/vehicle-c2.out array:byte:0x01,0x01 int32:0 int32:8 int32:250 \
    || fail 'vehicle-battery-clamp-write'
fixed /tmp/vehicle-c3.out array:byte:0x01,0x01 int32:16 int32:8 int32:80 \
    || fail 'vehicle-climate-clamp-write'
fixed /tmp/vehicle-c4.out array:byte:0x01,0x01 int32:24 int32:8 int32:200 \
    || fail 'vehicle-outside-clamp-write'
for marker in \
    'field=speed requested=1000 applied=300' \
    'field=battery requested=250 applied=100' \
    'field=climate requested=80 applied=60' \
    'field=outside requested=-56 applied=-40'; do
    grep -q "TWEAKS_VEHICLE_CLAMP $marker" /tmp/can-sim.log || fail "vehicle-clamp $marker"
done
STATE=$(state_line clamp) || fail 'vehicle-clamp-state-read'
case "$STATE" in
    *'speed=300 gear=3 ignition=2 battery=100 doors=5 climate=30.0 outside=-40'*)
        ;;
    *)
        echo "unexpected clamped state: $STATE"
        exit 1
        ;;
esac
echo 'HARNESS_CHECK vehicle-clamp PASS'

# Untrusted range values must be ignored, not overflow into the frame.
fixed /tmp/vehicle-r1.out array:byte:0x01,0x00 int32:2147483647 int32:1 int32:1 \
    || fail 'vehicle-range-max-write'
fixed /tmp/vehicle-r2.out array:byte:0x01,0x00 int32:64 int32:1 int32:1 \
    || fail 'vehicle-range-boundary-write'
grep -q 'TWEAKS_VEHICLE_WRITE_IGNORED reason=range startbit=2147483647 size=1' \
    /tmp/can-sim.log || fail 'vehicle-range-max-guard'
grep -q 'TWEAKS_VEHICLE_WRITE_IGNORED reason=range startbit=64 size=1' \
    /tmp/can-sim.log || fail 'vehicle-range-boundary-guard'
STATE=$(state_line range) || fail 'vehicle-range-state-read'
case "$STATE" in
    *'speed=300 gear=3 ignition=2 battery=100'*) ;;
    *)
        echo "unexpected state after range guard: $STATE"
        exit 1
        ;;
esac
echo 'HARNESS_CHECK vehicle-range-guard PASS'

# Fail closed: an unknown method must return a D-Bus error, never succeed.
if /usr/bin/dbus-send --system --print-reply --reply-timeout=8000 \
    --dest=com.lge.car.micom /Vehicle     org.scourge.vehicle.State.Bogus >/tmp/vehicle-unknown.out 2>&1; then
    fail 'vehicle-unknown-method-accepted'
fi
grep -q 'UnknownMethod' /tmp/vehicle-unknown.out || fail 'vehicle-unknown-method'
echo 'HARNESS_CHECK vehicle-unknown-method PASS'

/usr/bin/dbus-send --system --print-reply --reply-timeout=8000 \
    --dest=com.lge.car.micom /Can com.lge.car.micom.can.SetCANMicomRecv \
    array:byte:0x06,0x3f >/tmp/vehicle-bulk.out 2>&1 || fail 'vehicle-bulk-dbus'
grep -q 'method return' /tmp/vehicle-bulk.out || fail 'vehicle-bulk-reply'
grep -q 'TWEAKS_VEHICLE_BULK length=2 retained=2 hex=063f' /tmp/can-sim.log \
    || fail 'vehicle-bulk-marker'
STATE=$(state_line bulk) || fail 'vehicle-bulk-state-read'
case "$STATE" in
    *'bulk=063f'*) ;;
    *)
        echo "unexpected bulk state: $STATE"
        exit 1
        ;;
esac
echo 'HARNESS_CHECK vehicle-bulk PASS'

/usr/bin/dbus-send --system --print-reply --reply-timeout=8000 \
    --dest=com.lge.car.micom /Vehicle \
    org.scourge.vehicle.State.Reset >/tmp/vehicle-reset.out 2>&1 || fail 'vehicle-reset-dbus'
grep -q 'method return' /tmp/vehicle-reset.out || fail 'vehicle-reset-reply'
grep -q 'TWEAKS_VEHICLE_RESET' /tmp/can-sim.log || fail 'vehicle-reset-marker'
STATE=$(state_line reset) || fail 'vehicle-reset-state-read'
case "$STATE" in
    *'speed=0 gear=0 ignition=0 battery=0 doors=0 climate=22.0 outside=20 frames= bulk='*)
        ;;
    *)
        echo "unexpected reset state: $STATE"
        exit 1
        ;;
esac
echo 'HARNESS_CHECK vehicle-reset PASS'

echo '=== VEHICLE STATE LOG ==='
grep -E 'TWEAKS_VEHICLE_(STATE|CLAMP|RESET|BULK|WRITE_IGNORED)' /tmp/can-sim.log | tail -24
echo 'HARNESS_CHECK vehicle-state-smoke PASS'
