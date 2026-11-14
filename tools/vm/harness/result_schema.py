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


def _check_images(value, problems: list[str]) -> None:
    if not isinstance(value, dict):
        problems.append('images must be an object')
        return
    for name, entry in value.items():
        prefix = f'images.{name}'
        if not isinstance(entry, dict):
            problems.append(f'{prefix} must be an object')
            continue
        if not isinstance(entry.get('path'), str) or not entry['path']:
            problems.append(f'{prefix}.path must be a non-empty string')
        if not _is_int(entry.get('size')) or entry['size'] < 0:
            problems.append(f'{prefix}.size must be a non-negative integer')
        for key in ('mtime_ns', 'ctime_ns'):
            if not _is_int(entry.get(key)):
                problems.append(f'{prefix}.{key} must be an integer')
        if entry.get('sha256') is not None and not isinstance(entry['sha256'], str):
            problems.append(f'{prefix}.sha256 must be a string or null')
        for key in ('fresh_digest', 'trusted_reference_verified'):
            if not isinstance(entry.get(key), bool):
                problems.append(f'{prefix}.{key} must be boolean')


def _check_overlays(value, problems: list[str]) -> None:
    if not isinstance(value, dict):
        problems.append('overlays must be an object')
        return
    for name, entry in value.items():
        prefix = f'overlays.{name}'
        if not isinstance(entry, dict):
            problems.append(f'{prefix} must be an object')
            continue
        for key in ('path', 'backing'):
            if not isinstance(entry.get(key), str) or not entry[key]:
                problems.append(f'{prefix}.{key} must be a non-empty string')
        if 'persistent' in entry and not isinstance(entry['persistent'], bool):
            problems.append(f'{prefix}.persistent must be boolean')
        if 'state_dir' in entry and not isinstance(entry['state_dir'], str):
            problems.append(f'{prefix}.state_dir must be a string')


def _check_storage(value, problems: list[str]) -> None:
    if not isinstance(value, dict):
        problems.append('storage must be an object')
        return
    if value.get('mode') not in ('flat', 'split'):
        problems.append('storage.mode must be flat or split')
    volumes = value.get('volumes')
    if not isinstance(volumes, dict):
        problems.append('storage.volumes must be an object')
    else:
        for name, volume in volumes.items():
            prefix = f'storage.volumes.{name}'
            if not isinstance(volume, dict):
                problems.append(f'{prefix} must be an object')
                continue
            for key in ('mountpoint', 'backing', 'overlay_name', 'filesystem'):
                if not isinstance(volume.get(key), str) or not volume[key]:
                    problems.append(f'{prefix}.{key} must be a non-empty string')
            if not _is_int(volume.get('size_mib')) or volume['size_mib'] <= 0:
                problems.append(f'{prefix}.size_mib must be a positive integer')
            if not isinstance(volume.get('mount_options'), str):
                problems.append(f'{prefix}.mount_options must be a string')
    if 'setup' in value and not isinstance(value['setup'], dict):
        problems.append('storage.setup must be an object')
    if ('setup_seconds' in value
            and not _is_number(value['setup_seconds'])):
        problems.append('storage.setup_seconds must be a number')
    for key in ('nonce', 'seed', 'transport'):
        if key in value and not isinstance(value[key], str):
            problems.append(f'storage.{key} must be a string')
    for key in ('synthetic', 'production_layout_verified'):
        if key in value and not isinstance(value[key], bool):
            problems.append(f'storage.{key} must be boolean')


def _check_power_loss(value, problems: list[str]) -> None:
    if not isinstance(value, dict):
        problems.append('power_loss must be an object')
        return
    for key in ('requested', 'marker_seen', 'killed'):
        if key in value and not isinstance(value[key], bool):
            problems.append(f'power_loss.{key} must be boolean')
    if 'marker' in value and (not isinstance(value['marker'], str)
                              or not value['marker']):
        problems.append('power_loss.marker must be a non-empty string')
    if 'error' in value and not isinstance(value['error'], str):
        problems.append('power_loss.error must be a string')


