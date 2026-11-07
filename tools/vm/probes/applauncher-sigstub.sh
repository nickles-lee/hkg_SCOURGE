#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# applauncher-sigstub A/B probe: stock vs patched AppLauncher .sign gate.
#
# Phase S1 (stock launcher):  startup-group preload of a signature-corrupted
#   official binary (AppDMClient + one appended byte: .sign no longer matches,
#   binary still loads) must be REFUSED (CheckSign ret = 0, no execve), while
#   the good-signature ApplicationManager preload still spawns (positive
#   control through the same checked path).
# Phase S2 (stock launcher):  preload of an unsigned marker script (not an
#   ELF, no .sign) must be REFUSED (readSection elf_begin fail, no execve,
#   no marker file).
# Phase P (patched launcher): the good-signature ApplicationManager, a
#   signature-corrupted official binary (AppDMClient + one appended byte:
#   .sign no longer matches, binary still loads) and an unsigned marker
#   script (AppSetup slot, not an ELF) must ALL spawn, and the
#   corrupted-signature AppDMClient must still register its bus name (the
#   binary is functionally intact, only its signature is stale).
#
# The patched launcher substitutes two gates with `mov eax,1; ret`:
#   FUN_00115b50 (readSection .sign extraction + signCheck call) and
#   FUN_00112100 (navi file CheckSign).  On-demand request.launch records
#   set the +0x1c skip flag and are not sign-checked even on the stock
#   binary; this probe exercises the startup-group/preload path, which is
#   the path that verifies.
#
# Synthetic startup groups, micom model and software graphics stay explicit
# substitutions.  Not stock equivalence.
set -eu
modprobe ehci-pci; modprobe usb-storage; modprobe sd_mod
mkdir -p /media/tools /media/graphics /tmp/sigstub
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
[ -f /media/tools/AppLauncher.sigstub ] || { echo 'HARNESS_FAIL missing /media/tools/AppLauncher.sigstub'; exit 1; }
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
/usr/bin/dbus-monitor --session >/tmp/sigstub-bus.log 2>&1 &
printf '%s\n' '{"0":[{"appName":"ApplicationManager"},{"appName":"AppDMClient"}]}' >/rw_data/applaunch-config.json
echo 'HARNESS_SUBSTITUTION launcher-manager-client-startup-group'
/micom-dbus-sim 00000826050000051302000003c8140000000000000000000000020001000000000000 >/tmp/sigstub-micom.log 2>&1 &
echo 'HARNESS_SUBSTITUTION micom-model'
export XDG_RUNTIME_DIR=/var/run/user/root WAYLAND_DISPLAY=wayland-sigstub
printf '[core]\nshell=ivi-shell.so\n[ivi-shell]\nivi-module=ivi-window-manager.so,ivi-controller.so\nivi-shell-user-interface=/bin/true\n' >/tmp/weston-sigstub.ini
weston --backend=headless-backend.so --config=/tmp/weston-sigstub.ini \
    --width=1280 --height=720 --socket=wayland-sigstub --idle-time=0 >/tmp/sigstub-weston.log 2>&1 &
