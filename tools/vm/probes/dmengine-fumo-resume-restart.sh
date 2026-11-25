#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU NE_E.EUR.S5W_L.001.001.260128; isolated Mode C VM only.

# Mode C lab FUMO download.
#
# Creates the DMCDEVINFO segment and a lab /update state, points the captured
# tree at a loopback mock DM endpoint, starts the real dmengine and sends the
# OEM session-start command (code 0, field1=1). After the SyncML handshake and
# server authentication the mock serves an OMA Download Descriptor; the OEM
# confirmation command (code 10, DMC_REQ_DL_OK) lets the engine fetch the
# described DP object. The DP header is a structural lab fixture: this proves
# the download path, not an install or update.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
OEM_PID=
DLOK_PID=
RESULT_PID=
DM_PID=
SERVER_PID=
LOG=/log_data/AppUpgrade/fota/vdm.log
MODE=partial
RESULT_CODE=${FUMO_RESULT:-200}
INVALID_RESULT=${FUMO_INVALID_RESULT:-0}
SKIP_RESULT=${FUMO_SKIP_RESULT:-0}
LOG_LEVEL=debug

fail() {
    echo "HARNESS_FAIL $1"
    exit 1
}

modprobe ehci-pci 2>/dev/null || true
modprobe usb-storage 2>/dev/null || true
modprobe sd_mod 2>/dev/null || true
sleep 2
mkdir -p /media/payload
PAYLOAD=
for dev in /dev/sd?; do
    [ -b "$dev" ] || continue
    mount -o ro "$dev" /media/payload 2>/dev/null || continue
    if [ -f /media/payload/mock-dm-server ] \
        && [ -f /media/payload/mock-dm-shm ] \
        && [ -f /media/payload/mock-oem-queue ]; then
        PAYLOAD=/media/payload
        break
    fi
    umount /media/payload 2>/dev/null || true
done
[ -n "$PAYLOAD" ] || { echo 'HARNESS_BLOCKED dmengine-fumo-download payload missing'; exit 77; }
trap 'kill "$OEM_PID" "$DLOK_PID" "$RESULT_PID" "$DM_PID" "$SERVER_PID" 2>/dev/null || true; cd /; umount "$PAYLOAD" 2>/dev/null || true' EXIT

cp "$PAYLOAD/mock-dm-server" /tmp/mock-dm || fail 'fumo-server-copy'
cp "$PAYLOAD/mock-dm-shm" /tmp/mock-dm-shm || fail 'fumo-shm-copy'
cp "$PAYLOAD/mock-oem-queue" /tmp/mock-oem || fail 'fumo-oem-copy'
chmod 0755 /tmp/mock-dm /tmp/mock-dm-shm /tmp/mock-oem || fail 'fumo-helper-mode'

ip link set lo up || fail 'fumo-loopback-up'
if ! ip addr show lo 2>/dev/null | grep -q '127.0.0.1'; then
    fail 'fumo-loopback-address'
fi

/tmp/mock-dm-shm >/tmp/mock-dm-shm.out 2>&1 || fail 'fumo-shm-create'
grep -q 'MOCK_DM_SHM_READY' /tmp/mock-dm-shm.out || fail 'fumo-shm-marker'

cp /etc/passwd.legacy /tmp/passwd || fail 'fumo-passwd'
ln -sf /etc/shadow.Global /tmp/shadow || fail 'fumo-shadow'

mkdir -p /update/fota/dme /update/usb/bin
cp /app/share/dmengine/config.txt /tmp/lab-dm-config.txt || fail 'fumo-config'
sed -i \
    -e "s#^log_all=.*#log_all=$LOG_LEVEL#" \
    -e 's#^is_MOFClient_used=.*#is_MOFClient_used=false#' \
    -e 's#^dmtreefile=.*#dmtreefile=tree.xml#' \
    -e 's#^MOFServer_host_name=.*#MOFServer_host_name=127.0.0.1#' \
    -e '$awbxml=false' \
    /tmp/lab-dm-config.txt || fail 'fumo-config-edit'
sed -e '/<name>Addr<\/name>/,/<value>/ s#<value>.*</value>#<value>http://127.0.0.1:8002/dm/message</value>#' \
    -e '/<name>AAuthSecret<\/name>/,/<value>/ s#<value>.*</value>#<value>lab-secret</value>#' \
    -e '/<name>AAuthName<\/name>/,/<value>/ s#<value>.*</value>#<value>lab</value>#' \
    -e '/<name>PkgURL<\/name>/,/<value>/ s#<value>.*</value>#<value>http://127.0.0.1:8002/package.bin</value>#' \
    -e '/<name>MinServerAuth<\/name>/,/<value>/ s#<value>.*</value>#<value>none</value>#' \
    /app/share/dmengine/tree.xml > /tmp/lab-tree.xml || fail 'fumo-tree-edit'
