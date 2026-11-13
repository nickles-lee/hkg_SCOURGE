#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Repeatable managed-boot benchmark.

Runs a scenario N times under the global VM lock and writes per-run timings
plus mean/median/min/max to `runs/bench-<id>/bench.json`. Children use
`--no-lock` because this process holds the lock.
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import subprocess
import sys
import time
from pathlib import Path

from harness.config import load_scenario
from harness.governor import LockTimeout, run_lock
from harness.storage import parse_sizes

VM = Path(__file__).resolve().parent


def summarize(values: list[float]) -> dict:
    if not values:
        return {}
    return {
        'count': len(values),
        'mean': round(statistics.fmean(values), 3),
        'median': round(statistics.median(values), 3),
        'min': round(min(values), 3),
        'max': round(max(values), 3),
    }


def build_argv(args, scenario: dict) -> list[str]:
    argv = [
        sys.executable, '-B', str(VM / 'launch.py'), 'run', '--no-lock',
        '--profile', args.profile, '--scenario', scenario['name'],
        '--timeout', str(args.timeout), '--storage', args.storage,
    ]
    for value in args.storage_size:
        argv.extend(['--storage-size', value])
    if args.cache:
        argv.extend(['--cache', args.cache])
    if args.smp:
        argv.extend(['--smp', str(args.smp)])
    if args.memory:
        argv.extend(['--memory', str(args.memory)])
    if args.pin_cpu is not None:
        argv.extend(['--pin-cpu', str(args.pin_cpu)])
    if args.apic:
        argv.append('--apic')
    for requirement in scenario['requires']:
        value = getattr(args, requirement)
        if value is None:
            raise ValueError(f"{scenario['name']} requires --{requirement}")
        argv.extend([f'--{requirement}', str(value)])
    return argv


def result_path(log_path: Path) -> Path | None:
    for line in log_path.read_text().splitlines():
        if line.startswith('run: '):
            return Path(line[5:].strip()) / 'result.json'
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario', action='append', default=[],
                        help='scenario to benchmark (repeatable; default boot)')
    parser.add_argument('--profile', choices=('research', 'conformance'),
                        default='conformance')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--lock-timeout', type=float, default=600)
    parser.add_argument('--storage', choices=('flat', 'split'), default='flat')
    parser.add_argument('--storage-size', action='append', default=[])
    parser.add_argument('--cache', choices=('writeback', 'writethrough', 'directsync',
                                            'unsafe', 'none'), default=None)
    parser.add_argument('--smp', type=int, choices=(1, 2, 4), default=None)
    parser.add_argument('--memory', type=int, default=None)
    parser.add_argument('--pin-cpu', type=int, default=None)
    parser.add_argument('--apic', action='store_true')
    parser.add_argument('--usb', type=Path, default=None)
    parser.add_argument('--tools', type=Path, default=None)
    parser.add_argument('--payload', type=Path, default=None)
    parser.add_argument('--share', type=Path, default=None)
    args = parser.parse_args()
    if args.repeats <= 0 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('--repeats and --timeout must be finite and positive')
    names = args.scenario or ['boot']
    try:
        scenarios = [load_scenario(name) for name in names]
        parse_sizes(args.storage_size)
        # Validate required inputs and argv before taking the lock.
        for scenario in scenarios:
            build_argv(args, scenario)
    except ValueError as exc:
        parser.error(str(exc))
    try:
        with run_lock(VM, args.lock_timeout) as lock_info:
            directory = VM / 'runs' / f'bench-{time.time_ns()}'
            directory.mkdir(parents=True)
            rows = []
            for scenario in scenarios:
                for iteration in range(1, args.repeats + 1):
                    log = directory / f"{scenario['name']}-{iteration}.log"
                    with log.open('w') as output:
                        completed = subprocess.run(
                            build_argv(args, scenario), stdout=output,
                            stderr=subprocess.STDOUT)
                    path = result_path(log)
                    detail = (json.loads(path.read_text())
                              if path is not None and path.is_file() else {})
                    rows.append({
                        'scenario': scenario['name'],
                        'iteration': iteration,
                        'verdict': detail.get('verdict', 'FAIL'),
                        'rc': completed.returncode,
                        'boot_seconds': detail.get('boot_seconds'),
                        'readiness_seconds': detail.get('readiness_seconds'),
                        'result': str(path) if path is not None else None,
                        'log': str(log),
                    })
                    print(f"{scenario['name']} {iteration}/{args.repeats}: "
                          f"{rows[-1]['verdict']} rc={completed.returncode} "
                          f"boot={rows[-1]['boot_seconds']}", flush=True)
            summary = {}
            for scenario in scenarios:
                values = [row['boot_seconds'] for row in rows
                          if row['scenario'] == scenario['name']
                          and isinstance(row['boot_seconds'], (int, float))]
                summary[scenario['name']] = summarize(values)
            bench = {
                'ok': all(row['verdict'] == 'PASS' for row in rows),
                'lock': lock_info,
                'repeats': args.repeats,
                'scenarios': names,
                'summary': summary,
                'rows': rows,
            }
            (directory / 'bench.json').write_text(json.dumps(bench, indent=2) + '\n')
            print(f"bench: {directory / 'bench.json'}")
            print(json.dumps(summary, indent=2))
            return 0 if bench['ok'] else 1
    except LockTimeout as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
