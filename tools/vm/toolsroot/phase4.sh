#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Direct AppDMClient -> AppUpgrade path with state-based waits.
# Runs entirely inside the vendor-rootfs VM; base images are snapshot-only.
set +e
case "${HARNESS_PROFILE:-research}" in
    research) ;;
    conformance) echo 'HARNESS_BLOCKED phase4 requires research-only application and crypto substitutions'; exit 77 ;;
    *) echo 'HARNESS_FAIL unknown profile'; exit 1 ;;
esac
modprobe ehci-pci; modprobe usb-storage; modprobe sd_mod; sleep 3
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/app/bin

MEDIA=${MEDIA:-/media/usb}
DEV=${DEV:-/dev/sda}
BOOTINFO=${BOOTINFO:-00000826050000051302000003c8140000000000000000000000020001000000000000}
DMARGS='-cv:5 -vv:38 -sv:1299 -dt:3 -l:2 -ck:0 -pp:0 -usm:1 -et:8 -pt:0 -by:0 -pi:0 -pf:0 -cbp:1 -asvr:0 -gui:3 -sc:1 -vc:65 -kt:0 -dn:0 -nm:0 -p2s2:1 -ke:na -pvv:0x26 -os:1 -osr:1 -un:0 -er:0 -is:0 -isa:0 -isrcv:0 -pitch:0.1269x0.1269 -evm:0 -mp:1 -cnt:1'
LOG=/log_data/AppUpgrade/fota/DMClient.log
FAILED=0

diagnostics() {
    status=$?
    if [ "$status" != 0 ]; then
        echo '=== FAILURE DIAGNOSTICS ==='
        for file in /tmp/dm.stdout /tmp/dm.strace /tmp/appmanager-stub.log /tmp/applauncher-stub.log /tmp/appupgrade.out; do
            echo "=== $file ==="
            tail -80 "$file" 2>/dev/null
        done
    fi
}
trap diagnostics EXIT

wait_for_log() {
    pattern=$1
    timeout=${2:-30}
    i=0
    while [ "$i" -lt "$timeout" ]; do
        if grep -aq "$pattern" "$LOG" 2>/dev/null; then
            echo "PHASE4_WAIT_OK [$pattern] after ${i}s"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    echo "PHASE4_WAIT_TIMEOUT [$pattern] after ${timeout}s"
    return 1
}

wait_for_upgrade_log() {
    pattern=$1
    timeout=${2:-240}
    i=0
    while [ "$i" -lt "$timeout" ]; do
        if grep -aqE "$pattern" /log_data/AppUpgrade/upgrade_log 2>/dev/null; then
            echo "PHASE4_UPGRADE_LOG_OK [$pattern] after ${i}s"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    echo "PHASE4_UPGRADE_LOG_TIMEOUT [$pattern] after ${timeout}s"
    return 1
}

phase_fail() {
    echo "PHASE4_FAIL $1"
    FAILED=1
}

mkdir -p /media/usb /media/tools /log_data/AppUpgrade/fota /update/usb/bin /update/fota \
         /rw_data/app_data /rw_data/usr_data /rw_data/var /rw_data/home \
         /rw_data/vr_data /rw_data/mnt /rw_data/mnt/root /rw_data/var/run/dbus \
         /rw_data/var/run/user/root /rw_data/var/log /rw_data/dlt /Data/mnt-c /Data/mnt-wt \
         /Data/mnt-backup /run/dbus /var/run/dbus /var/run/user/root /run/user/root \
         /tmp/session
chmod 700 /run/user/root /var/run/user/root
: > "$LOG"

# esinit normally provides these; /etc/passwd and /etc/shadow are symlinks
# into /tmp in the vendor image.
cp /etc/passwd.legacy /tmp/passwd
ln -sf /etc/shadow.Global /tmp/shadow