weston_pid=$!
cleanup() {
    rc=$?
    echo "SIGSTUB_PROBE_RC $rc"
    for file in /tmp/sigstub/stock1-launcher.out /tmp/sigstub/stock1.trace \
        /tmp/sigstub/stock2-launcher.out /tmp/sigstub/stock2.trace \
        /tmp/sigstub/patched-launcher.out /tmp/sigstub/patched.trace \
        /tmp/sigstub-bus.log /tmp/sigstub-weston.log /log_data/AppUpgrade/fota/DMClient.log; do
        echo "=== $file ==="
        tail -120 "$file" 2>/dev/null || true
    done
    echo "=== /tmp/sigstub markers ==="
    ls -la /tmp/sigstub 2>/dev/null || true
    # Restore the pristine binaries; the per-run overlay is discarded anyway.
    cp -f /tmp/sigstub/AppDMClient.orig /app/bin/AppDMClient 2>/dev/null || true
    cp -f /tmp/sigstub/ApplicationManager.orig /app/bin/ApplicationManager 2>/dev/null || true
    cp -f /app/bin/AppLauncher.stock /app/bin/AppLauncher 2>/dev/null || true
    kill "$weston_pid" 2>/dev/null || true
}
trap cleanup EXIT
for attempt in {1..20}; do [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && break; sleep 1; done
[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]
export LD_LIBRARY_PATH=/media/graphics/runtime:/media/tools/mesa112/usr/lib/x86_64-linux-gnu/mesa-egl:/media/tools/mesa112/usr/lib/x86_64-linux-gnu:/media/tools/llvm38/usr/lib/x86_64-linux-gnu:/app/lib:/navi/navi/Lib:/usr/lib
export LIBGL_DRIVERS_PATH=/media/tools/mesa112/usr/lib/x86_64-linux-gnu/dri
unset QT_WAYLAND_SHELL_INTEGRATION
export QT_QPA_PLATFORM=wayland-egl LIBGL_ALWAYS_SOFTWARE=1
export MESA_LOADER_DRIVER_OVERRIDE=swrast EGL_PLATFORM=wayland
export QV4_MM_MAX_CHUNK_SIZE=16384 QV4_NO_SSA=1 QV4_FORCE_INTERPRETER=1 QSG_INFO=1
echo 'HARNESS_SUBSTITUTION software-egl'
echo 'HARNESS_SUBSTITUTION compositor-noop'

has_owner() {
    dbus-send --session --print-reply --dest=org.freedesktop.DBus / org.freedesktop.DBus.NameHasOwner \
        "string:$1" 2>/dev/null | grep -q 'boolean true'
}
launcher_pid() {
    for file in /proc/[0-9]*/comm; do
        read -r name <"$file" || continue
        if [ "$name" = AppLauncher ]; then
            local pid=${file#/proc/}
            echo "${pid%/comm}"
            return 0
        fi
    done
    return 1
}
kill_launcher_tree() {
    # SIGKILL only: Qt shutdown hangs in this guest, and children must be
    # orphaned (not killed by the launcher) so teardown is deterministic.
    # /proc/<pid>/comm is truncated to 15 chars: "ApplicationMana...".
    for file in /proc/[0-9]*/comm; do
        read -r name <"$file" || continue
        case "$name" in AppLauncher|ApplicationMana*|AppDMClient)
            local pid=${file#/proc/}
            kill -9 "${pid%/comm}" 2>/dev/null || true ;; esac
    done
    for attempt in {1..15}; do kill -0 "$1" 2>/dev/null || return 0; sleep 1; done
}
launcher_alive() {
    for file in /proc/[0-9]*/comm; do
        read -r name <"$file" || continue
        if [ "$name" = AppLauncher ]; then return 0; fi
    done
    return 1
}

# Fixtures: keep pristine copies, then corrupt the client's signature with one
# appended byte (hash changes, .sign goes stale, the ELF still loads).
cp /app/bin/AppDMClient /tmp/sigstub/AppDMClient.orig
cp /app/bin/ApplicationManager /tmp/sigstub/ApplicationManager.orig
sha256sum /app/bin/AppLauncher /app/bin/AppDMClient /app/bin/ApplicationManager
printf '\n' >>/app/bin/AppDMClient
echo "badsign_size=$(stat -c%s /app/bin/AppDMClient) (stock $(stat -c%s /tmp/sigstub/AppDMClient.orig))"

# ---------------- Phase S1: stock launcher vs bad signature -------------
cd /app/bin
strace -f -ttt -e trace=execve,kill,wait4 -s 4096 -o /tmp/sigstub/stock1.trace \
    /app/bin/AppLauncher 3 >/tmp/sigstub/stock1-launcher.out 2>&1 &
stock1_tracer=$!
for attempt in {1..40}; do has_owner com.lge.avn.applauncher && break; sleep 1; done
has_owner com.lge.avn.applauncher || { echo 'HARNESS_BLOCKED stock launcher did not acquire its bus name'; exit 77; }
echo 'HARNESS_CHECK stock-launcher-bus PASS'
for attempt in {1..45}; do has_owner com.lge.hyundai.avn.appmanager && break; sleep 1; done
has_owner com.lge.hyundai.avn.appmanager || { echo 'HARNESS_BLOCKED good-signature ApplicationManager preload did not register'; exit 77; }
echo 'HARNESS_CHECK stock-manager-preload PASS'
sleep 3
grep -q 'readSection : /app/bin/AppDMClient' /tmp/sigstub/stock1-launcher.out || {
    echo 'HARNESS_FAIL stock launcher did not attempt the .sign check for AppDMClient'; exit 1; }
grep -Eq 'CheckSign : /app/bin/AppDMClient .*ret = 0' /tmp/sigstub/stock1-launcher.out || {
    echo 'HARNESS_FAIL bad-signature AppDMClient did not fail signCheck on the stock launcher'; exit 1; }
if grep -q 'execve("/app/bin/AppDMClient"' /tmp/sigstub/stock1.trace; then
    echo 'HARNESS_FAIL stock launcher spawned the bad-signature binary'; exit 1
fi
if has_owner com.lge.hkmc.avn.appfunc.AppDMClient; then
    echo 'HARNESS_FAIL refused AppDMClient nevertheless registered'; exit 1
fi
echo 'HARNESS_CHECK stock-refuses-badsign PASS'
kill_launcher_tree "$stock1_tracer"
if launcher_alive; then echo 'HARNESS_FAIL stock launcher survived teardown'; exit 1; fi

# ---------------- Phase S2: stock launcher vs unsigned script -----------
printf '#!/bin/sh\necho $$ >>/tmp/sigstub/script-marker.txt\nsleep 30\n' >/app/bin/AppDMClient
chmod 755 /app/bin/AppDMClient
rm -f /tmp/sigstub/script-marker.txt
strace -f -ttt -e trace=execve,kill,wait4 -s 4096 -o /tmp/sigstub/stock2.trace \
    /app/bin/AppLauncher 3 >/tmp/sigstub/stock2-launcher.out 2>&1 &
stock2_tracer=$!
for attempt in {1..40}; do has_owner com.lge.avn.applauncher && break; sleep 1; done
has_owner com.lge.avn.applauncher || { echo 'HARNESS_BLOCKED stock launcher (2) did not acquire its bus name'; exit 77; }
# The group preload happens seconds after bus ownership (the first launcher
# took ~7.5 s from exec to the manager spawn); wait for the check attempt.
for attempt in {1..40}; do grep -q 'readSection : /app/bin/AppDMClient' /tmp/sigstub/stock2-launcher.out && break; sleep 1; done
grep -q 'readSection : /app/bin/AppDMClient' /tmp/sigstub/stock2-launcher.out || {
    echo 'HARNESS_FAIL stock launcher did not attempt the .sign check for the marker script'; exit 1; }
grep -Eq 'readSection (elf_begin fail|get data fail|open fail)' /tmp/sigstub/stock2-launcher.out || {
    echo 'HARNESS_FAIL unsigned marker script did not fail readSection on the stock launcher'; exit 1; }
if grep -q 'execve("/app/bin/AppDMClient"' /tmp/sigstub/stock2.trace; then
    echo 'HARNESS_FAIL stock launcher spawned the unsigned script'; exit 1
fi
if [ -f /tmp/sigstub/script-marker.txt ]; then
    echo 'HARNESS_FAIL unsigned script ran on the stock launcher'; exit 1
fi
echo 'HARNESS_CHECK stock-refuses-unsigned PASS'
kill_launcher_tree "$stock2_tracer"
if launcher_alive; then echo 'HARNESS_FAIL stock launcher (2) survived teardown'; exit 1; fi

# ---------------- Phase P: patched launcher ------------------------------
if launcher_alive; then echo 'HARNESS_FAIL launcher still running before the swap'; exit 1; fi
cp /app/bin/AppLauncher /app/bin/AppLauncher.stock
cp /media/tools/AppLauncher.sigstub /app/bin/AppLauncher
echo 'HARNESS_SUBSTITUTION applauncher-sigstub'
sha256sum /app/bin/AppLauncher
# Add an AppSetup slot for the unsigned-script case so the real
# ApplicationManager stays in the group (the client needs it to finish
# its registration; the manager itself is a good-signature positive control).
printf '%s\n' '{"0":[{"appName":"ApplicationManager"},{"appName":"AppDMClient"},{"appName":"AppSetup"}]}' >/rw_data/applaunch-config.json
# Client slot: functionally-intact binary with a stale signature.
cp /tmp/sigstub/AppDMClient.orig /app/bin/AppDMClient
printf '\n' >>/app/bin/AppDMClient
# AppSetup slot: unsigned marker script (no .sign, not an ELF).
printf '#!/bin/sh\necho $$ >>/tmp/sigstub/script-marker.txt\nsleep 30\n' >/app/bin/AppSetup
chmod 755 /app/bin/AppSetup
rm -f /tmp/sigstub/script-marker.txt
strace -f -ttt -e trace=execve,kill,wait4 -s 4096 -o /tmp/sigstub/patched.trace \
    /app/bin/AppLauncher 3 >/tmp/sigstub/patched-launcher.out 2>&1 &
patched_tracer=$!
for attempt in {1..40}; do has_owner com.lge.avn.applauncher && break; sleep 1; done
has_owner com.lge.avn.applauncher || { echo 'HARNESS_BLOCKED patched launcher did not acquire its bus name'; exit 77; }
echo 'HARNESS_CHECK patched-launcher-bus PASS'
for attempt in {1..45}; do has_owner com.lge.hyundai.avn.appmanager && break; sleep 1; done
has_owner com.lge.hyundai.avn.appmanager || { echo 'HARNESS_BLOCKED good-signature ApplicationManager did not register under the patched launcher'; exit 77; }
echo 'HARNESS_CHECK patched-manager-preload PASS'
for attempt in {1..40}; do [ -f /tmp/sigstub/script-marker.txt ] && break; sleep 1; done
[ -f /tmp/sigstub/script-marker.txt ] || {
    echo 'HARNESS_FAIL patched launcher did not spawn the unsigned script'; exit 1; }
grep -q 'execve("/app/bin/AppSetup"' /tmp/sigstub/patched.trace || {
    echo 'HARNESS_FAIL no execve of the unsigned script under the patched launcher'; exit 1; }
echo 'HARNESS_CHECK patched-spawns-unsigned PASS'
for attempt in {1..40}; do grep -q 'execve("/app/bin/AppDMClient"' /tmp/sigstub/patched.trace && break; sleep 1; done
grep -q 'execve("/app/bin/AppDMClient"' /tmp/sigstub/patched.trace || {
    echo 'HARNESS_FAIL patched launcher did not spawn the bad-signature binary'; exit 1; }
grep -Eq 'm_AppProcess->start\(/app/bin/AppDMClient\) PID\[[1-9]' /tmp/sigstub/patched-launcher.out || {
    echo 'HARNESS_FAIL patched launcher spawn of AppDMClient has no nonzero PID'; exit 1; }
echo 'HARNESS_CHECK patched-spawns-badsign PASS'
for attempt in {1..45}; do has_owner com.lge.hkmc.avn.appfunc.AppDMClient && break; sleep 1; done
has_owner com.lge.hkmc.avn.appfunc.AppDMClient || {
    echo 'HARNESS_BLOCKED bad-signature AppDMClient spawned but did not register its bus name'; exit 77; }
echo 'HARNESS_CHECK patched-client-registers PASS'
if grep -Eq 'CheckSign false|readSection .*fail' /tmp/sigstub/patched-launcher.out; then
    echo 'HARNESS_FAIL patched launcher still logged a signature-check failure'; exit 1
fi
pid=$(launcher_pid) || { echo 'HARNESS_FAIL patched launcher process not found'; exit 1; }
kill -0 "$pid" 2>/dev/null || { echo 'HARNESS_FAIL patched launcher exited'; exit 1; }
has_owner com.lge.avn.applauncher || { echo 'HARNESS_FAIL patched launcher lost its bus name'; exit 1; }
echo 'HARNESS_CHECK patched-launcher-alive PASS'
echo 'SIGSTUB_SCOPE preload-path .sign gate disabled: unsigned and bad-signature binaries spawn and the functionally-intact bad-signature client registers; on-demand skip flag, rendering and stock boot equivalence remain unverified here'
exit 0