if [ "$MODE" = combined ]; then
    sed -i '/<name>DownloadAndUpdate<\/name>/,/<\/node>/{
      /<name>PkgURL<\/name>/,/<\/leaf>/{
        /<format>chr<\/format>/a\
              <value>http://127.0.0.1:8002/package.bin</value>
      }
    }' /tmp/lab-tree.xml || fail 'fumo-tree-combined'
fi
cp /tmp/lab-tree.xml /update/fota/dme/tree.xml || fail 'fumo-tree-copy'
cp /app/share/dmengine/version.txt /update/fota/dme/version.txt 2>/dev/null
printf '0' > /update/fota/dm_state.txt || fail 'fumo-dm-state'
printf '20261107125619^NE_E.EUR.S5W_L.001.001.250226^NE_E.EUR.S5W_L.001.001.250604' \
    > /update/FumoUsbUpdateTimeVersion || fail 'fumo-time-file'
printf 'ui:user-interaction/targetVersion:NE_E.EUR.S5W_L.001.001.250604/nextCountFumo:1/nextCountScomo:-1/installTime:99999999/execCommand:0000' \
    > /update/fota/install_param.txt || fail 'fumo-install-param'
printf 'SIGN_VALUE' > /update/package.tar.sign || fail 'fumo-sign'
printf '\017\011\041\217lab-blob' > /update/usb/bin/.lge.upgrade.xml || fail 'fumo-blob'

if grep -q '210.183.168.13' /tmp/lab-tree.xml /tmp/lab-dm-config.txt; then
    fail 'fumo-production-address'
fi

: > /tmp/mock-dm.log
/tmp/mock-dm 8002 /tmp/mock-dm.log partial >/tmp/mock-dm.out 2>&1 &
SERVER_PID=$!
server_wait=0
while [ "$server_wait" -lt 20 ]; do
    grep -q 'MOCK_DM_READY' /tmp/mock-dm.out 2>/dev/null && break
    sleep 1
    server_wait=$((server_wait + 1))
done
grep -q 'MOCK_DM_READY' /tmp/mock-dm.out || fail 'fumo-server-ready'

/usr/bin/dbus-daemon --session --fork --print-address >/tmp/dm-session-addr 2>/dev/null \
    || fail 'fumo-session-bus'
DBUS_ADDR=$(cat /tmp/dm-session-addr 2>/dev/null)
[ -n "$DBUS_ADDR" ] || fail 'fumo-session-address'
export DBUS_SESSION_BUS_ADDRESS="$DBUS_ADDR"

# dmengine's DLT logger waits for a daemon; phase4 starts one for the same
# reason.
/usr/bin/dlt-daemon -d >/tmp/dlt.err 2>&1 || true
sleep 1

cd /app/share/dmengine || fail 'resume-cwd'
start_engine() {
    /app/bin/dmengine config=/tmp/lab-dm-config.txt >"$1" 2>&1 &
    DM_PID=$!
    engine_wait=0
    while [ "$engine_wait" -lt 90 ]; do
        dbus-send --session --print-reply --dest=org.freedesktop.DBus / \
            org.freedesktop.DBus.ListNames >/tmp/resume-names.out 2>/dev/null || true
        grep -q 'com.lge.hkmc.avn.upgrade.fota.mofInterface' /tmp/resume-names.out 2>/dev/null && break
        sleep 1
        engine_wait=$((engine_wait + 1))
    done
    grep -q 'com.lge.hkmc.avn.upgrade.fota.mofInterface' /tmp/resume-names.out 2>/dev/null
}

start_engine /tmp/dm-restart-1.out || fail 'resume-restart-phase1-ready'
echo 'HARNESS_CHECK fumo-resume-restart-phase1-ready PASS'
sleep 2
/tmp/mock-oem 0 /tmp/oem-restart1.log 30 1 0 >/tmp/oem-restart1.out 2>&1 &
OEM_PID=$!
sleep 3
/tmp/mock-oem 10 /tmp/oem-restart1-dlok.log 25 0 0 >/tmp/oem-restart1-dlok.out 2>&1 &
DLOK_PID=$!
partial_wait=0
while [ "$partial_wait" -lt 60 ]; do
    grep -q 'MOCK_DM_PARTIAL method=GET path=/object.bin bytes=64 total=184' /tmp/mock-dm.log 2>/dev/null && break
    sleep 1
    partial_wait=$((partial_wait + 1))