# Update tree: official packed updater for the XML hash gate, official
# update.tar.gz for the signature path, and a per-run DecryptToPIPE pass-through
# to the installed decrypted updater (the VM has no credman/TEE).
grep -q " $MEDIA " /proc/mounts || mount "$DEV" "$MEDIA" || exit 1
cp -f /media/tools/AppUpgrade.usb "$MEDIA/AppUpgrade" || exit 1
chmod +x "$MEDIA/AppUpgrade" 2>/dev/null
mkdir -p "$MEDIA/HU/firmware"
[ -f "$MEDIA/HU/firmware/update.tar.gz" ] || cp /media/tools/update.tar.gz "$MEDIA/HU/firmware/update.tar.gz"
if [ -x /app/share/AppUpgrade/DecryptToPIPE ]; then
    [ -f /app/share/AppUpgrade/DecryptToPIPE.orig ] || cp /app/share/AppUpgrade/DecryptToPIPE /app/share/AppUpgrade/DecryptToPIPE.orig
    printf '#!/bin/sh\ncat /app/bin/AppUpgrade\n' > /app/share/AppUpgrade/DecryptToPIPE
    chmod +x /app/share/AppUpgrade/DecryptToPIPE
    echo 'HARNESS_SUBSTITUTION decrypt-pass-through'
fi
echo "=== USB TREE AT START ==="
ls -la "$MEDIA" "$MEDIA/HU/firmware" 2>&1 | head -30

# Vendor Dbuslaunch.sh semantics: fixed session socket plus env file.
rm -f /tmp/session_appmgr /tmp/session_appmgr.env
dbus-daemon --session --print-address --fork > /tmp/session_appmgr || exit 1
printf 'DBUS_SESSION_BUS_ADDRESS=%s\n' "$(cat /tmp/session_appmgr)" > /tmp/session_appmgr.env
export DBUS_SESSION_BUS_ADDRESS="$(cat /tmp/session_appmgr)"

dbus-daemon --system --fork || exit 1
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/var/run/dbus/system_bus_socket
# Credential-manager stand-in: serves the vendor public key as GetData(ay).
/media/tools/credman-stub /app/share/AppUpgrade/teruten5_pub.key >/tmp/credman-stub.log 2>&1 &
echo 'HARNESS_SUBSTITUTION credman-public-key-fixture'
sleep 1
# Persistent automount service owner so the engine can connect its signals.
FSTYPE=unknown
while read -r source target fstype options rest; do
    [ "$target" = "$MEDIA" ] && FSTYPE=$fstype
done </proc/mounts
/media/tools/automount-stub "$MEDIA" "$DEV" "$FSTYPE" >/tmp/automount-stub.log 2>&1 &
echo 'HARNESS_SUBSTITUTION automount-model'
sleep 1
/usr/bin/dbus-monitor --session >/tmp/dbus-session.log 2>&1 &
/usr/bin/dbus-monitor --system >/tmp/dbus-system.log 2>&1 &

dlt-daemon -d >/tmp/dlt.err 2>&1
/usr/bin/dlt-receive -a localhost >/tmp/dlt.out 2>&1 &

printf '[core]\nshell=ivi-shell.so\n[ivi-shell]\nivi-module=ivi-window-manager.so,ivi-controller.so\nivi-shell-user-interface=/bin/true\n' >/tmp/weston.ini
export XDG_RUNTIME_DIR=/var/run/user/root
WESTON_RENDER_ARGS=(--use-pixman)
if [ "${HARNESS_COMPOSITOR:-pixman}" = noop ]; then
    WESTON_RENDER_ARGS=()
    echo 'HARNESS_SUBSTITUTION compositor-noop'
fi
/usr/bin/weston --backend=headless-backend.so --config=/tmp/weston.ini \
    "${WESTON_RENDER_ARGS[@]}" --width=1920 --height=720 --socket=wayland-0 \
    --idle-time=0 --debug >/tmp/weston.log 2>&1 &
sleep 3

