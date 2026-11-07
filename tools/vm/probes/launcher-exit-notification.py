#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Guest-only signal injection to observe the real launcher/manager exit path.

Unlike Request.terminate, this leaves the launcher's process object connected.
Always diagnostic/BLOCKED: does not establish general crash/restart policy.
"""
from pathlib import Path
import sys

VM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(VM))
import launch


def command() -> str:
    probe = (VM / 'probes' / 'launcher-lifecycle.sh').read_text()
    marker = "echo 'LIFECYCLE_REQUEST appkill AppDMClient'\n"
    if probe.count(marker) != 1:
        raise ValueError('lifecycle probe exit boundary changed')
    prefix = probe.split(marker)[0]
    return prefix + '''
echo "LIFECYCLE_INJECTION external-SIGTERM pid=$client_pid"
kill -TERM "$client_pid"
for attempt in {1..15}; do [ ! -d "/proc/$client_pid" ] && break; sleep 1; done
[ ! -d "/proc/$client_pid" ] || { echo 'HARNESS_FAIL injected client still alive'; exit 1; }
# Bounded observation window, not proof of an absent delayed restart policy.
sleep 8
kill -0 "$weston_pid"
has_owner com.lge.avn.applauncher
has_owner com.lge.hyundai.avn.appmanager
echo '=== EXIT NOTIFICATION BUS EVIDENCE ==='
grep -A 8 -B 2 -E 'member=appterminate|member=deregistration' /tmp/lifecycle-bus.log || true
echo '=== EXIT NOTIFICATION LAUNCHER EVIDENCE ==='
grep -E 'onFinished|FINISHED|notifyAppTerminate|appterminate|deregistration' /tmp/lifecycle-launcher.out || true
echo '=== CLIENT PROCESS CENSUS AFTER INJECTION ==='
for file in /proc/[0-9]*/comm; do
    read -r name <"$file" || continue
    if [ "$name" = AppDMClient ]; then echo "LIFECYCLE_REMAINING_CLIENT $file"; fi
done
echo 'HARNESS_BLOCKED external signal diagnostic only; general manager policy remains unverified'
exit 77
'''


if __name__ == '__main__':
    sys.exit(launch.run(command(), False, 160,
                         tools=VM / 'tools.update-20261030.img',
                         payload=VM / 'graphics-pinned-20261031/payload',
                         storage_mode='split', collect_audit=True))
