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
MODE=${FUMO_MODE:-download}
RESULT_CODE=${FUMO_RESULT:-200}
INVALID_RESULT=${FUMO_INVALID_RESULT:-0}
SKIP_RESULT=${FUMO_SKIP_RESULT:-0}
case "$MODE" in
    multidp|combinedmulti) EXPECTED_OBJECT=291; EXPECTED_COMPONENTS=2 ;;
    *) EXPECTED_OBJECT=184; EXPECTED_COMPONENTS=1 ;;
esac
LOG_LEVEL=notice
case "$MODE" in
    update|combined|combinedmulti) LOG_LEVEL=debug ;;
esac

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
if [ "$MODE" = combined ] || [ "$MODE" = combinedmulti ]; then
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

cd /app/share/dmengine || fail 'fumo-cwd'
/app/bin/dmengine config=/tmp/lab-dm-config.txt >/tmp/dmengine-fumo.out 2>&1 &
DM_PID=$!
echo "DMENGINE_PID $DM_PID"
engine_wait=0
while [ "$engine_wait" -lt 90 ]; do
    dbus-send --session --print-reply --dest=org.freedesktop.DBus / \
        org.freedesktop.DBus.ListNames >/tmp/fumo-names.out 2>/dev/null || true
    grep -q 'com.lge.hkmc.avn.upgrade.fota.mofInterface' /tmp/fumo-names.out 2>/dev/null && break
    sleep 1
    engine_wait=$((engine_wait + 1))
done
if ! grep -q 'com.lge.hkmc.avn.upgrade.fota.mofInterface' /tmp/fumo-names.out 2>/dev/null; then
    tail -40 /tmp/dmengine-fumo.out
    fail 'fumo-ready'
fi
sleep 5
echo 'HARNESS_CHECK fumo-ready PASS'
LOG_MARK=$(wc -c < "$LOG" 2>/dev/null || echo 0)

/tmp/mock-oem 0 /tmp/mock-oem.log 20 1 0 >/tmp/mock-oem.out 2>&1 &
OEM_PID=$!

request_wait=0
while [ "$request_wait" -lt 90 ]; do
    grep -q 'MOCK_DM_DD method=GET path=/package.bin' /tmp/mock-dm.log 2>/dev/null && break
    sleep 1
    request_wait=$((request_wait + 1))
done
if ! grep -q 'MOCK_DM_REQUEST method=POST path=/dm/message' /tmp/mock-dm.log 2>/dev/null; then
    tail -40 /tmp/mock-dm.log 2>/dev/null
    fail 'fumo-http-post'
fi
echo 'HARNESS_CHECK fumo-http-post PASS'

if ! grep -q 'MOCK_DM_RESPONSE status=200 syncml=1 session=' /tmp/mock-dm.log; then
    tail -20 /tmp/mock-dm.log
    fail 'fumo-response'
fi
echo 'HARNESS_CHECK fumo-response PASS'

if ! grep -q 'content_type=application/vnd.syncml.dm+wbxml' /tmp/mock-dm.log; then
    tail -20 /tmp/mock-dm.log
    fail 'fumo-syncml-type'
fi
LENGTH=$(sed -n 's/.*content_length=\([0-9]*\).*/\1/p' /tmp/mock-dm.log | head -1)
if [ -z "$LENGTH" ] || [ "$LENGTH" -le 0 ]; then
    tail -20 /tmp/mock-dm.log
    fail 'fumo-syncml-length'
fi
if ! grep -q 'body_prefix=02' /tmp/mock-dm.log; then
    tail -20 /tmp/mock-dm.log
    fail 'fumo-syncml-wbxml'
fi
echo 'HARNESS_CHECK fumo-syncml PASS'

tail -c +$((LOG_MARK + 1)) "$LOG" > /tmp/vdm-delta.log 2>/dev/null
if ! grep -q '\[dmc_startFumoSession\] sesstionType = \[0\]' /tmp/vdm-delta.log \
    || ! grep -q 'Triggering DM session' /tmp/vdm-delta.log \
    || ! grep -q 'Performing HTTP POST' /tmp/vdm-delta.log; then
    tail -60 /tmp/vdm-delta.log
    fail 'fumo-trigger'
