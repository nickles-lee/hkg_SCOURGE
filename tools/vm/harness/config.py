# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Validated built-in profiles and scenarios, separate from target content."""
from __future__ import annotations

import json
from pathlib import Path

from .audit import audit_script
from .storage import probe_script

VM = Path(__file__).resolve().parent.parent
SCENARIO_NAMES = ('boot', 'usb-discovery', 'dbus-ready', 'dmclient-events',
                  'updater-launch', 'stock-update-stage', 'storage-isolation', 'environment-audit', 'update-graphics',
                  'entropy-ready', 'vendor-lifecycle', 'applauncher-sigstub',
                  'vendor-lifecycle-sigstub', 'ui-smoke', 'wifi-ui',
                  'can-smoke', 'vehicle-state', 'recovery-reboot', 'recovery-no-space',
                  'recovery-corrupt-media', 'recovery-usb-removal',
                  'recovery-power-loss', 'vendor-systemd',
                  'vendor-systemd-dbus', 'recovery-repeat-update')


def load_profile(name: str) -> dict:
    if name not in ('research', 'conformance'):
        raise ValueError(f'unknown profile: {name}')
    profile = json.loads((VM / 'profiles' / f'{name}.json').read_text())
    if profile['name'] != name:
        raise ValueError('profile name mismatch')
    if name == 'conformance' and any(profile['allowed_workarounds'].values()):
        raise ValueError('conformance profile cannot permit application/crypto bypasses')
    return profile


def load_scenario(name: str) -> dict:
    if name not in SCENARIO_NAMES:
        raise ValueError(f'unknown scenario: {name}')
    scenario = json.loads((VM / 'scenarios' / f'{name}.json').read_text())
    if scenario['name'] != name:
        raise ValueError('scenario name mismatch')
    return scenario


