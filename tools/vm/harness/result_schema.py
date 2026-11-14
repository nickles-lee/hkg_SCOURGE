# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Result and benchmark JSON contract validation.

The validators are intentionally lenient about additive fields: older
artifacts remain readable, while missing/wrong core fields are reported.
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
    rc = result.get('rc')
    if not _is_int(rc):
        problems.append('rc must be an integer')
    elif verdict in ('PASS', 'DRY-RUN') and rc != 0:
        problems.append(f'{verdict} requires rc == 0')
    elif verdict == 'BLOCKED' and rc != 77:
        problems.append('BLOCKED requires rc == 77')
    elif verdict == 'FAIL' and rc == 0:
        problems.append('FAIL requires a nonzero rc')
    blockers = result.get('blockers')
    if verdict == 'BLOCKED':
        if (not isinstance(blockers, list) or not blockers
                or any(not isinstance(item, str) or not item
                       for item in blockers)):
            problems.append('BLOCKED requires a non-empty blockers list')
    elif blockers is not None and not isinstance(blockers, list):
        problems.append('blockers must be a list')
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


BENCH_VERDICTS = VERDICTS


def _summary_ok(value) -> bool:
    if not isinstance(value, dict):
        return False
    if not value:
        return True
    return (_is_int(value.get('count')) and value.get('count', -1) >= 0
            and all(_is_number(value.get(key))
                    for key in ('mean', 'median', 'min', 'max')))


def validate_bench(bench) -> list[str]:
    """Validate a bench.json document; legacy docs may omit newer fields."""
    problems: list[str] = []
    if not isinstance(bench, dict):
        return ['bench must be a JSON object']
    modern = 'schema_version' in bench
    if modern and (not _is_int(bench['schema_version']) or bench['schema_version'] < 1):
        problems.append('schema_version must be an integer >= 1')
    if modern:
        if not isinstance(bench.get('run'), str) or not bench['run']:
            problems.append('run must be a non-empty string')
    elif bench.get('run') is not None and not isinstance(bench['run'], str):
        problems.append('run must be a string or null')
    if not isinstance(bench.get('ok'), bool):
        problems.append('ok must be boolean')
    if not _is_int(bench.get('repeats')) or bench['repeats'] < 1:
        problems.append('repeats must be an integer >= 1')
    warmup = bench.get('warmup', 0)
    if not _is_int(warmup) or warmup < 0:
        problems.append('warmup must be a non-negative integer')
    scenarios = bench.get('scenarios')
    if (not isinstance(scenarios, list) or not scenarios
            or any(not isinstance(item, str) or not item for item in scenarios)):
        problems.append('scenarios must be a non-empty list of strings')
    variants = bench.get('variants')
    if variants is None and not modern:
        pass
    elif not isinstance(variants, list):
        problems.append('variants must be a list')
    else:
        for index, variant in enumerate(variants):
            if not isinstance(variant, dict):
                problems.append(f'variants[{index}] must be an object')
                continue
            if variant.get('cache') is not None and not isinstance(variant['cache'], str):
                problems.append(f'variants[{index}].cache must be a string or null')
            if variant.get('pin_cpu') is not None and not _is_int(variant['pin_cpu']):
                problems.append(f'variants[{index}].pin_cpu must be an integer or null')
    rows = bench.get('rows')
    if not isinstance(rows, list):
        problems.append('rows must be a list')
    else:
        for index, row in enumerate(rows):
            prefix = f'rows[{index}]'
            if not isinstance(row, dict):
                problems.append(f'{prefix} must be an object')
                continue
            if not isinstance(row.get('scenario'), str) or not row['scenario']:
                problems.append(f'{prefix}.scenario must be a non-empty string')
            if not _is_int(row.get('iteration')) or row['iteration'] < 1:
                problems.append(f'{prefix}.iteration must be an integer >= 1')
            if row.get('verdict') not in BENCH_VERDICTS:
                problems.append(f'{prefix}.verdict must be one of {sorted(BENCH_VERDICTS)}')
            if not _is_int(row.get('rc')):
                problems.append(f'{prefix}.rc must be an integer')
            if not isinstance(row.get('log'), str) or not row['log']:
                problems.append(f'{prefix}.log must be a non-empty string')
            if row.get('cache') is not None and not isinstance(row['cache'], str):
                problems.append(f'{prefix}.cache must be a string or null')
            if row.get('pin_cpu') is not None and not _is_int(row['pin_cpu']):
                problems.append(f'{prefix}.pin_cpu must be an integer or null')
            if row.get('result') is not None and not isinstance(row['result'], str):
                problems.append(f'{prefix}.result must be a string or null')
            for metric in ('boot_seconds', 'readiness_seconds', 'scenario_seconds'):
                if row.get(metric) is not None and not _is_number(row[metric]):
                    problems.append(f'{prefix}.{metric} must be a number or null')
    summary = bench.get('summary')
    if not isinstance(summary, dict):
        problems.append('summary must be an object')
    else:
        for key, value in summary.items():
            if not isinstance(key, str) or not _summary_ok(value):
                problems.append(f'summary[{key!r}] must be a metric summary')
    metrics = bench.get('metrics')
    if metrics is not None:
        if not isinstance(metrics, dict):
            problems.append('metrics must be an object')
        else:
            for key, values in metrics.items():
                if not isinstance(values, dict):
                    problems.append(f'metrics[{key!r}] must be an object')
                    continue
                for metric, value in values.items():
                    if metric not in ('boot_seconds', 'readiness_seconds', 'scenario_seconds'):
                        problems.append(f'metrics[{key!r}].{metric} is unknown')
                    elif not _summary_ok(value):
                        problems.append(f'metrics[{key!r}].{metric} must be a summary')
    warmups = bench.get('warmups', [])
    if not isinstance(warmups, list):
        problems.append('warmups must be a list')
    else:
        for index, row in enumerate(warmups):
            if not isinstance(row, dict):
                problems.append(f'warmups[{index}] must be an object')
                continue
            if not isinstance(row.get('scenario'), str):
                problems.append(f'warmups[{index}].scenario must be a string')
            if not _is_int(row.get('warmup')) or row['warmup'] < 1:
                problems.append(f'warmups[{index}].warmup must be an integer >= 1')
            if row.get('verdict') not in BENCH_VERDICTS:
                problems.append(f'warmups[{index}].verdict is invalid')
            if not _is_int(row.get('rc')):
                problems.append(f'warmups[{index}].rc must be an integer')
    source = bench.get('source')
    if source is not None:
        if not isinstance(source, dict):
            problems.append('source must be an object')
        else:
            if source.get('git_head') is not None and not isinstance(source['git_head'], str):
                problems.append('source.git_head must be a string or null')
            if source.get('dirty') is not None and not isinstance(source['dirty'], bool):
                problems.append('source.dirty must be boolean or null')
    if 'lock' in bench and bench['lock'] is not None and not isinstance(bench['lock'], dict):
        problems.append('lock must be an object or null')
    return problems
