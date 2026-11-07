#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Research lifecycle probe: real AppLauncher and unmodified AppDMClient.
# Synthetic startup groups, hardware state and software graphics stay explicit.
set -eu
modprobe ehci-pci; modprobe usb-storage; modprobe sd_mod
mkdir -p /media/tools /media/graphics
for role in tools graphics; do
    found=0
    for attempt in {1..20}; do
        for dev in /dev/sd?; do
            [ -b "$dev" ] || continue
            mount -o ro "$dev" "/media/$role" 2>/dev/null || continue
            if { [ "$role" = tools ] && [ -x /media/tools/entropy-seed ]; } ||
               { [ "$role" = graphics ] && [ -f /media/graphics/GRAPHICS_RUNTIME ]; }; then
                found=1; break
            fi
            umount "/media/$role"
        done
        [ "$found" = 1 ] && break
        sleep 1
    done
    [ "$found" = 1 ]
done
/media/tools/entropy-seed --rdrand
mkdir -p /rw_data/var/run/dbus /var/run/dbus /run/dbus /run/user/root \
    /var/run/user/root /log_data/AppUpgrade/fota /rw_data/app_data /rw_data/usr_data \
    /rw_data/home /rw_data/vr_data /rw_data/mnt /tmp/session /Data/mnt-c /Data/mnt-wt /Data/mnt-backup
chmod 700 /run/user/root /var/run/user/root
cp /etc/passwd.legacy /tmp/passwd
ln -sf /etc/shadow.Global /tmp/shadow
dbus-daemon --system --fork
export DBUS_SYSTEM_BUS_ADDRESS=unix:path=/var/run/dbus/system_bus_socket
dbus-daemon --session --fork --print-address >/tmp/session_appmgr
export DBUS_SESSION_BUS_ADDRESS="$(cat /tmp/session_appmgr)"
printf 'DBUS_SESSION_BUS_ADDRESS=%s\n' "$DBUS_SESSION_BUS_ADDRESS" >/tmp/session_appmgr.env
/usr/bin/dbus-monitor --session >/tmp/lifecycle-bus.log 2>&1 &
# Recovered config grammar: numbered groups of appName objects. Start the real
# manager before group completion so it receives the launcher's preload signal.
# This intentionally reduced group list is not production startup equivalence.
startup=${LIFECYCLE_STARTUP:-ordered}
if [ "${LIFECYCLE_MANAGER:-real}" = fixture ]; then startup=manual; fi
case "$startup" in
    ordered)
        printf '%s\n' '{"0":[{"appName":"ApplicationManager"}]}' >/rw_data/applaunch-config.json
        echo 'HARNESS_SUBSTITUTION launcher-manager-only-startup-group'
        ;;
    manual)
        printf '{}\n' >/rw_data/applaunch-config.json
        echo 'HARNESS_SUBSTITUTION launcher-empty-startup-groups'
        ;;
    *) echo 'HARNESS_FAIL unknown launcher startup mode'; exit 1 ;;
esac
/micom-dbus-sim 00000826050000051302000003c8140000000000000000000000020001000000000000 >/tmp/lifecycle-micom.log 2>&1 &
echo 'HARNESS_SUBSTITUTION micom-model'
case "${LIFECYCLE_MANAGER:-real}" in
    fixture)
        /media/tools/appmanager-stub 11 >/tmp/lifecycle-manager.log 2>&1 &
        echo 'HARNESS_SUBSTITUTION appmanager-model'
        ;;
    real) echo 'HARNESS_UNMODIFIED ApplicationManager'; sha256sum /app/bin/ApplicationManager ;;
    *) echo 'HARNESS_FAIL unknown manager mode'; exit 1 ;;
esac
export XDG_RUNTIME_DIR=/var/run/user/root WAYLAND_DISPLAY=wayland-lifecycle
printf '[core]\nshell=ivi-shell.so\n[ivi-shell]\nivi-module=ivi-window-manager.so,ivi-controller.so\nivi-shell-user-interface=/bin/true\n' >/tmp/weston-lifecycle.ini
weston --backend=headless-backend.so --config=/tmp/weston-lifecycle.ini \
    --width=1280 --height=720 --socket=wayland-lifecycle --idle-time=0 >/tmp/lifecycle-weston.log 2>&1 &