def _check_usb_removal(value, problems: list[str]) -> None:
    if not isinstance(value, dict):
        problems.append('usb_removal must be an object')
        return
    for key in ('requested', 'marker_seen'):
        if key in value and not isinstance(value[key], bool):
            problems.append(f'usb_removal.{key} must be boolean')
    for key in ('device_id', 'marker'):
        if key in value and (not isinstance(value[key], str)
                             or not value[key]):
            problems.append(f'usb_removal.{key} must be a non-empty string')
    if 'qmp' in value and not isinstance(value['qmp'], dict):
        problems.append('usb_removal.qmp must be an object')
    if 'error' in value and not isinstance(value['error'], str):
        problems.append('usb_removal.error must be a string')


def _check_patch(value, problems: list[str]) -> None:
    if not isinstance(value, dict):
        problems.append('patch must be an object')
        return
    attempts = value.get('connect_attempts')
    if attempts is not None and (not _is_int(attempts) or attempts < 1):
        problems.append('patch.connect_attempts must be an integer >= 1')
    for key in ('stop', 'driver_init', 'table_entry', 'original',
                'replacement', 'mode', 'kernel', 'initramfs'):
        if key in value and not isinstance(value[key], str):
            problems.append(f'patch.{key} must be a string')
    if 'stop_signal' in value and (not isinstance(value['stop_signal'], str)
                                   or not value['stop_signal']):
        problems.append('patch.stop_signal must be a non-empty string')


def _check_phase(value, problems: list[str]) -> None:
    if not isinstance(value, dict):
        problems.append('phase must be an object')
        return
    for key in ('ok', 'fail', 'appupgrade', 'launch_requested', 'usb_mount'):
        if key in value and not isinstance(value[key], bool):
            problems.append(f'phase.{key} must be boolean')
    for key in ('steps', 'timeouts', 'waits', 'dmclient_standby',
                'verify_key'):
        if key in value and not isinstance(value[key], list):
            problems.append(f'phase.{key} must be a list')


def _check_audit(value, problems: list[str]) -> None:
    if not isinstance(value, dict):
        problems.append('audit must be an object')
        return
    if 'status' in value and (not isinstance(value['status'], str)
                              or not value['status']):
        problems.append('audit.status must be a non-empty string')
    if 'path' in value and not isinstance(value['path'], str):
        problems.append('audit.path must be a string')
    for key in ('gaps', 'missing_required'):
        if key in value:
            items = value[key]
            if (not isinstance(items, list)
                    or any(not isinstance(item, str) for item in items)):
                problems.append(f'audit.{key} must be a list of strings')


def _check_screenshot(value, problems: list[str]) -> None:
    if not isinstance(value, dict):
        problems.append('screenshot must be an object')
        return
    if 'requested' in value and not isinstance(value['requested'], bool):
        problems.append('screenshot.requested must be boolean')
    if 'ok' in value and value['ok'] is not None and not isinstance(value['ok'], bool):
        problems.append('screenshot.ok must be boolean or null')
    artifacts = value.get('artifacts', [])
    if not isinstance(artifacts, list):
        problems.append('screenshot.artifacts must be a list')
    else:
        for index, artifact in enumerate(artifacts):
            prefix = f'screenshot.artifacts[{index}]'
            if not isinstance(artifact, dict):
                problems.append(f'{prefix} must be an object')
                continue
            for key in ('name', 'path', 'sha256'):
                if not isinstance(artifact.get(key), str) or not artifact[key]:
                    problems.append(f'{prefix}.{key} must be a non-empty string')
            if not _is_int(artifact.get('size')) or artifact['size'] < 0:
                problems.append(f'{prefix}.size must be a non-negative integer')
    qmp = value.get('qmp')
    if qmp is not None:
        if not isinstance(qmp, dict):
            problems.append('screenshot.qmp must be an object or null')
        else:
            for key in ('path', 'sha256'):
                if not isinstance(qmp.get(key), str) or not qmp[key]:
                    problems.append(f'screenshot.qmp.{key} must be a non-empty string')
            if not _is_int(qmp.get('size')) or qmp['size'] < 0:
                problems.append('screenshot.qmp.size must be a non-negative integer')
            for key in ('width', 'height'):
                if not _is_int(qmp.get(key)) or qmp[key] <= 0:
                    problems.append(f'screenshot.qmp.{key} must be a positive integer')
    for key in ('qmp_error', 'error'):
        if key in value and value[key] is not None and not isinstance(value[key], str):
            problems.append(f'screenshot.{key} must be a string or null')
    for key in ('qmp_socket', 'serial'):
        if key in value and not isinstance(value[key], str):
            problems.append(f'screenshot.{key} must be a string')
    pixels = value.get('qmp_pixels')
    if pixels is not None:
        if not isinstance(pixels, dict):
            problems.append('screenshot.qmp_pixels must be an object or null')
        else:
            if not _is_int(pixels.get('count')) or pixels['count'] < 0:
                problems.append('screenshot.qmp_pixels.count must be a non-negative integer')
            bbox = pixels.get('bbox')
            if bbox is not None and (
                    not isinstance(bbox, list) or len(bbox) != 4
                    or any(not _is_int(item) for item in bbox)):
                problems.append('screenshot.qmp_pixels.bbox must be four integers or null')
            for key in ('width', 'height'):
                if not _is_int(pixels.get(key)) or pixels[key] <= 0:
                    problems.append(f'screenshot.qmp_pixels.{key} must be positive')


