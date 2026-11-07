# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Summarize observed guest lifecycle syscalls; sending a signal is not an exit."""
from __future__ import annotations

import re


def parse_manager_registrations(text: str) -> list[dict]:
    """Pair only AppEngineBase's plane/result log lines; missing replies stay unknown."""
    observations = []
    plane = None
    for line in text.splitlines():
        if 'AppEngineBase::registrationAppMain(' not in line:
            continue
        match = re.search(r'plane\s*:\s*(\d+)\s*$', line)
        if match:
            plane = int(match[1])
        reply = re.search(r'response is:\s*QVariant\(int,\s*(-?\d+)\)', line)
        if reply and plane is not None:
            observations.append({'plane': plane, 'result': int(reply[1])})
            plane = None
    return observations


def parse_lifecycle(text: str) -> dict:
    pids = re.findall(r'(?m)^LIFECYCLE_CLIENT_PID ([0-9]+)\r?$', text)
    if len(pids) != 1:
        raise ValueError('expected one observed lifecycle client PID')
    pid = pids[0]
    signal_pattern = (r'(?m)^(\d+)\s+(?:(\d+\.\d+)\s+)?kill\(' + pid +
                      r', (SIG[A-Z0-9]+)\)\s*= 0\r?$')
    signals = [{'sender_pid': int(sender), 'guest_seconds': float(timestamp) if timestamp else None,
                'signal': signal} for sender, timestamp, signal in re.findall(signal_pattern, text)]
    reaped = re.findall(r'(?m)^.*wait4\(' + pid +
                        r', \[\{WIFSIGNALED\(s\) && WTERMSIG\(s\) == (SIG[A-Z0-9]+)\}\].*\)\s*= ' +
                        pid + r'\r?$', text)
    if len(set(reaped)) > 1:
        raise ValueError('conflicting lifecycle exit-signal observations')
    names = [item['signal'] for item in signals]
    span = None
    if len(signals) > 1 and all(item['guest_seconds'] is not None for item in signals):
        span = round((signals[-1]['guest_seconds'] - signals[0]['guest_seconds']) * 1000, 3)
    checks = dict(re.findall(r'(?m)^HARNESS_CHECK ([\w-]+) (PASS|FAIL)\r?$', text))
    registrations = parse_manager_registrations(text)
    launcher_ack = (checks.get('client-launcher-ack') == 'PASS' or
                    checks.get('client-native-registration') == 'PASS')
    return {'schema_version': 2, 'client_pid': int(pid), 'signals_sent': signals,
            'sigkill_requested': 'SIGKILL' in names, 'signal_sequence_span_ms': span,
            'reaped': bool(reaped), 'observed_exit_signal': reaped[0] if reaped else None,
            'normal_exit_observed': False if reaped else None,
            'real_manager_bus_observed': checks.get('real-manager-bus') == 'PASS',
            'manager_preload_complete_observed': checks.get('manager-preload-complete') == 'PASS',
            'launcher_native_ack_observed': launcher_ack,
            'manager_registrations': registrations,
            'manager_plane1_registration_observed': {'plane': 1, 'result': 0} in registrations,
            'ivi_shell_selected_observed': checks.get('client-ivi-shell-selected') == 'PASS',
            'appkill_bookkeeping_observed': checks.get('appkill-is-bookkeeping') == 'PASS',
            'scope': 'External Request.terminate for an on-demand AppDMClient; not general manager policy or update conformance',
            'stock_equivalence_verified': False,
            'limitations': ['Launcher acknowledgement is not manager registration or full application readiness.',
                            'Plane-specific registration results do not establish display/rendering health.',
                            'Successful kill(SIGKILL) does not prove SIGKILL caused exit; use wait4 status.',
                            'Guest timestamps do not establish physical head-unit timing.']}