def scenario_command(scenario: dict, profile: dict, graphics: str = 'patched-offscreen',
                     storage: dict | None = None,
                     kernel_target: str = 'vendor') -> str:
    if graphics not in ('patched-offscreen', 'mesa112'):
        raise ValueError(f'unknown graphics mode: {graphics}')
    name = scenario['name']
    if name in ('applauncher-sigstub', 'vendor-lifecycle-sigstub'):
        # These scenarios run the signature-verification-bypass launcher; the
        # conformance profile forbids application/crypto bypasses.
        if profile['name'] == 'conformance':
            raise ValueError(f'{name} requires the research profile (crypto bypass)')
    if name == 'vendor-lifecycle':
        return ('export LIFECYCLE_MANAGER=real LIFECYCLE_ACCEPT_COMPONENT=1\n' +
                (VM / 'probes' / 'launcher-lifecycle.sh').read_text())
    if name == 'vendor-lifecycle-sigstub':
        return ('export LIFECYCLE_SIGSTUB=1 LIFECYCLE_MANAGER=real LIFECYCLE_ACCEPT_COMPONENT=1\n' +
                (VM / 'probes' / 'launcher-lifecycle.sh').read_text())
    if name == 'applauncher-sigstub':
        return (VM / 'probes' / 'applauncher-sigstub.sh').read_text()
    if name == 'update-graphics':
        return (VM / 'probes' / 'graphics-diagnose.sh').read_text()
    if name == 'ui-smoke':
        return (VM / 'probes' / 'ui-smoke.sh').read_text()
    if name == 'wifi-ui':
        return (VM / 'probes' / 'wifi-ui.sh').read_text()
    if name == 'can-smoke':
        return (VM / 'probes' / 'can-smoke.sh').read_text()
    if name == 'recovery-reboot':
        return ("echo 'HARNESS_CHECK reboot-marker PASS'; sync; reboot -f\n")
    if name == 'recovery-no-space':
        return r'''set -u
dd if=/dev/zero of=/update/.no-space bs=1M count=32 2>/tmp/no-space.log
rc=$?
rm -f /update/.no-space 2>/dev/null || true
if [ "$rc" -eq 0 ]; then
    echo 'HARNESS_FAIL recovery-no-space (write unexpectedly succeeded)'
    exit 1
fi
if ! grep -qi 'no space' /tmp/no-space.log; then
    echo 'HARNESS_FAIL recovery-no-space (missing ENOSPC)'
    cat /tmp/no-space.log
    exit 1
fi
echo 'HARNESS_CHECK recovery-no-space PASS'
'''
    if name == 'vendor-systemd-dbus':
        return (VM / 'probes' / 'vendor-systemd-dbus.sh').read_text()
    if name == 'vehicle-state':
        return (VM / 'probes' / 'vehicle-state.sh').read_text()
    if name == 'recovery-corrupt-media':
        return (VM / 'probes' / 'recovery-corrupt-media.sh').read_text()
    if name == 'recovery-repeat-update':
        return (VM / 'probes' / 'recovery-repeat-update.sh').read_text()
    if name == 'recovery-usb-removal':
        return r'''set -u
modprobe ehci-pci 2>/dev/null || true
modprobe usb-storage 2>/dev/null || true
modprobe sd_mod 2>/dev/null || true
sleep 2
mkdir -p /media/usb
USB_DEV=
for dev in /dev/sd?; do
    [ -b "$dev" ] || continue
    mount -o rw "$dev" /media/usb 2>/dev/null || continue
    if [ -f /media/usb/IONIQ5_EU.ver ]; then USB_DEV=$dev; break; fi
    umount /media/usb 2>/dev/null || true
done
[ -n "$USB_DEV" ] || { echo 'HARNESS_BLOCKED USB update stick missing'; exit 77; }
echo 'HARNESS_CHECK usb-mount PASS'
echo "USB_REMOVAL_READY_$(date +%s)_$$"
sleep 4
if [ ! -b "$USB_DEV" ] || ! dd if="$USB_DEV" of=/dev/null bs=512 count=4 skip=1000 2>/tmp/usb-removed.log; then
    echo 'HARNESS_CHECK usb-removed PASS'
else
    echo 'HARNESS_FAIL usb-removed (device still readable)'
    cat /tmp/usb-removed.log 2>/dev/null
    exit 1
fi
umount -l /media/usb 2>/dev/null || true
echo 'HARNESS_CHECK usb-cleanup PASS'
echo 'HARNESS_CHECK recovery-usb-removal PASS'
'''
    if name == 'recovery-power-loss':
        return r'''set -u
printf 'POWER_LOSS_MARKER\n' > /tmp/power-loss-marker
sync
echo POWER_LOSS_READY
i=0
while [ "$i" -lt 1000000 ]; do
    echo "POWER_LOSS_LINE_$i" >> /tmp/power-loss-fill
    i=$((i+1))
done
'''
    if name == 'environment-audit':
        return audit_script()
    if name == 'storage-isolation':
        if storage is None or storage['mode'] != 'split':
            raise ValueError('storage-isolation requires --storage split')
        return probe_script(storage)
    if name == 'boot':
        if kernel_target == 'compat':
            return ('test "$(id -u)" = 0 && test -b /dev/vda && '
                    "printf 'HARNESS_CHECK boot PASS\\n'")
        return ('test "$(uname -r)" = 4.1.27-abl && test "$(id -u)" = 0 && '
                "printf 'HARNESS_CHECK boot PASS\\n'")
    # This bootstrap lives on the host: it can locate the guest tools without
    # assuming sd[a/b] ordering. Tools must include the current scenario runner.
    if kernel_target == 'compat':
        bootstrap = 'mkdir -p /media/tools\n'
    else:
        bootstrap = ('modprobe ehci-pci && modprobe usb-storage && '
                     'modprobe sd_mod || exit 1\nmkdir -p /media/tools\n')
    return bootstrap + f'''found=0
for attempt in {{1..20}}; do
  for dev in /dev/sd? /dev/sd?1; do
    [ -b "$dev" ] || continue
    mount -o ro "$dev" /media/tools 2>/dev/null || continue
    if [ -f /media/tools/scenarios.sh ]; then found=1; break; fi
    umount /media/tools || exit 1
  done
  [ "$found" = 1 ] && break
  sleep 1
done
[ "$found" = 1 ] || {{ echo 'HARNESS_FAIL missing current scenario tools'; exit 1; }}
if [ {name} = entropy-ready ]; then
  [ -x /media/tools/entropy-seed ] || {{ echo 'HARNESS_BLOCKED rebuild tools with entropy-seed'; exit 77; }}
  /media/tools/entropy-seed --rdrand
  exit $?
fi
HARNESS_PROFILE={profile['name']} USE_MESA={int(graphics == 'mesa112')} /bin/bash /media/tools/scenarios.sh {name}
'''
