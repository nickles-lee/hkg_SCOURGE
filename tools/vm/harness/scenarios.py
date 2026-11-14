# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Static scenario-contract validation used by `launch.py doctor` and tests."""
from __future__ import annotations

import json
import math
from pathlib import Path

from .config import SCENARIO_NAMES

ALLOWED_REQUIRES = {'usb', 'tools', 'payload'}
ALLOWED_MODES = {'vendor', 'compat'}
ALLOWED_BLOCKERS = {'research', 'conformance'}


def _probe_problem(vm_dir: Path, probe: str) -> str | None:
    if not isinstance(probe, str) or not probe:
        return 'known_issue.probe must be a non-empty string'
    if probe == 'RUNBOOK.md':
        candidates = [vm_dir / 'docs' / 'RUNBOOK.md', vm_dir / 'RUNBOOK.md']
        if not any(path.is_file() for path in candidates):
            return 'known_issue.probe RUNBOOK.md is missing in both layouts'
        return None
    if probe.startswith('probes/') and probe.endswith('.md'):
        return f'known_issue.probe {probe} is excluded from the public sync'
    if not (vm_dir / probe).is_file():
        return f'known_issue.probe {probe} does not resolve'
    return None


def validate_scenario(name: str, data: dict, vm_dir: Path) -> list[str]:
    problems: list[str] = []
    if not isinstance(data, dict):
        return ['scenario must be a JSON object']
    if data.get('name') != name:
        problems.append(f"name {data.get('name')!r} does not match {name!r}")
    if not isinstance(data.get('description'), str) or not data['description'].strip():
        problems.append('description must be a non-empty string')
    checks = data.get('checks')
    if not isinstance(checks, list) or not checks:
        problems.append('checks must be a non-empty list')
    elif any(not isinstance(item, str) or not item for item in checks):
        problems.append('checks entries must be non-empty strings')
    elif len(set(checks)) != len(checks):
        problems.append('checks must be unique')
    requires = data.get('requires', [])
    if not isinstance(requires, list) or set(requires) - ALLOWED_REQUIRES:
        problems.append(f'requires must be a subset of {sorted(ALLOWED_REQUIRES)}')
    mode = data.get('requires_mode')
    if mode is not None and mode not in ALLOWED_MODES:
        problems.append(f'requires_mode must be one of {sorted(ALLOWED_MODES)}')
    blockers = data.get('blockers', {})
    if not isinstance(blockers, dict) or set(blockers) - ALLOWED_BLOCKERS:
        problems.append(f'blockers keys must be a subset of {sorted(ALLOWED_BLOCKERS)}')
    elif any(not isinstance(value, list) for value in blockers.values()):
        problems.append('blockers values must be lists')
    if data.get('requires_storage') not in (None, 'flat', 'split'):
        problems.append('requires_storage must be flat or split')
    sizes = data.get('storage_size', {})
    if not isinstance(sizes, dict):
        problems.append('storage_size must be an object')
    elif any(not isinstance(value, int) or not 16 <= value <= 8192
             for value in sizes.values()):
        problems.append('storage_size values must be integers in 16..8192 MiB')
    boot_marker = data.get('boot_marker')
    if boot_marker is not None and (not isinstance(boot_marker, str)
                                    or not boot_marker.strip()):
        problems.append('boot_marker must be a non-empty string')
    for flag in ('expect_qemu_exit', 'requires_visible_frame', 'requires_share'):
        if flag in data and not isinstance(data[flag], bool):
            problems.append(f'{flag} must be boolean')
    if data.get('expect_qemu_exit'):
        for field in ('expect_qemu_exit_marker', 'expect_qemu_exit_check'):
            value = data.get(field)
            if not isinstance(value, str) or not value:
                problems.append(f'expect_qemu_exit requires {field}')
    expected_fail = data.get('expected_fail')
    if expected_fail is not None and not (
            isinstance(expected_fail, bool)
            or (isinstance(expected_fail, dict) and expected_fail)):
        problems.append('expected_fail must be boolean or a non-empty object')
    for name in ('power_loss', 'usb_removal'):
        value = data.get(name)
        if value is None:
            continue
        if not isinstance(value, dict):
            problems.append(f'{name} must be an object')
            continue
        marker = value.get('marker')
        if not isinstance(marker, str) or not marker:
            problems.append(f'{name}.marker must be a non-empty string')
        delay = value.get('delay')
        if delay is not None and (
                not isinstance(delay, (int, float)) or isinstance(delay, bool)
                or not math.isfinite(delay) or delay < 0):
            problems.append(f'{name}.delay must be a non-negative finite number')
        if name == 'usb_removal':
            device = value.get('device_id')
            if not isinstance(device, str) or not device:
                problems.append('usb_removal.device_id must be a non-empty string')
    settle = data.get('settle_seconds')
    if settle is not None and (
            not isinstance(settle, (int, float)) or isinstance(settle, bool)
            or not math.isfinite(settle) or settle < 0):
        problems.append('settle_seconds must be a non-negative finite number')
    known = data.get('known_issue') or {}
    if known:
        if not isinstance(known, dict):
            problems.append('known_issue must be an object')
        elif known.get('probe'):
            problem = _probe_problem(vm_dir, known['probe'])
            if problem:
                problems.append(problem)
    if data.get('init_override') and data.get('systemd_transition'):
        problems.append('init_override and systemd_transition are mutually exclusive')
    if data.get('init_override') and not data.get('boot_marker'):
        problems.append('init_override requires boot_marker')
    transition = data.get('systemd_transition')
    if transition:
        if not isinstance(transition, dict):
            problems.append('systemd_transition must be an object')
        else:
            markers = transition.get('required_markers')
            if not isinstance(markers, dict) or not markers:
                problems.append('systemd_transition.required_markers must be non-empty')
            elif any(not isinstance(k, str) or not isinstance(v, str)
                     for k, v in markers.items()):
                problems.append('systemd_transition markers must be string pairs')
            ordering = transition.get('ordering')
            if ordering is not None and (
                    not isinstance(ordering, dict)
                    or not ordering.get('before') or not ordering.get('after')
                    or not ordering.get('check')):
                problems.append('systemd_transition.ordering requires before/after/check')
    return problems


