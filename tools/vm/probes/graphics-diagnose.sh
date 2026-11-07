#!/bin/bash
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Diagnostic only, executed inside QEMU. No application patches or crypto bypass.
set -eu
modprobe ehci-pci; modprobe usb-storage; modprobe sd_mod
mkdir -p /media/tools /run/user/root
chmod 700 /run/user/root
found=0
for attempt in {1..20}; do
    for dev in /dev/sd?; do
        [ -b "$dev" ] || continue
        mount -o ro "$dev" /media/tools 2>/dev/null || continue
        if [ -d /media/tools/mesa112 ]; then found=1; break; fi
        umount /media/tools
    done
    [ "$found" = 1 ] && break
    sleep 1
done
[ "$found" = 1 ]
if [ "${GRAPHICS_ENTROPY:-0}" = 1 ]; then /media/tools/entropy-seed --rdrand; fi
mkdir -p /media/graphics
runtime=
for dev in /dev/sd?; do
    [ -b "$dev" ] || continue
    mount -o ro "$dev" /media/graphics 2>/dev/null || continue
    if [ -f /media/graphics/GRAPHICS_RUNTIME ]; then runtime=/media/graphics/runtime:; break; fi
    umount /media/graphics
done
export XDG_RUNTIME_DIR=/run/user/root WAYLAND_DISPLAY=wayland-probe
printf '[core]\nshell=ivi-shell.so\n[ivi-shell]\nivi-module=ivi-window-manager.so,ivi-controller.so\nivi-shell-user-interface=/bin/true\n' >/tmp/weston-probe.ini
weston --backend=headless-backend.so --config=/tmp/weston-probe.ini \
    --width=1280 --height=720 --socket=wayland-probe --idle-time=0 >/tmp/weston-probe.log 2>&1 &
weston_pid=$!
probe_cleanup() {
    rc=$?
    if [ "$rc" != 0 ]; then
        echo '=== EARLY WESTON DIAGNOSTICS ==='
        cat /tmp/weston-probe.log 2>/dev/null || true
    fi
    kill "$weston_pid" 2>/dev/null || true
}
trap probe_cleanup EXIT
for attempt in {1..20}; do [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] && break; sleep 1; done
[ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]
echo '=== WESTON INFO ==='
weston-info
export LD_LIBRARY_PATH=${runtime}/media/tools/mesa112/usr/lib/x86_64-linux-gnu/mesa-egl:/media/tools/mesa112/usr/lib/x86_64-linux-gnu:/media/tools/llvm38/usr/lib/x86_64-linux-gnu:/app/lib:/usr/lib
export LIBGL_DRIVERS_PATH=/media/tools/mesa112/usr/lib/x86_64-linux-gnu/dri
export LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=swrast EGL_PLATFORM=wayland
export QT_QPA_PLATFORM=wayland-egl QT_DEBUG_PLUGINS=1 QT_LOGGING_RULES='qt.qpa.*=true'
export QSG_INFO=1 LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug
export QT_WAYLAND_SHELL_INTEGRATION=ivi
unset QT_QUICK_BACKEND
echo '=== CLIENT INTEGRATION LINKAGE ==='
ldd /usr/lib/qt5/plugins/wayland-graphics-integration-client/libwayland-egl.so >/tmp/graphics-ldd-client.log 2>&1 &
link_pid=$!
sleep 3
kill -KILL "$link_pid" 2>/dev/null || true
wait "$link_pid" 2>/dev/null || true
cat /tmp/graphics-ldd-client.log
echo '=== QMLSCENE LINKAGE ==='
ldd /usr/bin/qt5/qmlscene >/tmp/graphics-ldd-qml.log 2>&1 &
link_pid=$!
sleep 3
kill -KILL "$link_pid" 2>/dev/null || true
wait "$link_pid" 2>/dev/null || true
cat /tmp/graphics-ldd-qml.log
echo '=== STARTING QMLSCENE ==='
cat >/tmp/graphics-probe.qml <<'QML'
import QtQuick 2.4
Rectangle {
    width: 320; height: 200; color: "#236789"
    Timer { interval: 3000; running: true; onTriggered: { console.log("GRAPHICS_PROBE_TIMER"); Qt.quit(); } }
    Component.onCompleted: console.log("GRAPHICS_PROBE_CREATED")
}
QML
echo '=== QML FILE READY ==='
set +e
/usr/bin/qt5/qmlscene /tmp/graphics-probe.qml >/tmp/qml-probe.log 2>&1 &
qml_pid=$!
echo "QML_PROBE_PID=$qml_pid"
# LLVM's first software-context initialization can take tens of seconds on the
# one-vCPU vendor guest. Wait for process completion, not a short startup sleep.
for ((attempt=0; attempt<${GRAPHICS_WAIT_SECONDS:-60}; attempt++)); do
    kill -0 "$qml_pid" 2>/dev/null || break
    sleep 1
