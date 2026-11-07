# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Read-only guest environment census. Observations are data, never commands."""
from __future__ import annotations

import re
import shlex

DEVICE_PATHS = ('/dev/keystore', '/dev/dri/card0', '/dev/dri/renderD128', '/dev/watchdog', '/dev/ttyS1')

SECTIONS = {
    'kernel': ['uname', '-a'],
    'cmdline': ['cat', '/proc/cmdline'],
    'pid1': ['cat', '/proc/1/comm'],
    'mounts': ['cat', '/proc/mounts'],
    'partitions': ['cat', '/proc/partitions'],
    'filesystem_space': ['df', '-Pk', '/', '/update', '/rw_data', '/log_data'],
    'processes': ['/bin/bash', '-o', 'pipefail', '-c', "ps -ef | sed -n '1,200p'"],
    'netdev': ['cat', '/proc/net/dev'],
    'smack_current': ['cat', '/proc/self/attr/current'],
    'smack_onlycap': ['cat', '/sys/fs/smackfs/onlycap'],
    'smack_bringup': ['cat', '/sys/fs/smackfs/bringup-switch'],
    'smack_rules': ['sed', '-n', '1,2000p', '/sys/fs/smackfs/load2'],
    'core_pattern': ['cat', '/proc/sys/kernel/core_pattern'],
    'entropy': ['cat', '/proc/sys/kernel/random/entropy_avail'],
    'version': ['cat', '/etc/version.txt'],
    'devices': ['/bin/bash', '-c',
                'for path in /dev/keystore /dev/dri/card0 /dev/dri/renderD128 /dev/watchdog /dev/ttyS1; do\n'
                'if [ -c "$path" ]; then echo "$path character"; elif [ -e "$path" ]; then echo "$path other"; else echo "$path absent"; fi\n'
                'done'],
    'path_metadata': ['ls', '-ldn', '/update', '/rw_data', '/log_data', '/tmp', '/var'],
    'smack_labels': ['/bin/bash', '-c',
                     'missing=0\nfor path in /update /rw_data /log_data /tmp /app/bin/AppUpgrade; do\n'
                     'echo "PATH $path"\n'
                     'attr -S -g SMACK64 "$path" 2>&1 || { echo LABEL_UNOBSERVED; missing=1; }\n'
                     'done\nexit "$missing"'],
}
REQUIRED = ('kernel', 'cmdline', 'pid1', 'mounts', 'devices')


def audit_script() -> str:
    commands = []
    for name, argv in SECTIONS.items():
        commands.append(f'audit_section {name} ' + shlex.join(argv))
    return '''set +e
audit_failed=0
audit_section() {
  name=$1; shift
  printf '\\nHARNESS_AUDIT_BEGIN %s\\n' "$name"
  "$@" 2>&1
  status=$?
  printf '\\nHARNESS_AUDIT_END %s %s\\n' "$name" "$status"
  case "$name" in kernel|cmdline|pid1|mounts|devices) [ "$status" = 0 ] || audit_failed=1 ;; esac
}
''' + '\n'.join(commands) + '''
if [ "$audit_failed" = 0 ]; then echo 'HARNESS_CHECK environment-audit PASS'; exit 0; fi
echo 'HARNESS_FAIL required environment observations failed'
exit 1
'''


def parse_audit(text: str) -> dict:
    observations = {}
    cursor = 0
    for name in SECTIONS:
        begin = list(re.finditer(r'(?m)^HARNESS_AUDIT_BEGIN ' + name + r'\r?\n', text))
        end = list(re.finditer(r'(?m)^HARNESS_AUDIT_END ' + name + r' ([0-9]{1,3})\r?$', text))
        if (len(begin) != 1 or len(end) != 1 or begin[0].start() < cursor or
                end[0].start() < begin[0].end() or int(end[0].group(1)) > 255):
            raise ValueError(f'missing, duplicate or malformed audit section: {name}')
        cursor = end[0].end()
        observations[name] = {'rc': int(end[0].group(1)),
                              'text': text[begin[0].end():end[0].start()].rstrip('\r\n')}
    mounts = []
    for line in observations['mounts']['text'].splitlines():
        fields = line.split()
        if len(fields) == 6:
            mounts.append({'source': fields[0], 'target': fields[1],
                           'filesystem': fields[2], 'options': fields[3].split(',')})
    devices = {}
    for line in observations['devices']['text'].splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] in DEVICE_PATHS and fields[1] in ('character', 'other', 'absent'):
            if fields[0] in devices:
                raise ValueError(f'duplicate device observation: {fields[0]}')
            devices[fields[0]] = fields[1]
    writable = {point: next((item for item in mounts if item['target'] == point), None)
                for point in ('/update', '/rw_data', '/log_data')}
    gaps = []
    if observations['pid1']['text'].strip() != 'systemd':
        gaps.append('PID 1 is not systemd; stock service ordering is not established.')
    if not all(writable.values()):
        gaps.append('One or more writable-data paths are not separate mountpoints.')
    if devices.get('/dev/keystore') == 'absent':
        gaps.append('Expected /dev/keystore device is absent; this does not enumerate all possible crypto backends.')
    if all(devices.get(path) == 'absent' for path in ('/dev/dri/card0', '/dev/dri/renderD128')):
        gaps.append('Inspected DRM devices are absent; software rendering is a separate question.')
    if observations['smack_rules']['rc'] != 0:
        gaps.append('Rules from the inspected SMACK load2 interface were not observed.')
    elif not observations['smack_rules']['text'].strip():
        gaps.append('The inspected SMACK load2 interface returned no rules; enforcement state is not established by this alone.')
    if observations['smack_bringup']['rc'] == 0 and observations['smack_bringup']['text'].strip() == '1':
        gaps.append('The inspected SMACK bring-up switch reports enabled.')
    if observations['smack_labels']['rc'] != 0:
        gaps.append('SMACK labels on one or more inspected paths were not observed.')
    if observations['entropy']['rc'] == 0 and observations['entropy']['text'].strip() == '0':
        gaps.append('The kernel reports zero credited entropy; RNG initialization is not established and getrandom may block.')
    gaps.append('Production partition layout, initial data, and SMACK policy equivalence are not verified.')
    missing_required = [name for name in REQUIRED
                        if observations[name]['rc'] != 0 or not observations[name]['text'].strip()]
    if set(devices) != set(DEVICE_PATHS) and 'devices' not in missing_required:
        missing_required.append('devices')
    return {'schema_version': 1, 'observations': observations, 'mounts': mounts,
            'writable_mounts': writable, 'devices': devices, 'gaps': gaps,
            'missing_required': missing_required, 'stock_equivalence_verified': False,
            'coverage': {'processes': 'first 200 lines', 'smack_rules': 'first 2000 lines',
                         'devices': list(devices), 'labels': 'selected paths only'}}