def scenario_state(vm_dir: Path) -> dict:
    """Validate every registered scenario and flag unregistered JSON files."""
    vm_dir = Path(vm_dir)
    root = vm_dir / 'scenarios'
    problems: list[str] = []
    count = 0
    for name in SCENARIO_NAMES:
        path = root / f'{name}.json'
        if not path.is_file():
            problems.append(f'{name}: missing {path.name}')
            continue
        try:
            data = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            problems.append(f'{name}: cannot read: {exc}')
            continue
        count += 1
        problems.extend(f'{name}: {problem}'
                        for problem in validate_scenario(name, data, vm_dir))
    try:
        extra = sorted(path.stem for path in root.glob('*.json')
                       if path.stem not in SCENARIO_NAMES)
    except OSError:
        extra = []
    problems.extend(f'unregistered scenario file: {name}' for name in extra)
    return {'ok': not problems, 'count': count, 'problems': problems,
            'unregistered': extra}


PROFILE_NAMES = ('research', 'conformance')
ALLOWED_WORKAROUNDS = {
    'dmclient-gui-patch', 'getkeyfile-success-patch', 'decrypt-pass-through',
    'plaintext-update', 'applauncher-sigstub',
}


def validate_profile(name: str, data) -> list[str]:
    problems: list[str] = []
    if not isinstance(data, dict):
        return ['profile must be a JSON object']
    if data.get('name') != name:
        problems.append(f"name {data.get('name')!r} does not match {name!r}")
    description = data.get('description')
    if not isinstance(description, str) or not description.strip():
        problems.append('description must be a non-empty string')
    workarounds = data.get('allowed_workarounds')
    if not isinstance(workarounds, dict):
        problems.append('allowed_workarounds must be an object')
    elif set(workarounds) - ALLOWED_WORKAROUNDS:
        problems.append('allowed_workarounds has unknown keys')
    elif any(not isinstance(value, bool) for value in workarounds.values()):
        problems.append('allowed_workarounds values must be boolean')
    elif name == 'conformance' and any(workarounds.values()):
        problems.append('conformance profile cannot permit workarounds')
    limitations = data.get('limitations')
    if (not isinstance(limitations, list) or not limitations
            or any(not isinstance(item, str) or not item.strip()
                   for item in limitations)):
        problems.append('limitations must be a non-empty list of strings')
    return problems


def profile_state(vm_dir: Path) -> dict:
    """Validate the research/conformance profiles and flag extras."""
    vm_dir = Path(vm_dir)
    root = vm_dir / 'profiles'
    problems: list[str] = []
    count = 0
    for name in PROFILE_NAMES:
        path = root / f'{name}.json'
        if not path.is_file():
            problems.append(f'{name}: missing {path.name}')
            continue
        try:
            data = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            problems.append(f'{name}: cannot read: {exc}')
            continue
        count += 1
        problems.extend(f'{name}: {problem}'
                        for problem in validate_profile(name, data))
    try:
        extra = sorted(path.stem for path in root.glob('*.json')
                       if path.stem not in PROFILE_NAMES)
    except OSError:
        extra = []
    problems.extend(f'unregistered profile file: {name}' for name in extra)
    return {'ok': not problems, 'count': count, 'problems': problems,
            'unregistered': extra}