fi
echo 'HARNESS_CHECK fumo-trigger PASS'

if ! grep -q 'MOCK_DM_RESPONSE status=200 syncml=1 session=.* exec=1' /tmp/mock-dm.log; then
    tail -20 /tmp/mock-dm.log
    fail 'fumo-command'
fi
echo 'HARNESS_CHECK fumo-command PASS'

if ! grep -q 'CLIENT_DIGEST=' /tmp/mock-dm-auth.log 2>/dev/null; then
    cat /tmp/mock-dm-auth.log 2>/dev/null
    fail 'fumo-server-auth'
fi
if grep -q 'MOCK_DM_CLIENT_STATUS status=407' /tmp/mock-dm.log; then
    grep -a 'MOCK_DM_CLIENT_STATUS' /tmp/mock-dm.log
    fail 'fumo-server-auth-407'
fi
echo 'HARNESS_CHECK fumo-server-auth PASS'

if ! grep -q 'MOCK_DM_DD method=GET path=/package.bin' /tmp/mock-dm.log \
    || ! grep -q 'object=/object.bin' /tmp/mock-dm.log; then
    tail -40 /tmp/mock-dm.log
    fail 'fumo-dd'
fi
echo 'HARNESS_CHECK fumo-dd PASS'

if [ "$MODE" = missing ]; then
    : > /tmp/mock-oem-dlok.log
    /tmp/mock-oem 10 /tmp/mock-oem-dlok.log 15 0 0 >/tmp/mock-oem-dlok.out 2>&1 &
    DLOK_PID=$!
    missing_wait=0
    while [ "$missing_wait" -lt 60 ]; do
        grep -q 'MOCK_DM_MISSING method=GET path=/object.bin' /tmp/mock-dm.log 2>/dev/null && break
        sleep 1
        missing_wait=$((missing_wait + 1))
    done
    kill "$DLOK_PID" 2>/dev/null
    wait "$DLOK_PID" 2>/dev/null
    DLOK_PID=
    sleep 5
    tail -c +$((LOG_MARK + 1)) "$LOG" > /tmp/vdm-delta.log 2>/dev/null

    if ! grep -q 'MOCK_OEM_SENT code=10' /tmp/mock-oem-dlok.log 2>/dev/null; then
        cat /tmp/mock-oem-dlok.log 2>/dev/null
        fail 'fumo-confirm'
    fi
    echo 'HARNESS_CHECK fumo-confirm PASS'

    if ! grep -q 'MOCK_DM_MISSING method=GET path=/object.bin' /tmp/mock-dm.log 2>/dev/null; then
        tail -30 /tmp/mock-dm.log
        fail 'fumo-missing-request'
    fi
    echo 'HARNESS_CHECK fumo-missing-request PASS'

    if ! grep -q 'DL session aborted, 0x6306' /tmp/vdm-delta.log \
        || ! grep -q 'fumo result : 407, last error : 25350' /tmp/vdm-delta.log; then
        grep -aE 'DL session|fumo result|onSessionStateChange|lastError' /tmp/vdm-delta.log | tail -30
        fail 'fumo-missing-error'
    fi
    echo 'HARNESS_CHECK fumo-missing-error PASS'

    if grep -q 'MOCK_DM_DOWNLOAD method=GET path=/object.bin' /tmp/mock-dm.log 2>/dev/null; then
        grep -a 'MOCK_DM_DOWNLOAD' /tmp/mock-dm.log
        fail 'fumo-missing-no-download'
    fi
    echo 'HARNESS_CHECK fumo-missing-no-download PASS'
else
: > /tmp/mock-oem-dlok.log
/tmp/mock-oem 10 /tmp/mock-oem-dlok.log 15 0 0 >/tmp/mock-oem-dlok.out 2>&1 &
DLOK_PID=$!
download_wait=0
while [ "$download_wait" -lt 60 ]; do
    grep -q 'MOCK_DM_DOWNLOAD method=GET path=/object.bin' /tmp/mock-dm.log 2>/dev/null && break
    sleep 1
    download_wait=$((download_wait + 1))
