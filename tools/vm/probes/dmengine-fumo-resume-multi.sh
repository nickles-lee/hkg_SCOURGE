#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU NE_E.EUR.S5W_L.001.001.260128; isolated Mode C VM only.

# Mode C lab FUMO multi-component resume.
#
# The mock serves a 64/291 partial of the two-component/three-update DP with
# Accept-Ranges and ETag "lab-dp-291". The engine retries with
# Range: bytes=64- / If-Range; the mock returns the remaining 227 bytes with
# 206 and the download completes. The package is a structural lab fixture.
set -u
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
OEM_PID=
DLOK_PID=
RESULT_PID=
DM_PID=
SERVER_PID=
LOG=/log_data/AppUpgrade/fota/vdm.log
MODE=multiresume
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
/tmp/mock-dm 8002 /tmp/mock-dm.log "$MODE" >/tmp/mock-dm.out 2>&1 &
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

cd /app/share/dmengine || fail 'multiresume-cwd'
start_engine() {
    /app/bin/dmengine config=/tmp/lab-dm-config.txt >"$1" 2>&1 &
    DM_PID=$!
    engine_wait=0
    while [ "$engine_wait" -lt 90 ]; do
        dbus-send --session --print-reply --dest=org.freedesktop.DBus / \
            org.freedesktop.DBus.ListNames >/tmp/multiresume-names.out 2>/dev/null || true
        grep -q 'com.lge.hkmc.avn.upgrade.fota.mofInterface' /tmp/multiresume-names.out 2>/dev/null && break
        sleep 1
        engine_wait=$((engine_wait + 1))
    done
    grep -q 'com.lge.hkmc.avn.upgrade.fota.mofInterface' /tmp/multiresume-names.out 2>/dev/null
}

start_engine /tmp/dm-multiresume.out || fail 'fumo-multi-resume-ready'
echo 'HARNESS_CHECK fumo-multi-resume-ready PASS'
sleep 2
LOG_MARK=$(wc -c < "$LOG" 2>/dev/null || echo 0)
/tmp/mock-oem 0 /tmp/oem-multiresume.log 30 1 0 >/tmp/oem-multiresume.out 2>&1 &
OEM_PID=$!
sleep 3
/tmp/mock-oem 10 /tmp/oem-multiresume-dlok.log 25 0 0 >/tmp/oem-multiresume-dlok.out 2>&1 &
DLOK_PID=$!
partial_wait=0
while [ "$partial_wait" -lt 60 ]; do
    grep -q 'MOCK_DM_PARTIAL method=GET path=/object.bin bytes=64 total=291' /tmp/mock-dm.log 2>/dev/null && break
    sleep 1
    partial_wait=$((partial_wait + 1))
done
if ! grep -q 'MOCK_DM_PARTIAL method=GET path=/object.bin bytes=64 total=291' /tmp/mock-dm.log; then
    tail -40 /tmp/mock-dm.log
    fail 'fumo-multi-resume-partial'
fi
if ! grep -q 'MOCK_DM_DD method=GET path=/package.bin bytes=403 object=/object.bin object_bytes=291' /tmp/mock-dm.log; then
    grep -a 'MOCK_DM_DD' /tmp/mock-dm.log
    fail 'fumo-multi-resume-partial'
fi
echo 'HARNESS_CHECK fumo-multi-resume-partial PASS'

resume_wait=0
while [ "$resume_wait" -lt 60 ]; do
    grep -q 'MOCK_DM_RESUME method=GET path=/object.bin offset=64' /tmp/mock-dm.log 2>/dev/null && break
    sleep 1
    resume_wait=$((resume_wait + 1))
done
sleep 5
tail -c +$((LOG_MARK + 1)) "$LOG" > /tmp/vdm-multiresume.log 2>/dev/null
if ! grep -q 'MOCK_DM_RESUME method=GET path=/object.bin offset=64 bytes=227 total=291' /tmp/mock-dm.log \
    || ! grep -q 'MOCK_DM_OBJECT_HEADERS range=Range: bytes=64-' /tmp/mock-dm.log; then
    tail -40 /tmp/mock-dm.log
    fail 'fumo-multi-resume-request'
fi
if ! grep -qE 'ETag: "lab-dp-291"|eTag=<"lab-dp-291">' /tmp/mock-dm.log /tmp/vdm-multiresume.log 2>/dev/null; then
    grep -aE 'ETag|eTag|Range' /tmp/mock-dm.log /tmp/vdm-multiresume.log 2>/dev/null | tail -20
    fail 'fumo-multi-resume-request'
fi
echo 'HARNESS_CHECK fumo-multi-resume-request PASS'

if ! grep -q 'DL session complete' /tmp/vdm-multiresume.log \
    || ! grep -q 'Set state, 30' /tmp/vdm-multiresume.log \
    || ! grep -q 'Set state, 40' /tmp/vdm-multiresume.log; then
    grep -aE 'DL |Set state|session' /tmp/vdm-multiresume.log | tail -40
    fail 'fumo-multi-resume-complete'
fi
echo 'HARNESS_CHECK fumo-multi-resume-complete PASS'

if ! grep -q 'MOCK_DM_NOTIFY path=/notify bytes=11 body=900 Success' /tmp/mock-dm.log; then
    grep -a 'MOCK_DM_NOTIFY' /tmp/mock-dm.log 2>/dev/null
    fail 'fumo-multi-resume-notify'
fi
echo 'HARNESS_CHECK fumo-multi-resume-notify PASS'

if grep -q '210.183.168.13' /tmp/vdm-multiresume.log /tmp/mock-dm.log /tmp/lab-tree.xml /tmp/lab-dm-config.txt; then
    fail 'fumo-production-address'
fi
echo 'HARNESS_CHECK fumo-no-production PASS'

echo '=== MULTI RESUME EVIDENCE ==='
grep -aE 'MOCK_DM_DD|MOCK_DM_PARTIAL|MOCK_DM_RESUME|MOCK_DM_NOTIFY' /tmp/mock-dm.log 2>/dev/null | tail -8
grep -aE 'DL resume|eTag=|DL session complete|Set state, 40' /tmp/vdm-multiresume.log 2>/dev/null | tail -12

kill "$DM_PID" "$OEM_PID" "$DLOK_PID" "$SERVER_PID" 2>/dev/null || true
sleep 1
kill -9 "$DM_PID" "$OEM_PID" "$DLOK_PID" "$SERVER_PID" 2>/dev/null || true
echo 'HARNESS_CHECK dmengine-fumo-resume-multi PASS'