def _check_sources(value, problems: list[str]) -> None:
    if not isinstance(value, dict):
        problems.append('harness_sources must be an object')
        return
    for name, digest in value.items():
        if not isinstance(name, str) or not name:
            problems.append('harness_sources keys must be non-empty strings')
        if not isinstance(digest, str) or not digest:
            problems.append(f'harness_sources.{name} must be a non-empty string')


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
    _check_images(result.get('images'), problems)
    _check_overlays(result.get('overlays'), problems)
    if 'harness_sources' in result:
        _check_sources(result['harness_sources'], problems)
    if 'storage' in result:
        _check_storage(result['storage'], problems)
    if result.get('power_loss') is not None:
        _check_power_loss(result['power_loss'], problems)
    if result.get('usb_removal') is not None:
        _check_usb_removal(result['usb_removal'], problems)
    if result.get('screenshot') is not None:
        _check_screenshot(result['screenshot'], problems)
    if result.get('patch') is not None:
        _check_patch(result['patch'], problems)
    if result.get('phase') is not None:
        _check_phase(result['phase'], problems)
    if result.get('audit') is not None:
        _check_audit(result['audit'], problems)
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
    path = Path(path)
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        return None, [f'cannot read result: {exc}']
    problems = validate_result(data)
    if isinstance(data, dict) and data.get('schema_version') == CURRENT_SCHEMA:
        expected = str(path.parent.resolve())
        if data.get('run_dir') != expected:
            problems.append(
                'run_dir does not match result directory: '
                f'{data.get("run_dir")!r} != {expected!r}')
    return data, problems


def schema_state(result: dict | None, problems: list[str]) -> str:
    if result is None:
        return 'missing'
    if result.get('schema_version') != CURRENT_SCHEMA:
        return 'legacy'
    return 'invalid' if problems else 'ok'


def check_all_results(vm_dir: Path) -> dict:
    """Bulk-validate every run with a result.json; missing dirs are reported."""
    root = Path(vm_dir) / 'runs'
    counts = {'valid': 0, 'legacy': 0, 'invalid': 0, 'missing': 0,
              'bench_only': 0}
    problems: list[dict] = []
    entries = []
    try:
        entries = sorted(path for path in root.iterdir() if path.is_dir())
    except OSError:
        pass
    for path in entries:
        result_file = path / 'result.json'
        if not result_file.is_file():
            if (path / 'bench.json').is_file():
                counts['bench_only'] += 1
            else:
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


def check_all_bench(vm_dir: Path) -> dict:
    """Bulk-validate every bench.json under a VM's runs/ directory."""
    return check_all_bench_dir(Path(vm_dir) / 'runs')