done
kill "$DLOK_PID" 2>/dev/null
wait "$DLOK_PID" 2>/dev/null
DLOK_PID=
if ! grep -q 'MOCK_OEM_SENT code=10' /tmp/mock-oem-dlok.log 2>/dev/null; then
    cat /tmp/mock-oem-dlok.log 2>/dev/null
    fail 'fumo-confirm'
fi
echo 'HARNESS_CHECK fumo-confirm PASS'

if ! grep -q "MOCK_DM_DOWNLOAD method=GET path=/object.bin bytes=$EXPECTED_OBJECT preamble=RedBendEFDPackage version=4 components=$EXPECTED_COMPONENTS" /tmp/mock-dm.log; then
    tail -40 /tmp/mock-dm.log
    fail 'fumo-download'
fi
echo 'HARNESS_CHECK fumo-download PASS'

if [ "$MODE" = multidp ] || [ "$MODE" = combinedmulti ]; then
    if ! grep -q 'MOCK_DM_MULTIDP method=GET path=/object.bin bytes=291 components=2 updates=3' /tmp/mock-dm.log; then
        grep -a 'MOCK_DM_MULTIDP' /tmp/mock-dm.log 2>/dev/null
        fail 'fumo-multidp'
    fi
    echo 'HARNESS_CHECK fumo-multidp PASS'
fi

if [ "$MODE" = badsig ]; then
    if ! grep -q 'MOCK_DM_BADSIG path=/object.bin bytes=184' /tmp/mock-dm.log; then
        grep -a 'MOCK_DM_BADSIG' /tmp/mock-dm.log 2>/dev/null
        fail 'fumo-badsig'
    fi
    echo 'HARNESS_CHECK fumo-badsig PASS'
fi

if [ "$MODE" = notifyfail ]; then
    notify_wait=0
    while [ "$notify_wait" -lt 30 ]; do
        grep -q 'MOCK_DM_NOTIFY_FAIL path=/notify' /tmp/mock-dm.log 2>/dev/null && break
        sleep 1
        notify_wait=$((notify_wait + 1))
    done
    sleep 3
    tail -c +$((LOG_MARK + 1)) "$LOG" > /tmp/vdm-delta.log 2>/dev/null

    if ! grep -q 'MOCK_DM_NOTIFY_FAIL path=/notify bytes=11 body=900 Success' /tmp/mock-dm.log; then
        grep -a 'MOCK_DM_NOTIFY' /tmp/mock-dm.log 2>/dev/null
        fail 'fumo-notify-fail'
    fi
    echo 'HARNESS_CHECK fumo-notify-fail PASS'

    if ! grep -q 'DL session aborted, 0x6308' /tmp/vdm-delta.log \
        || ! grep -q 'fumo result : 503, last error : 25352' /tmp/vdm-delta.log; then
        grep -aE 'DL session|fumo result|onSessionStateChange|lastError' /tmp/vdm-delta.log | tail -30
        fail 'fumo-notify-failure-error'
    fi
    echo 'HARNESS_CHECK fumo-notify-failure-error PASS'

    if grep -q 'Set state, 40' /tmp/vdm-delta.log \
        || grep -q 'Set state, 60' /tmp/vdm-delta.log; then
        grep -aE 'Set state' /tmp/vdm-delta.log | tail -20
        fail 'fumo-notify-no-completion'
    fi
    echo 'HARNESS_CHECK fumo-notify-no-completion PASS'
else
notify_wait=0
while [ "$notify_wait" -lt 30 ]; do
    grep -q 'MOCK_DM_NOTIFY path=/notify' /tmp/mock-dm.log 2>/dev/null && break
    sleep 1
    notify_wait=$((notify_wait + 1))