/micom-dbus-sim "$BOOTINFO" >/tmp/micom.log 2>&1 &
echo 'HARNESS_SUBSTITUTION micom-model'

# Vehicle/app-manager state: starts at 11, then phase4 triggers standby(12).
/media/tools/appmanager-stub 11 >/tmp/appmanager-stub.log 2>&1 &
echo 'HARNESS_SUBSTITUTION appmanager-model'
sleep 2

export HOME=/root
export QV4_MM_MAX_CHUNK_SIZE=16384 QV4_NO_SSA=1 QV4_FORCE_INTERPRETER=1
if [ "${USE_MESA:-0}" = 1 ] && [ -d /media/tools/mesa112 ]; then
    export LD_LIBRARY_PATH=${HARNESS_GRAPHICS_RUNTIME:+${HARNESS_GRAPHICS_RUNTIME}:}/media/tools/mesa112/usr/lib/x86_64-linux-gnu/mesa-egl:/media/tools/mesa112/usr/lib/x86_64-linux-gnu:/media/tools/llvm38/usr/lib/x86_64-linux-gnu:/app/lib:/navi/navi/Lib:/usr/lib
    export LIBGL_DRIVERS_PATH=/media/tools/mesa112/usr/lib/x86_64-linux-gnu/dri
    export MESA_LOADER_DRIVER_OVERRIDE=swrast
    export EGL_PLATFORM=wayland
    export QT_QPA_PLATFORM=wayland-egl
    export QT_WAYLAND_SHELL_INTEGRATION=ivi LIBGL_ALWAYS_SOFTWARE=1
    export WAYLAND_DISPLAY=wayland-0
    echo "PHASE4_STEP mesa112_swrast"
    echo 'HARNESS_SUBSTITUTION software-egl'
    echo 'HARNESS_UNMODIFIED AppDMClient'
    sha256sum /app/bin/AppDMClient
else
    export LD_LIBRARY_PATH=/app/lib:/navi/navi/Lib:/usr/lib
    export QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
    if [ -f /media/tools/AppDMClient.nogui ]; then
        [ -f /app/bin/AppDMClient.orig ] || cp /app/bin/AppDMClient /app/bin/AppDMClient.orig
        cp /media/tools/AppDMClient.nogui /app/bin/AppDMClient
        chmod +x /app/bin/AppDMClient
        echo 'HARNESS_SUBSTITUTION dmclient-gui-patch'
        echo 'HARNESS_SUBSTITUTION getkeyfile-success-patch'
    fi
fi

# Launch after environment exports: children inherit this context. No OEM
# AppUpgrade argv is invented here; this remains a documented research fixture.
/media/tools/applauncher-stub >/tmp/applauncher-stub.log 2>&1 &
echo 'HARNESS_SUBSTITUTION applauncher-model'
for attempt in {1..10}; do
    grep -q 'APPLAUNCHER_STUB_READY' /tmp/applauncher-stub.log 2>/dev/null && break
    sleep 1
done
grep -q 'APPLAUNCHER_STUB_READY' /tmp/applauncher-stub.log || exit 1

cd /app/bin
strace -f -e trace=execve -s 300 -o /tmp/dm.strace ./AppDMClient $DMARGS >/tmp/dm.stdout 2>&1 &
DMPID=$!
echo "DMCLIENT_PID $DMPID"

wait_for_log 'registerUsbAutoMount' 40 || { phase_fail 'automount registration'; exit 1; }

echo "PHASE4_STEP emit_usbmount"
dbus-send --system --dest=com.lge.car.automount --type=method_call /com/lge/car/automount com.lge.car.automount.trigger_mount >/dev/null 2>&1
if wait_for_log 'USB mount path' 10; then
    echo 'HARNESS_CHECK dmclient-usb PASS'
else
    phase_fail 'first UsbMount not seen'
fi

