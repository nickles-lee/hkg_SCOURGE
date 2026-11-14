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


def variants(args) -> list[tuple[str | None, int | None]]:
    """Cache/pinning combinations for one benchmark invocation."""
    if not args.matrix:
        return [(args.cache, args.pin_cpu)]
    caches = args.matrix_cache or ['writeback', 'writethrough']
    pins = [None]
    for cpu in args.matrix_cpu:
        if cpu not in pins:
            pins.append(cpu)
    return [(cache, pin) for cache in caches for pin in pins]


def build_argv(args, scenario: dict, cache: str | None = None,
               pin_cpu: int | None = None) -> list[str]:
    cache = args.cache if cache is None else cache
    pin_cpu = args.pin_cpu if pin_cpu is None else pin_cpu
    argv = [
        sys.executable, '-B', str(VM / 'launch.py'), 'run', '--no-lock',
        '--profile', args.profile, '--kernel', args.kernel,
        '--scenario', scenario['name'], '--timeout', str(args.timeout),
        '--storage', args.storage,
    ]
    for value in args.storage_size:
        argv.extend(['--storage-size', value])
    if cache:
        argv.extend(['--cache', cache])
    if args.smp:
        argv.extend(['--smp', str(args.smp)])
    if args.memory:
        argv.extend(['--memory', str(args.memory)])
    if pin_cpu is not None:
        argv.extend(['--pin-cpu', str(pin_cpu)])
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
    parser.add_argument('--kernel', choices=('vendor', 'compat'),
                        default='vendor')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--timeout', type=float, default=120)
    parser.add_argument('--lock-timeout', type=float, default=600)
    parser.add_argument('--storage', choices=('flat', 'split'), default='flat')
    parser.add_argument('--storage-size', action='append', default=[])
    parser.add_argument('--cache', choices=('writeback', 'writethrough', 'directsync',
                                            'unsafe', 'none'), default=None)
    parser.add_argument('--matrix', action='store_true',
                        help='compare cache modes and optional pinned CPUs')
    parser.add_argument('--matrix-cache', action='append', default=[],
                        choices=('writeback', 'writethrough', 'directsync',
                                 'unsafe', 'none'),
                        help='cache mode for --matrix (default: writeback, writethrough)')
    parser.add_argument('--matrix-cpu', action='append', type=int, default=[],
                        help='pinned CPU for --matrix (unpinned is always included)')
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
    if (args.matrix_cache or args.matrix_cpu) and not args.matrix:
        parser.error('--matrix-cache/--matrix-cpu require --matrix')
    if args.matrix and (args.cache is not None or args.pin_cpu is not None):
        parser.error('--matrix cannot be combined with --cache/--pin-cpu')
    names = args.scenario or ['boot']
    try:
        scenarios = [load_scenario(name) for name in names]
        parse_sizes(args.storage_size)
        # Validate required inputs and argv before taking the lock.
        for scenario in scenarios:
            for cache, pin_cpu in variants(args):
                build_argv(args, scenario, cache=cache, pin_cpu=pin_cpu)
    except ValueError as exc:
        parser.error(str(exc))
    try:
        with run_lock(VM, args.lock_timeout) as lock_info:
            directory = VM / 'runs' / f'bench-{time.time_ns()}'
            directory.mkdir(parents=True)
            rows = []
            for scenario in scenarios:
                for cache, pin_cpu in variants(args):
                    for iteration in range(1, args.repeats + 1):
                        suffix = cache or 'default'
                        if pin_cpu is not None:
                            suffix += f'-cpu{pin_cpu}'
                        log = directory / f"{scenario['name']}-{suffix}-{iteration}.log"
                        with log.open('w') as output:
                            completed = subprocess.run(
                                build_argv(args, scenario, cache=cache,
                                           pin_cpu=pin_cpu),
                                stdout=output, stderr=subprocess.STDOUT)
                        path = result_path(log)
                        detail = (json.loads(path.read_text())
                                  if path is not None and path.is_file() else {})
                        rows.append({
                            'scenario': scenario['name'],
                            'iteration': iteration,
                            'cache': cache,
                            'pin_cpu': pin_cpu,
                            'verdict': detail.get('verdict', 'FAIL'),
                            'rc': completed.returncode,
                            'boot_seconds': detail.get('boot_seconds'),
                            'readiness_seconds': detail.get('readiness_seconds'),
                            'result': str(path) if path is not None else None,
                            'log': str(log),
                        })
                        print(f"{scenario['name']} [{suffix}] {iteration}/{args.repeats}: "
                              f"{rows[-1]['verdict']} rc={completed.returncode} "
                              f"boot={rows[-1]['boot_seconds']}", flush=True)
            summary = {}
            for scenario in scenarios:
                for cache, pin_cpu in variants(args):
                    key = (f"{scenario['name']} cache={cache or 'default'} "
                           f"pin={pin_cpu if pin_cpu is not None else 'none'}")
                    values = [row['boot_seconds'] for row in rows
                              if row['scenario'] == scenario['name']
                              and row['cache'] == cache and row['pin_cpu'] == pin_cpu
                              and isinstance(row['boot_seconds'], (int, float))]
                    summary[key] = summarize(values)
            bench = {
                'ok': all(row['verdict'] == 'PASS' for row in rows),
                'lock': lock_info,
                'repeats': args.repeats,
                'scenarios': names,
                'variants': [{'cache': cache, 'pin_cpu': pin}
                             for cache, pin in variants(args)],
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