done
if ! grep -q 'MOCK_DM_NOTIFY path=/notify bytes=11 body=900 Success' /tmp/mock-dm.log; then
    grep -a 'MOCK_DM_NOTIFY' /tmp/mock-dm.log 2>/dev/null
    fail 'fumo-notify'
fi
echo 'HARNESS_CHECK fumo-notify PASS'

sleep 5
tail -c +$((LOG_MARK + 1)) "$LOG" > /tmp/vdm-delta.log 2>/dev/null
if ! grep -q 'DL session complete' /tmp/vdm-delta.log; then
    grep -aE 'DL session|onSessionStateChange|FUMO state|Set state' /tmp/vdm-delta.log | tail -30
    fail 'fumo-dl-complete'
fi
echo 'HARNESS_CHECK fumo-dl-complete PASS'

if ! grep -q 'Set state, 30' /tmp/vdm-delta.log \
    || ! grep -q 'Set state, 40' /tmp/vdm-delta.log; then
    grep -aE 'FUMO state|Set state|onCompleted' /tmp/vdm-delta.log | tail -30
    fail 'fumo-state'
fi
echo 'HARNESS_CHECK fumo-state PASS'

if [ "$MODE" = combined ] || [ "$MODE" = combinedmulti ]; then
    combined_wait=0
    while [ "$combined_wait" -lt 45 ]; do
        grep -q 'Set state, 60' /tmp/vdm-delta.log 2>/dev/null && break
        sleep 1
        combined_wait=$((combined_wait + 1))
    done
    sleep 2
    tail -c +$((LOG_MARK + 1)) "$LOG" > /tmp/vdm-delta.log 2>/dev/null
    if ! grep -qF 'Got EXEC on "DownloadAndUpdate" node.' /tmp/vdm-delta.log \
        || ! grep -qF 'FUMO DownloadAndUpdate executed in this session' /tmp/vdm-delta.log; then
        grep -aE 'DownloadAndUpdate|Got EXEC|FUMO_Plugin' /tmp/vdm-delta.log | tail -30
        fail 'fumo-combined-command'
    fi
    echo 'HARNESS_CHECK fumo-combined-command PASS'

    if ! grep -qF 'E_VDM_FUMO_STATE_update_progressing' /tmp/vdm-delta.log \
        || ! grep -q 'Set state, 60' /tmp/vdm-delta.log; then
        grep -aE 'update_progressing|Set state' /tmp/vdm-delta.log | tail -30
        fail 'fumo-update-progressing'
    fi
    echo 'HARNESS_CHECK fumo-update-progressing PASS'

    if ! grep -qF 'VDM_FUMO_UA_executeUpdate (forceUpdate=0)' /tmp/vdm-delta.log \
        || ! grep -qF 'confirmUpdateCB (response=0)' /tmp/vdm-delta.log \
        || ! grep -qF 'event to OEM handler = [77]' /tmp/vdm-delta.log \
        || ! grep -qF 'strData = [/update//lab-fumo]' /tmp/vdm-delta.log; then
        grep -aE 'executeUpdate|confirmUpdateCB|event to OEM handler|lab-fumo' /tmp/vdm-delta.log | tail -30
        fail 'fumo-update-handoff'
    fi
    echo 'HARNESS_CHECK fumo-update-handoff PASS'

    if [ "$SKIP_RESULT" = 1 ]; then
        sleep 8
        tail -c +$((LOG_MARK + 1)) "$LOG" > /tmp/vdm-delta.log 2>/dev/null
        if grep -q 'VDMC_FUMO_updateReport' /tmp/vdm-delta.log \
            || grep -q 'Set state, 90' /tmp/vdm-delta.log; then
            grep -aE 'updateReport|Set state' /tmp/vdm-delta.log | tail -20
            fail 'fumo-update-no-result'
        fi
        echo 'HARNESS_CHECK fumo-update-no-result PASS'
    else
    : > /tmp/mock-oem-result.log
    /tmp/mock-oem 20 /tmp/mock-oem-result.log 15 "$RESULT_CODE" 0 >/tmp/mock-oem-result.out 2>&1 &
    RESULT_PID=$!
    result_wait=0
    while [ "$result_wait" -lt 30 ]; do
        tail -c +$((LOG_MARK + 1)) "$LOG" > /tmp/vdm-delta.log 2>/dev/null
        grep -q "updateReport] result = \[$RESULT_CODE\]" /tmp/vdm-delta.log 2>/dev/null && break
        sleep 1
        result_wait=$((result_wait + 1))
    done
    kill "$RESULT_PID" 2>/dev/null
    wait "$RESULT_PID" 2>/dev/null
    RESULT_PID=
    sleep 2
    tail -c +$((LOG_MARK + 1)) "$LOG" > /tmp/vdm-delta.log 2>/dev/null

    if ! grep -q "MOCK_OEM_SENT code=20 field1=$RESULT_CODE" /tmp/mock-oem-result.log 2>/dev/null \
        || ! grep -qF "VDMC_FUMO_updateReport] result = [$RESULT_CODE]" /tmp/vdm-delta.log; then
        cat /tmp/mock-oem-result.log 2>/dev/null
        grep -aE 'updateReport|result = \[' /tmp/vdm-delta.log | tail -20
        fail 'fumo-update-result-sent'
    fi
    echo 'HARNESS_CHECK fumo-update-result-sent PASS'

    if ! grep -qF "VDM_FUMO_triggerReportSession($RESULT_CODE)" /tmp/vdm-delta.log; then
        grep -aE 'triggerReportSession|Set state' /tmp/vdm-delta.log | tail -30
        fail 'fumo-update-result-accepted'
    fi
    if [ "$INVALID_RESULT" = 1 ]; then
        if ! grep -qF "validateResultCode - invalid result code ($RESULT_CODE)" /tmp/vdm-delta.log \
            || ! grep -qF 'using UNDEFINED(409)' /tmp/vdm-delta.log \
            || ! grep -q 'Set state, 70' /tmp/vdm-delta.log; then
            grep -aE 'validateResultCode|Set state' /tmp/vdm-delta.log | tail -20
            fail 'fumo-update-invalid-remap'
        fi
        echo 'HARNESS_CHECK fumo-update-invalid-remap PASS'
    elif grep -qF 'validateResultCode - invalid result code' /tmp/vdm-delta.log; then
        grep -aE 'validateResultCode' /tmp/vdm-delta.log | tail -10
        fail 'fumo-update-result-rejected'
    fi
    if [ "$RESULT_CODE" = 200 ]; then
        if ! grep -q 'Set state, 90' /tmp/vdm-delta.log; then
            grep -aE 'Set state' /tmp/vdm-delta.log | tail -20
            fail 'fumo-update-result-accepted'
        fi
    else
        if grep -q 'Set state, 90' /tmp/vdm-delta.log; then
            grep -aE 'Set state' /tmp/vdm-delta.log | tail -20
            fail 'fumo-update-failure'
        fi
        echo 'HARNESS_CHECK fumo-update-failure PASS'
    fi
    echo 'HARNESS_CHECK fumo-update-result-accepted PASS'

    if ! grep -q 'DM session complete' /tmp/vdm-delta.log; then
        grep -aE 'DM session|session aborted' /tmp/vdm-delta.log | tail -20
        fail 'fumo-update-result-reported'
    fi
    echo 'HARNESS_CHECK fumo-update-result-reported PASS'
    fi
