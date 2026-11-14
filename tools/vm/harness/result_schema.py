# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Result.json contract validation.

The validator is intentionally lenient about additive fields: older run
results remain readable, while missing/wrong core fields are reported.
"""
from __future__ import annotations

import json
import math
from pathlib import Path

CURRENT_SCHEMA = 3
VERDICTS = {'PASS', 'FAIL', 'BLOCKED', 'DRY-RUN'}
CHECK_STATES = {'PASS', 'FAIL'}


def _is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _is_number(value) -> bool:
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value))


def _check_dict(value, name: str, problems: list[str]) -> None:
    if not isinstance(value, dict):
        problems.append(f'{name} must be an object')


def validate_result(result) -> list[str]:
    """Return contract problems; an empty list means the core schema is valid."""
    problems: list[str] = []
    if not isinstance(result, dict):
        return ['result must be a JSON object']
    version = result.get('schema_version')
    if not _is_int(version) or version < 1:
        problems.append('schema_version must be an integer >= 1')
    if not isinstance(result.get('run_dir'), str) or not result['run_dir']:
        problems.append('run_dir must be a non-empty string')
    verdict = result.get('verdict')
    if verdict not in VERDICTS:
        problems.append(f'verdict must be one of {sorted(VERDICTS)}')
    if not _is_int(result.get('rc')):
        problems.append('rc must be an integer')
    timeout = result.get('timeout')
    if not _is_number(timeout) or timeout <= 0:
        problems.append('timeout must be a positive finite number')
    if result.get('cmd') is not None and not isinstance(result['cmd'], str):
        problems.append('cmd must be a string or null')
    if result.get('error') is not None and not isinstance(result['error'], str):
        problems.append('error must be a string or null')
    if result.get('guest_rc') is not None and not _is_int(result['guest_rc']):
        problems.append('guest_rc must be an integer or null')
    for name in ('boot_seconds', 'readiness_seconds', 'scenario_seconds'):
        value = result.get(name)
        if value is not None and not _is_number(value):
            problems.append(f'{name} must be a finite number or null')
    _check_dict(result.get('images'), 'images', problems)
    _check_dict(result.get('overlays'), 'overlays', problems)
    profile = result.get('profile')
    if not isinstance(profile, dict) or not isinstance(profile.get('name'), str):
        problems.append('profile.name must be a string')
    scenario = result.get('scenario')
    if scenario is not None:
        if not isinstance(scenario, dict):
            problems.append('scenario must be an object or null')
        elif not isinstance(scenario.get('name'), str):
            problems.append('scenario.name must be a string')
    checks = result.get('checks')
    if checks is not None:
        if not isinstance(checks, dict):
            problems.append('checks must be an object')
        else:
            for name, state in checks.items():
                if not isinstance(name, str) or state not in CHECK_STATES:
                    problems.append(f'checks.{name} must be PASS or FAIL')
    substitutions = result.get('applied_substitutions', [])
    if not isinstance(substitutions, list) or any(
            not isinstance(item, str) for item in substitutions):
        problems.append('applied_substitutions must be a list of strings')
    if 'dry_run' in result and not isinstance(result['dry_run'], bool):
        problems.append('dry_run must be boolean')
    if 'pin_cpu' in result and result['pin_cpu'] is not None and not _is_int(result['pin_cpu']):
        problems.append('pin_cpu must be an integer or null')
    if 'lock' in result and result['lock'] is not None:
        _check_dict(result['lock'], 'lock', problems)
    if 'failure_bundle' in result and result['failure_bundle'] is not None:
        _check_dict(result['failure_bundle'], 'failure_bundle', problems)
    return problems


def validate_file(path: Path) -> tuple[dict | None, list[str]]:
    try:
        data = json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError) as exc:
        return None, [f'cannot read result: {exc}']
    return data, validate_result(data)


def schema_state(result: dict | None, problems: list[str]) -> str:
    if result is None:
        return 'missing'
    if result.get('schema_version') != CURRENT_SCHEMA:
        return 'legacy'
    return 'invalid' if problems else 'ok'


def check_all_results(vm_dir: Path) -> dict:
    """Bulk-validate every run with a result.json; missing dirs are reported."""
    root = Path(vm_dir) / 'runs'
    counts = {'valid': 0, 'legacy': 0, 'invalid': 0, 'missing': 0}
    problems: list[dict] = []
    entries = []
    try:
        entries = sorted(path for path in root.iterdir() if path.is_dir())
    except OSError:
        pass
    for path in entries:
        result_file = path / 'result.json'
        if not result_file.is_file():
            counts['missing'] += 1
            continue
        data, validation = validate_file(result_file)
        if data is None:
            state = 'invalid'
        else:
            state = schema_state(data, validation)
        key = 'valid' if state == 'ok' else state
        counts[key if key in counts else 'invalid'] += 1
        if state == 'invalid':
            problems.append({'run': path.name, 'problems': validation})
    return {
        'ok': not problems,
        'checked': counts['valid'] + counts['legacy'] + counts['invalid'],
        **counts,
        'problem_count': len(problems),
        'problems': problems[:100],
    }