done
if ! grep -q 'MOCK_DM_PARTIAL method=GET path=/object.bin bytes=64 total=184' /tmp/mock-dm.log; then
    tail -40 /tmp/mock-dm.log
    fail 'fumo-resume-partial'
fi
echo 'HARNESS_CHECK fumo-resume-partial PASS'
kill "$OEM_PID" "$DLOK_PID" 2>/dev/null
kill -9 "$DM_PID" 2>/dev/null
wait "$DM_PID" 2>/dev/null
OEM_PID=
DLOK_PID=
sleep 1
[ -f /update/fota/dme/dlresume.dat ] || fail 'fumo-resume-restart-state'
echo 'HARNESS_CHECK fumo-resume-restart-state PASS'
printf '1' > /update/fota/dm_state.txt

kill -9 "$SERVER_PID" 2>/dev/null
sleep 1
/tmp/mock-dm 8002 /tmp/mock-dm.log resume >/tmp/mock-dm2.out 2>&1 &
SERVER_PID=$!
server_wait=0
while [ "$server_wait" -lt 20 ]; do
    grep -q 'MOCK_DM_READY' /tmp/mock-dm2.out 2>/dev/null && break
    sleep 1
    server_wait=$((server_wait + 1))
done
grep -q 'MOCK_DM_READY' /tmp/mock-dm2.out || fail 'fumo-resume-restart-server'
echo 'HARNESS_CHECK fumo-resume-restart-server PASS'

start_engine /tmp/dm-restart-2.out || fail 'fumo-resume-restart-phase2-ready'
echo 'HARNESS_CHECK fumo-resume-restart-phase2-ready PASS'
sleep 2
LOG_MARK=$(wc -c < "$LOG" 2>/dev/null || echo 0)
/tmp/mock-oem 0 /tmp/oem-restart2.log 30 1 0 >/tmp/oem-restart2.out 2>&1 &
OEM_PID=$!
sleep 3
/tmp/mock-oem 10 /tmp/oem-restart2-dlok.log 25 0 0 >/tmp/oem-restart2-dlok.out 2>&1 &
DLOK_PID=$!
resume_wait=0
while [ "$resume_wait" -lt 60 ]; do
    grep -q 'MOCK_DM_RESUME method=GET path=/object.bin offset=64' /tmp/mock-dm.log 2>/dev/null && break
    sleep 1
    resume_wait=$((resume_wait + 1))
done
sleep 5
cp "$LOG" /tmp/vdm-restart.log 2>/dev/null || : > /tmp/vdm-restart.log
if ! grep -q 'MOCK_DM_RESUME method=GET path=/object.bin offset=64 bytes=120 total=184' /tmp/mock-dm.log \
    || ! grep -q 'MOCK_DM_OBJECT_HEADERS range=Range: bytes=64-' /tmp/mock-dm.log; then
    tail -40 /tmp/mock-dm.log
    fail 'fumo-resume-request'
fi
echo 'HARNESS_CHECK fumo-resume-request PASS'
if ! grep -qE '(DL|DM) session complete' /tmp/vdm-restart.log \
    || ! grep -q 'Set state, 50' /tmp/vdm-restart.log; then
    grep -aE 'DL |Set state|session' /tmp/vdm-restart.log | tail -40
    fail 'fumo-resume-complete'
fi
echo 'HARNESS_CHECK fumo-resume-complete PASS'
if ! grep -q 'MOCK_DM_NOTIFY path=/notify bytes=11 body=900 Success' /tmp/mock-dm.log; then
    grep -a 'MOCK_DM_NOTIFY' /tmp/mock-dm.log 2>/dev/null
    fail 'fumo-resume-notify'
fi
echo 'HARNESS_CHECK fumo-resume-notify PASS'

if grep -q '210.183.168.13' /tmp/vdm-restart.log /tmp/mock-dm.log /tmp/lab-tree.xml /tmp/lab-dm-config.txt; then
    fail 'fumo-production-address'
fi
echo 'HARNESS_CHECK fumo-no-production PASS'

kill "$DM_PID" "$OEM_PID" "$DLOK_PID" "$SERVER_PID" 2>/dev/null || true
sleep 1
kill -9 "$DM_PID" "$OEM_PID" "$DLOK_PID" "$SERVER_PID" 2>/dev/null || true
echo 'HARNESS_CHECK dmengine-fumo-resume-restart PASS'