fi

if [ "$MODE" = update ]; then
    update_wait=0
    while [ "$update_wait" -lt 45 ]; do
        grep -q 'MOCK_DM_UPDATE_EXEC' /tmp/mock-dm.log 2>/dev/null && break
        sleep 1
        update_wait=$((update_wait + 1))
    done
    sleep 3
    tail -c +$((LOG_MARK + 1)) "$LOG" > /tmp/vdm-delta.log 2>/dev/null
    if ! grep -q 'MOCK_DM_UPDATE_EXEC' /tmp/mock-dm.log 2>/dev/null \
        || ! grep -q 'targetURI=./ManagedObjects/FUMO/Update' /tmp/vdm-delta.log; then
        tail -40 /tmp/mock-dm.log
        grep -aE 'FUMO/Update|DM Report|Update' /tmp/vdm-delta.log | tail -30
        fail 'fumo-update-request'
    fi
    echo 'HARNESS_CHECK fumo-update-request PASS'

    if ! grep -q 'FUMO_Plugin_onSessionStateChange(DM): Error=\[24593\]' /tmp/vdm-delta.log \
        || ! grep -q 'DM session aborted, 0x6011' /tmp/vdm-delta.log; then
        grep -aE 'onSessionStateChange|session aborted|lastError' /tmp/vdm-delta.log | tail -30
        fail 'fumo-update-rejected'
    fi
    echo 'HARNESS_CHECK fumo-update-rejected PASS'

    if grep -q 'Set state, 60' /tmp/vdm-delta.log; then
        grep -aE 'Set state' /tmp/vdm-delta.log | tail -20
        fail 'fumo-update-no-completion'
    fi
    echo 'HARNESS_CHECK fumo-update-no-completion PASS'