weston_pid=$!
cleanup() {
    rc=$?
    echo "LIFECYCLE_PROBE_RC $rc"
    for file in /log_data/AppLauncher.log /tmp/lifecycle-launcher.out /tmp/lifecycle.trace \
        /log_data/AppUpgrade/fota/DMClient.log /tmp/lifecycle-manager.log /tmp/lifecycle-bus.log \
        /tmp/lifecycle-weston.log; do
        echo "=== $file ==="
        tail -200 "$file" 2>/dev/null || true
    done
    kill "$weston_pid" 2>/dev/null || true
}
trap cleanup EXIT
for attempt in {1..20}; do [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && break; sleep 1; done
[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]
export LD_LIBRARY_PATH=/media/graphics/runtime:/media/tools/mesa112/usr/lib/x86_64-linux-gnu/mesa-egl:/media/tools/mesa112/usr/lib/x86_64-linux-gnu:/media/tools/llvm38/usr/lib/x86_64-linux-gnu:/app/lib:/navi/navi/Lib:/usr/lib
export LIBGL_DRIVERS_PATH=/media/tools/mesa112/usr/lib/x86_64-linux-gnu/dri
# The real launcher supplies IVI through an append/merge helper. Pre-exporting
# it here produced ivi:ivi in the child; follow the shipped wrapper instead.
unset QT_WAYLAND_SHELL_INTEGRATION
export QT_QPA_PLATFORM=wayland-egl LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=swrast EGL_PLATFORM=wayland
export QV4_MM_MAX_CHUNK_SIZE=16384 QV4_NO_SSA=1 QV4_FORCE_INTERPRETER=1 QSG_INFO=1
echo 'HARNESS_SUBSTITUTION software-egl'
echo 'HARNESS_SUBSTITUTION compositor-noop'
if [ "${LIFECYCLE_SIGSTUB:-0}" = 1 ]; then
    # applauncher_sigstub: swap in the patched launcher (two mov eax,1; ret
    # stubs at file offsets 88912/73984) before it starts. The rest of the
    # contract runs unchanged; on-demand request.launch records skip the
    # sign check even on the stock binary, so this exercises the preload
    # path with the check disabled.
    [ -f /media/tools/AppLauncher.sigstub ] || { echo 'HARNESS_FAIL missing /media/tools/AppLauncher.sigstub'; exit 1; }
    cp /app/bin/AppLauncher /app/bin/AppLauncher.stock
    cp /media/tools/AppLauncher.sigstub /app/bin/AppLauncher
    echo 'HARNESS_SUBSTITUTION applauncher-sigstub'
else
    echo 'HARNESS_UNMODIFIED AppLauncher AppDMClient'
fi
sha256sum /app/bin/AppLauncher /app/bin/AppDMClient
cd /app/bin
strace -f -ttt -e trace=execve,kill,wait4 -s 4096 -o /tmp/lifecycle.trace \
    /app/bin/AppLauncher 3 >/tmp/lifecycle-launcher.out 2>&1 &
launcher_trace=$!
has_owner() {
    dbus-send --session --print-reply --dest=org.freedesktop.DBus / org.freedesktop.DBus.NameHasOwner \
        "string:$1" 2>/dev/null | grep -q 'boolean true'
}
for attempt in {1..40}; do has_owner com.lge.avn.applauncher && break; sleep 1; done
has_owner com.lge.avn.applauncher || { echo 'HARNESS_BLOCKED real launcher did not acquire its bus name'; exit 77; }
echo 'HARNESS_CHECK real-launcher-bus PASS'
if [ "${LIFECYCLE_MANAGER:-real}" = real ]; then
    if [ "$startup" = manual ]; then
        dbus-send --session --print-reply --dest=com.lge.avn.applauncher /Request \
            com.lge.avn.applauncher.request.launch int32:2 string:ApplicationManager string:/app/bin/ApplicationManager
    fi
    for attempt in {1..45}; do has_owner com.lge.hyundai.avn.appmanager && break; sleep 1; done
    has_owner com.lge.hyundai.avn.appmanager || { echo 'HARNESS_BLOCKED real ApplicationManager did not acquire its bus name'; exit 77; }
    echo 'HARNESS_CHECK real-manager-bus PASS'
    dbus-send --session --print-reply --dest=com.lge.hyundai.avn.appmanager /Information \
        com.lge.avn.appmanager.info.ask_standby_status
    if [ "$startup" = ordered ]; then
        for attempt in {1..30}; do
            dbus-send --session --print-reply --dest=com.lge.hyundai.avn.appmanager /Information \
                com.lge.avn.appmanager.info.ask_preload_complete_status >/tmp/manager-preload-reply
            grep -q 'int32 1$' /tmp/manager-preload-reply && break
            sleep 1
        done
        cat /tmp/manager-preload-reply
        grep -q 'int32 1$' /tmp/manager-preload-reply || {
            echo 'HARNESS_BLOCKED real manager did not observe launcher preload completion'
            exit 77
        }
        echo 'HARNESS_CHECK manager-preload-complete PASS'
    fi
fi
dbus-send --session --print-reply --dest=com.lge.avn.applauncher /Request \
    org.freedesktop.DBus.Introspectable.Introspect
dbus-send --session --print-reply --dest=com.lge.avn.applauncher /Request \
    com.lge.avn.applauncher.request.launch int32:311 string:AppDMClient string:/app/bin/AppDMClient
for attempt in {1..40}; do has_owner com.lge.hkmc.avn.appfunc.AppDMClient && break; sleep 1; done
has_owner com.lge.hkmc.avn.appfunc.AppDMClient || { echo 'HARNESS_BLOCKED launched client did not register'; exit 77; }
echo 'HARNESS_CHECK client-bus-owner PASS'
if [ "${LIFECYCLE_MANAGER:-real}" = real ]; then
    for attempt in {1..45}; do
        grep -q 'registeredFromNativeApp AppId\[311\]' /tmp/lifecycle-launcher.out && break
        sleep 1
    done
    grep -q 'registeredFromNativeApp AppId\[311\]' /tmp/lifecycle-launcher.out || {
        echo 'HARNESS_BLOCKED client owns a bus name but native launcher registration is not complete'
        exit 77
    }
    echo 'HARNESS_CHECK client-launcher-ack PASS'
    # Native /Inform.launched acknowledgement is independent of the manager's
    # per-plane registration result. Require the recovered front-plane success,
    # but retain rear-plane rejection instead of presenting total readiness.
    awk '
        /AppEngineBase::registrationAppMain/ {
            if ($0 ~ /plane[[:space:]]*:/) plane=$NF
            if ($0 ~ /response is:/ && plane != "") {
                result=$NF; sub(/\)$/, "", result)
                print "LIFECYCLE_MANAGER_REGISTRATION plane=" plane " result=" result
                if (plane == 1 && result == 0) ok=1
                plane=""
            }
        }
        END { exit !ok }
    ' /log_data/AppUpgrade/fota/DMClient.log || {
        echo 'HARNESS_BLOCKED real manager plane-1 registration success not observed'
        exit 77
    }
    echo 'HARNESS_CHECK manager-plane1-registration PASS'