echo "PHASE4_STEP trigger_standby"
dbus-send --session --dest=com.lge.hyundai.avn.appmanager --type=method_call \
    --print-reply /Information com.lge.avn.appmanager.info.trigger_standby \
    >/tmp/standby-trigger.out 2>&1
if wait_for_log 'slotOnStandbyStatus.*status= 12' 20; then
    echo 'HARNESS_CHECK dmclient-standby PASS'
else
    phase_fail 'standby 12 not seen'
fi

if [ "${HARNESS_TARGET:-updater-launch}" = dmclient-events ]; then
    [ "$FAILED" = 0 ] && exit 0
    exit 1
fi

if wait_for_log 'verifyKeyFile' 20; then
    echo "PHASE4_STEP verifyKeyFile $(grep -a 'verifyKeyFile' "$LOG" | tail -1)"
fi

# Retry the automount signal until the launcher call is observed.
i=0
LAUNCHED=0
while [ "$i" -lt 8 ]; do
    if grep -aq 'APPLAUNCHER_STUB_EXEC_OK' /tmp/applauncher-stub.log 2>/dev/null && \
       grep -aq 'APPLAUNCHER_STUB_ALIVE' /tmp/applauncher-stub.log 2>/dev/null; then
        LAUNCHED=1
        break
    fi
    echo "PHASE4_STEP retry_usbmount $i"
    dbus-send --system --dest=com.lge.car.automount --type=method_call /com/lge/car/automount com.lge.car.automount.trigger_mount >/dev/null 2>&1
    sleep 5
    i=$((i + 1))
done
[ "$LAUNCHED" = 1 ] && echo "PHASE4_STEP appupgrade_spawned" || phase_fail 'AppUpgrade did not spawn'
if [ "$LAUNCHED" = 1 ]; then
    echo 'HARNESS_CHECK updater-exec PASS'
    echo 'HARNESS_CHECK updater-alive PASS'
    wait_for_upgrade_log 'Check Signature|Decrypting|startPartitionUpdate|FAIL|Error' 240 || true
fi

echo "=== PROCESSES ==="
ps -ef | grep -E 'AppDMClient|AppUpgrade|appmanager-stub|ApplicationManager|DecryptToPIPE|tar' | grep -v grep
echo "=== UPDATER TREE ==="
ls -la /update/usb/bin 2>&1
echo "=== DM STDOUT ==="
head -160 /tmp/dm.stdout 2>&1
echo "=== EARLY DMCLOG ==="
grep -a -n 'get_standby_status\|slotOnStandbyStatus\|verifyKeyFile\|slotUsbMounted\|checkUSBUpdate\|m_UsbConneted' "$LOG" 2>&1 | head -80
echo "=== LAUNCH LOG ==="
grep -a -n -A70 'launchAppUpgrade' "$LOG" 2>&1 | tail -180
echo "=== AUTOMOUNT STUB ==="
tail -40 /tmp/automount-stub.log 2>&1
echo "=== CREDMAN STUB ==="
tail -80 /tmp/credman-stub.log 2>&1
echo "=== APPLAUNCHER STUB ==="
tail -80 /tmp/applauncher-stub.log 2>&1
echo "=== APPUPGRADE OUT ==="
tail -80 /tmp/appupgrade.out 2>&1
echo "=== LOGDATA ==="
find /log_data -maxdepth 4 -type f -printf '%p %s\n' 2>/dev/null | sort | head -60
echo "=== UPGRADE LOG ==="
tail -300 /log_data/AppUpgrade/upgrade_log 2>&1
echo "=== DMCLOG CHECKUSB ==="
grep -a -n -E 'checkUSBUpdate|No Hash error|Hash error|AppUpgrade|verifyKeyFile|UsbUpdate' "$LOG" 2>&1 | tail -120
echo "=== EXECVE TRACE ==="
tail -120 /tmp/dm.strace 2>&1

if [ "$FAILED" = 0 ]; then
    echo "PHASE4_OK"
    exit 0
fi
echo "PHASE4_FAIL"
exit 1