fi
fi
fi

if grep -q '210.183.168.13' /tmp/vdm-delta.log /tmp/mock-dm.log /tmp/lab-tree.xml /tmp/lab-dm-config.txt; then
    fail 'fumo-production-address'
fi
echo 'HARNESS_CHECK fumo-no-production PASS'

echo '=== MOCK DM REQUEST ==='
head -8 /tmp/mock-dm.log 2>/dev/null
echo '=== VDM TRIGGER ==='
grep -aE 'dmc_startFumoSession|Triggering DM session|Open connection|Performing HTTP POST|onSessionStateChange|DL session' \
    /tmp/vdm-delta.log 2>/dev/null | head -20
echo '=== DD AND PACKAGE GET ==='
grep -aE 'MOCK_DM_DD|MOCK_DM_DOWNLOAD' /tmp/mock-dm.log 2>/dev/null | head -8
kill "$DM_PID" "$OEM_PID" "$DLOK_PID" "$RESULT_PID" "$SERVER_PID" 2>/dev/null || true
sleep 1
kill -9 "$DM_PID" "$OEM_PID" "$DLOK_PID" "$RESULT_PID" "$SERVER_PID" 2>/dev/null || true
if [ "$MODE" = combinedmulti ]; then
    if [ "$INVALID_RESULT" = 1 ]; then
        echo 'HARNESS_CHECK dmengine-fumo-download-update-multi-invalid-result PASS'
    elif [ "$RESULT_CODE" = 200 ]; then
        echo 'HARNESS_CHECK dmengine-fumo-download-update-multi PASS'
    else
        echo 'HARNESS_CHECK dmengine-fumo-download-update-multi-failure PASS'
    fi
elif [ "$MODE" = multidp ]; then
    echo 'HARNESS_CHECK dmengine-fumo-download-multi PASS'
elif [ "$MODE" = badsig ]; then
    echo 'HARNESS_CHECK dmengine-fumo-download-badsig PASS'
elif [ "$MODE" = notifyfail ]; then
    echo 'HARNESS_CHECK dmengine-fumo-notify-failure PASS'
elif [ "$MODE" = missing ]; then
    echo 'HARNESS_CHECK dmengine-fumo-download-missing PASS'
elif [ "$MODE" = combined ]; then
    if [ "$SKIP_RESULT" = 1 ]; then
        echo 'HARNESS_CHECK dmengine-fumo-update-no-result PASS'
    elif [ "$INVALID_RESULT" = 1 ]; then
        echo 'HARNESS_CHECK dmengine-fumo-update-invalid-result PASS'
    elif [ "$RESULT_CODE" = 200 ]; then
        echo 'HARNESS_CHECK dmengine-fumo-download-update PASS'
    else
        echo 'HARNESS_CHECK dmengine-fumo-update-failure PASS'
    fi
elif [ "$MODE" = update ]; then
    echo 'HARNESS_CHECK dmengine-fumo-update PASS'
else
    echo 'HARNESS_CHECK dmengine-fumo-download PASS'
fi