done
echo '=== QML DEADLINE REACHED ==='
timed_out=0
if kill -0 "$qml_pid" 2>/dev/null; then
    timed_out=1
    echo '=== QML SHUTDOWN WAIT ==='
    for task in /proc/"$qml_pid"/task/*; do
        echo "TASK ${task##*/}"
        cat "$task/comm" "$task/wchan" "$task/syscall" 2>/dev/null || true
    done
    if [ "${GRAPHICS_DISCONNECT_DIAGNOSTIC:-0}" = 1 ]; then
        # Causal experiment only: a client needing its compositor terminated is
        # not normal graceful shutdown, so timed_out remains set and fails.
        echo 'HARNESS_SUBSTITUTION compositor-disconnect-diagnostic'
        # SIGTERM enters Weston's own (potentially blocking) teardown. This
        # experiment needs a verified closed server connection, not just a
        # termination request; force only this probe's owned compositor.
        kill -KILL "$weston_pid" 2>/dev/null || true
        wait "$weston_pid" 2>/dev/null
        echo "QT_COMPOSITOR_DISCONNECT_STATUS $?"
        for ((attempt=0; attempt<10; attempt++)); do
            kill -0 "$qml_pid" 2>/dev/null || break
            sleep 1
        done
        if kill -0 "$qml_pid" 2>/dev/null; then
            echo 'QT_AFTER_COMPOSITOR_DISCONNECT alive'
        else
            wait "$qml_pid"
            disconnect_rc=$?
            echo "QT_AFTER_COMPOSITOR_DISCONNECT exited rc=$disconnect_rc"
        fi
    fi
    if [ "${GRAPHICS_GDB:-0}" = 1 ]; then
        # Guest-only attachment. The host GDB may read symbols but must never
        # launch a firmware executable on the host. UART1 has no network route.
        LD_LIBRARY_PATH=/usr/lib /media/graphics/gdbserver --once --attach /dev/ttyS1 "$qml_pid" \
            >/tmp/qt-gdbserver.log 2>&1 &
        gdb_pid=$!
        echo "QT_BACKTRACE_READY pid=$qml_pid gdbserver=$gdb_pid"
        for ((attempt=0; attempt<180; attempt++)); do
            kill -0 "$gdb_pid" 2>/dev/null || break
            sleep 1
        done
        kill -KILL "$gdb_pid" 2>/dev/null || true
        wait "$gdb_pid" 2>/dev/null || true
        echo '=== GUEST GDBSERVER ==='
        cat /tmp/qt-gdbserver.log
    fi
    kill -KILL "$qml_pid" 2>/dev/null
fi
status=1
if [ "$timed_out" = 0 ]; then
    wait "$qml_pid"
    status=$?
fi
if ! grep -q 'GRAPHICS_PROBE_TIMER' /tmp/qml-probe.log; then status=1; fi
if ! grep -q 'GL_VERSION:.*OpenGL ES' /tmp/qml-probe.log; then status=1; fi
if [ "$status" = 0 ]; then
    echo 'HARNESS_CHECK qt-egl-context PASS'
    echo 'HARNESS_CHECK qt-event-loop PASS'
fi
echo 'HARNESS_SUBSTITUTION software-egl'
echo 'HARNESS_SUBSTITUTION compositor-noop'
echo "QML_PROBE_VERDICT_RC=$status"
echo '=== QMLSCENE DIAGNOSTICS ==='
ls -l /tmp/qml-probe.log
head -500 /tmp/qml-probe.log
tail -150 /tmp/qml-probe.log
echo '=== WESTON DIAGNOSTICS ==='
cat /tmp/weston-probe.log
exit "$status"