def check_all_bench_dir(root: Path) -> dict:
    """Bulk-validate every */bench.json under `root`."""
    root = Path(root)
    checked = valid = invalid = 0
    problems: list[dict] = []
    try:
        paths = sorted(root.glob('*/bench.json'))
    except OSError:
        paths = []
    for path in paths:
        checked += 1
        try:
            data = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            invalid += 1
            problems.append({'bench': path.parent.name,
                             'problems': [f'cannot read: {exc}']})
            continue
        document_problems = validate_bench(data)
        if document_problems:
            invalid += 1
            problems.append({'bench': path.parent.name,
                             'problems': document_problems})
        else:
            valid += 1
    return {
        'ok': invalid == 0,
        'checked': checked,
        'valid': valid,
        'invalid': invalid,
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
        seen_variants = set()
        for index, variant in enumerate(variants):
            if not isinstance(variant, dict):
                problems.append(f'variants[{index}] must be an object')
                continue
            if variant.get('cache') is not None and not isinstance(variant['cache'], str):
                problems.append(f'variants[{index}].cache must be a string or null')
            if variant.get('pin_cpu') is not None and not _is_int(variant['pin_cpu']):
                problems.append(f'variants[{index}].pin_cpu must be an integer or null')
            key = (variant.get('cache'), variant.get('pin_cpu'))
            if key in seen_variants:
                problems.append(f'variants[{index}] duplicates an earlier variant')
            seen_variants.add(key)
    scenario_names = ([item for item in scenarios if isinstance(item, str)]
                      if isinstance(scenarios, list) else [])
    declared = None
    if isinstance(variants, list):
        declared = {(item.get('cache'), item.get('pin_cpu'))
                    for item in variants if isinstance(item, dict)}
    repeat_limit = bench.get('repeats')
    rows = bench.get('rows')
    if not isinstance(rows, list):
        problems.append('rows must be a list')
    else:
        seen_rows = set()
        for index, row in enumerate(rows):
            prefix = f'rows[{index}]'
            if not isinstance(row, dict):
                problems.append(f'{prefix} must be an object')
                continue
            row_key = (row.get('scenario'), row.get('iteration'),
                       row.get('cache'), row.get('pin_cpu'))
            if row_key in seen_rows:
                problems.append(f'{prefix} duplicates an earlier row')
            seen_rows.add(row_key)
            if not isinstance(row.get('scenario'), str) or not row['scenario']:
                problems.append(f'{prefix}.scenario must be a non-empty string')
            elif scenario_names and row['scenario'] not in scenario_names:
                problems.append(f'{prefix}.scenario is not a benchmarked scenario')
            if (declared is not None
                    and (row.get('cache'), row.get('pin_cpu')) not in declared):
                problems.append(f'{prefix} is not a declared variant')
            if not _is_int(row.get('iteration')) or row['iteration'] < 1:
                problems.append(f'{prefix}.iteration must be an integer >= 1')
            elif _is_int(repeat_limit) and row['iteration'] > repeat_limit:
                problems.append(f'{prefix}.iteration exceeds repeats')
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
    warmup_limit = bench.get('warmup', 0)
    warmups = bench.get('warmups', [])
    if not isinstance(warmups, list):
        problems.append('warmups must be a list')
    else:
        seen_warmups = set()
        for index, row in enumerate(warmups):
            if not isinstance(row, dict):
                problems.append(f'warmups[{index}] must be an object')
                continue
            warm_key = (row.get('scenario'), row.get('warmup'),
                        row.get('cache'), row.get('pin_cpu'))
            if warm_key in seen_warmups:
                problems.append(f'warmups[{index}] duplicates an earlier warmup')
            seen_warmups.add(warm_key)
            if not isinstance(row.get('scenario'), str):
                problems.append(f'warmups[{index}].scenario must be a string')
            elif scenario_names and row['scenario'] not in scenario_names:
                problems.append(f'warmups[{index}].scenario is not benchmarked')
            if (declared is not None
                    and (row.get('cache'), row.get('pin_cpu')) not in declared):
                problems.append(f'warmups[{index}] is not a declared variant')
            if not _is_int(row.get('warmup')) or row['warmup'] < 1:
                problems.append(f'warmups[{index}].warmup must be an integer >= 1')
            elif _is_int(warmup_limit) and row['warmup'] > warmup_limit:
                problems.append(f'warmups[{index}].warmup exceeds warmup count')
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
