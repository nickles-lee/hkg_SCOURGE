# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Evidence-based verdicts. Launch requests are not readiness evidence."""
from __future__ import annotations

import re


def phase_summary(text: str) -> dict:
    return {
        'ok': bool(re.search(r'(?m)^PHASE4_OK\r?$', text)),
        'fail': bool(re.search(r'(?m)^PHASE4_FAIL(?:\s|$)', text)),
        'steps': re.findall(r'(?m)^PHASE4_STEP ([^\r\n]+)', text),
        'timeouts': re.findall(r'(?m)^PHASE4_WAIT_TIMEOUT \[(.+?)\]', text),
        'waits': re.findall(r'(?m)^PHASE4_WAIT_OK \[(.+?)\]', text),
        'dmclient_standby': re.findall(r'slotOnStandbyStatus.*?status=\s+(\d+)', text),
        'verify_key': re.findall(r'verifyKeyFile\s*=\s*(\w+)', text),
        'appupgrade': 'APPLAUNCHER_STUB_EXEC_OK' in text,
        'launch_requested': 'APPLAUNCHER_STUB_LAUNCH' in text or 'AppUpgrade is executed' in text,
        'usb_mount': 'USB mount path: "/media/usb"' in text,
    }


def evaluate(text: str, guest_rc: int, scenario: dict | None = None,
             profile: dict | None = None, context: dict | None = None) -> dict:
    phase = phase_summary(text)
    check_records = re.findall(r'(?m)^HARNESS_CHECK ([\w-]+) (PASS|FAIL)\r?$', text)
    checks = dict(check_records)
    substitutions = sorted(set(re.findall(r'(?m)^HARNESS_SUBSTITUTION ([\w-]+)\r?$', text)))
    failures = re.findall(r'(?m)^(?:HARNESS_FAIL|RUN_ERROR:|PHASE4_FAIL)[ \t]*([^\r\n]*)', text)
    failures.extend(f'check failed: {name}' for name, status in check_records if status == 'FAIL')
    for name in substitutions:
        if (profile or {}).get('allowed_workarounds', {}).get(name) is False:
            failures.append(f'profile forbids substitution: {name}')
    missing = [name for name in (scenario or {}).get('checks', []) if checks.get(name) != 'PASS']
    blocked = re.findall(r'(?m)^HARNESS_BLOCKED ([^\r\n]+)', text)
    runtime_errors = re.findall(r'(?m)^[^\r\n]*(?:Segmentation fault|Aborted|APPLAUNCHER_STUB_EXEC_FAIL)[^\r\n]*', text)
    if guest_rc == 77 and blocked and not failures and not runtime_errors:
        verdict = 'BLOCKED'
    elif guest_rc != 0 or blocked or failures or runtime_errors or phase['fail'] or phase['timeouts'] or missing:
        verdict = 'FAIL'
    else:
        verdict = 'PASS'
    expected = (scenario or {}).get('expected_fail', False)
    if isinstance(expected, dict):
        expected = all((context or {}).get(key) == value for key, value in expected.items())
    expected = bool(expected)
    return {'verdict': verdict, 'checks': checks, 'missing_checks': missing,
            'failures': failures, 'blockers': blocked, 'phase': phase,
            'runtime_errors': runtime_errors,
            'applied_substitutions': substitutions,
            'expected_fail': expected,
            'known_failure': expected and verdict == 'FAIL',
            'known_issue': (scenario or {}).get('known_issue')}
