#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Observe unmodified ApplicationManager startup ordering, not a ready-state stub."""
from pathlib import Path
import argparse
import sys

VM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(VM))
import launch


def command(ordered: bool = False) -> str:
    probe = (VM / 'probes' / 'launcher-lifecycle.sh').read_text()
    boundary = ('dbus-send --session --print-reply --dest=com.lge.avn.applauncher /Request '
                + '\\\n' + '    org.freedesktop.DBus.Introspectable.Introspect\n')
    if probe.count(boundary) != 1:
        raise ValueError('lifecycle manager observation boundary changed')
    probe = probe.split(boundary)[0]
    probe = probe.replace('/tmp/lifecycle-weston.log; do',
                          '/tmp/lifecycle-weston.log /tmp/lifecycle-micom.log /tmp/manager-system-bus.log; do')
    probe = probe.replace('/usr/bin/dbus-monitor --session >/tmp/lifecycle-bus.log 2>&1 &',
                          '/usr/bin/dbus-monitor --session >/tmp/lifecycle-bus.log 2>&1 &\n'
                          '/usr/bin/dbus-monitor --system >/tmp/manager-system-bus.log 2>&1 &')
    mode = 'ordered' if ordered else 'manual'
    return f'export LIFECYCLE_MANAGER=real LIFECYCLE_STARTUP={mode}\n' + probe + '''
echo '=== MANAGER RECOVERED INFORMATION INTERFACE ==='
dbus-send --session --print-reply --dest=com.lge.hyundai.avn.appmanager /Information \
    org.freedesktop.DBus.Introspectable.Introspect
snapshot() {
    echo "MANAGER_SNAPSHOT $1"
    for method in ask_standby_status ask_power_status ask_preload_complete_status ask_acc_status ask_ign_status; do
        echo "MANAGER_QUERY $method"
        dbus-send --session --print-reply --dest=com.lge.hyundai.avn.appmanager /Information \
            "com.lge.avn.appmanager.info.$method" || true
    done
}
snapshot initial
sleep 12
snapshot settled
echo '=== REAL LAUNCHER/MANAGER INITIALIZATION EVENTS ==='
grep -A 5 -B 1 -E 'member=preloadcomplete|member=standby_status|member=preload_complete_status' \
    /tmp/lifecycle-bus.log || true
echo '=== MANAGER FILE/SHARED MEMORY OBSERVATION ==='
ls -l /dev/shm || true
cat /proc/sysvipc/shm || true
echo '=== MANAGER PROCESS LOG FILES ==='
for file in /proc/[0-9]*/comm; do
    read -r name <"$file" || continue
    if [ "$name" = ApplicationMana ]; then
        process=${file%/comm}
        ls -l "$process/fd" || true
    fi
done
echo 'HARNESS_BLOCKED manager initialization observation only; no update-ready state synthesized'
exit 77
'''


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ordered', action='store_true')
    args = parser.parse_args()
    sys.exit(launch.run(command(args.ordered), False, 160,
                         tools=VM / 'tools.update-20261030.img',
                         payload=VM / 'graphics-pinned-20261031/payload',
                         storage_mode='split', collect_audit=True))
