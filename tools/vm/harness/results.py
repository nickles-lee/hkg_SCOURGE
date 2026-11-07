# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Evidence-based verdicts. Launch requests are not readiness evidence."""
from __future__ import annotations

import re


ANSI_ESCAPE = re.compile(r'\x1b\[[0-9;?]*[A-Za-z]')


def systemd_events(text: str) -> dict:
    """Ordering/failure evidence from a systemd serial log, ANSI-stripped."""
    clean = ANSI_ESCAPE.sub('', text.replace('\r', ''))
    # Evidence only; capped so a noisy boot cannot bloat result.json.
    return {
        'started_units': sorted(set(re.findall(r'Started (.+)\.$', clean, re.M)))[:50],
        'reached_targets': sorted(set(re.findall(r'Reached target (.+)\.$', clean, re.M)))[:50],
        'failed_unit_descriptions': sorted(
            set(re.findall(r'Failed to start (.+)\.$', clean, re.M)))[:50],
    }


def evaluate_systemd_transition(scenario: dict, prep_rc: int | None,
                                prep_output: str, serial_text: str) -> dict:
    """Evaluate a research-shell -> systemd handoff without a live shell.

    The preparation check is explicit; every required marker maps to a
    scenario check, and an optional ordering rule proves one marker appeared
    before another in the same serial window. Missing evidence fails closed.
    """
    transition = scenario.get('systemd_transition') or {}
    checks = {name: 'FAIL' for name in scenario.get('checks', [])}
    failures: list[str] = []
    prep_ok = prep_rc == 0 and 'P28_PREP_OK' in prep_output
    if 'systemd-prep' in checks:
        checks['systemd-prep'] = 'PASS' if prep_ok else 'FAIL'
    if not prep_ok:
        failures.append('systemd preparation failed')

    shell = re.search(r'P28_SHELL pid=(\d+) ppid=(\d+)', serial_text)
    shell_pid1 = bool(shell and shell.group(1) == '1')
    if 'research-shell-pid1' in checks:
        checks['research-shell-pid1'] = 'PASS' if shell_pid1 else 'FAIL'
    if not shell_pid1:
        failures.append('systemd handoff did not run from PID 1')

    markers = transition.get('required_markers') or {}
    marker_evidence: dict[str, bool] = {}
    for marker, check in markers.items():
        present = marker in serial_text
        marker_evidence[marker] = present
        if check in checks:
            checks[check] = 'PASS' if present else 'FAIL'
        if not present:
            failures.append(f'systemd marker not reached: {marker}')

    ordering = transition.get('ordering') or {}
    ordering_ok: bool | None = None
    if ordering:
        before_text = str(ordering.get('before', ''))
        after_text = str(ordering.get('after', ''))
        before = serial_text.find(before_text) if before_text else -1
        after = serial_text.find(after_text) if after_text else -1
        ordering_ok = before >= 0 and after >= 0 and before < after
        check = ordering.get('check')
        if check in checks:
            checks[check] = 'PASS' if ordering_ok else 'FAIL'
        if not ordering_ok:
            failures.append('systemd ordering not observed: '
                            f'{before_text} before {after_text}')

    if 'Kernel panic' in serial_text:
        failures.append('kernel panic')
    missing_checks = sorted(name for name, status in checks.items()
                            if status != 'PASS')
    verdict = 'FAIL' if failures or missing_checks else 'PASS'
    return {
        'checks': checks,
        'failures': failures,
        'missing_checks': missing_checks,
        'markers': marker_evidence,
        'ordering': ordering_ok,
        'exec_shell': ({'pid': int(shell.group(1)), 'ppid': int(shell.group(2))}
                       if shell else None),
        **systemd_events(serial_text),
        'verdict': verdict,
        'rc': 1 if verdict == 'FAIL' else 0,
    }


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