fi
client_pid=
for file in /proc/[0-9]*/comm; do
    read -r name <"$file" || continue
    if [ "$name" = AppDMClient ]; then client_pid=${file#/proc/}; client_pid=${client_pid%/comm}; break; fi
done
[ -n "$client_pid" ]
echo "LIFECYCLE_CLIENT_PID $client_pid"
echo '=== RECOVERED CHILD COMMAND ==='
tr '\000' '\n' <"/proc/$client_pid/cmdline"
echo '=== OBSERVED CHILD ENVIRONMENT ==='
tr '\000' '\n' <"/proc/$client_pid/environ" | grep -E '^(APPID|QT_[^=]*|LGE_[^=]*|LD_LIBRARY_PATH|WAYLAND_DISPLAY)='
tr '\000' '\n' <"/proc/$client_pid/environ" | grep -qx 'QT_WAYLAND_SHELL_INTEGRATION=ivi'
grep -q "Using the 'ivi' shell integration" /log_data/AppUpgrade/fota/DMClient.log
echo 'HARNESS_CHECK client-ivi-shell-selected PASS'
echo 'LIFECYCLE_REQUEST appkill AppDMClient'
dbus-send --session --print-reply --dest=com.lge.avn.applauncher /Request \
    com.lge.avn.applauncher.request.appkill string:AppDMClient
sleep 2
[ -d "/proc/$client_pid" ]
if grep -q "kill($client_pid," /tmp/lifecycle.trace; then
    echo 'HARNESS_FAIL appkill unexpectedly sent a process signal'
    exit 1
fi
echo 'HARNESS_CHECK appkill-is-bookkeeping PASS'
if grep -Eq 'Failed to load shell integration|The Wayland connection experienced a fatal error' \
        /log_data/AppUpgrade/fota/DMClient.log /tmp/lifecycle-launcher.out; then
    echo 'HARNESS_FAIL client reported a shell integration or Wayland connection failure'
    exit 1
fi
echo 'LIFECYCLE_REQUEST terminate 311'
dbus-send --session --print-reply --dest=com.lge.avn.applauncher /Request \
    com.lge.avn.applauncher.request.terminate int32:311
for attempt in {1..15}; do [ ! -d "/proc/$client_pid" ] && break; sleep 1; done
if [ -d "/proc/$client_pid" ]; then
    echo 'HARNESS_FAIL real launcher termination left the client alive'
    exit 1
fi
grep -q "kill($client_pid, SIGTERM)" /tmp/lifecycle.trace
grep -q "kill($client_pid, SIGKILL)" /tmp/lifecycle.trace
grep -q "wait4($client_pid," /tmp/lifecycle.trace
kill -0 "$weston_pid"
has_owner com.lge.avn.applauncher
if [ "${LIFECYCLE_MANAGER:-real}" = real ]; then has_owner com.lge.hyundai.avn.appmanager; fi
echo 'HARNESS_CHECK real-launcher-sigterm PASS'
echo 'HARNESS_CHECK real-launcher-forced-reap PASS'
echo 'HARNESS_CHECK client-terminated-compositor-alive PASS'
if [ "${LIFECYCLE_ACCEPT_COMPONENT:-0}" = 1 ]; then
    echo 'LIFECYCLE_SCOPE recovered on-demand launch/registration/bookkeeping/forced-termination subset; not stock equivalence'
    exit 0
fi
echo 'HARNESS_BLOCKED lifecycle subset observed; ApplicationManager policy and complete update conformance remain unverified'
exit 77
