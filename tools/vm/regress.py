#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Run managed VM scenarios repeatedly; retain individual logs and an aggregate."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import subprocess
import sys
import time

from harness.config import load_scenario
from harness.storage import parse_sizes

VM = Path(__file__).resolve().parent


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scenario', action='append', default=[])
    parser.add_argument('--profile', choices=('research', 'conformance'), default='research')
    parser.add_argument('--repeat', type=int, default=1)
    parser.add_argument('--tools', type=Path, default=VM / 'tools.dev.img')
    parser.add_argument('--usb', type=Path, default=VM / 'update-k.img')
    parser.add_argument('--payload', type=Path, help='optional scenario payload directory/image')
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--storage', choices=('flat', 'split'), default='flat')
    parser.add_argument('--storage-size', action='append', default=[])
    parser.add_argument('--audit', action='store_true')
    args = parser.parse_args()
    if args.repeat <= 0 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error('--repeat and --timeout must be finite and positive')
    names = args.scenario or ['boot', 'usb-discovery', 'dbus-ready', 'dmclient-events', 'updater-launch']
    try:
        scenarios = [load_scenario(name) for name in names]
        sizes = parse_sizes(args.storage_size)
        if sizes and args.storage != 'split':
            raise ValueError('--storage-size requires --storage split')
        if any(item.get('requires_storage', args.storage) != args.storage for item in scenarios):
            raise ValueError('selected scenario requires --storage split')
    except (ValueError, OSError) as exc:
        parser.error(str(exc))
    directory = VM / 'runs' / f'regression-{time.time_ns()}'
    directory.mkdir(parents=True)
    paths = {name: path.resolve() for name, path in {'usb': args.usb, 'tools': args.tools,
                                                     'rootfs': VM / 'rootfs.img', 'kernel': VM / 'bzImage'}.items()}
    if args.payload is not None:
        paths['payload'] = args.payload.resolve()

    def identities():
        return {name: [path.stat().st_size, path.stat().st_mtime_ns, path.stat().st_ctime_ns]
                for name, path in paths.items() if path.is_file()}

    before = identities()
    results = []
    for iteration in range(args.repeat):
        for scenario in scenarios:
            argv = [sys.executable, '-B', str(VM / 'launch.py'), 'run', '--profile', args.profile,
                    '--scenario', scenario['name'], '--timeout', str(args.timeout), '--storage', args.storage]
            for value in args.storage_size:
                argv.extend(['--storage-size', value])
            if args.audit:
                argv.append('--audit')
            for name in scenario['requires']:
                if name not in paths:
                    parser.error(f'{scenario["name"]} requires --{name}')
                argv.extend([f'--{name}', str(paths[name])])
            log = directory / f"{iteration + 1}-{scenario['name']}.log"
            with log.open('w') as output:
                # launch.py handles deadlines and owns QEMU cleanup; this runner
                # does not use a second, shorter subprocess timeout.
                completed = subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT)
            text = log.read_text()
            first = next((line[5:] for line in text.splitlines() if line.startswith('run: ')), None)
            result_file = Path(first) / 'result.json' if first else None
            detail = json.loads(result_file.read_text()) if result_file and result_file.is_file() else {}
            verdict = detail.get('verdict', 'FAIL')
            known_failure = bool(detail.get('known_failure'))
            results.append({'iteration': iteration + 1, 'scenario': scenario['name'],
                            'verdict': verdict, 'rc': completed.returncode,
                            'known_failure': known_failure,
                            'known_issue': detail.get('known_issue'),
                            'result': str(result_file) if result_file else None, 'log': str(log)})
            label = f'{verdict} (known)' if known_failure else verdict
            print(f"{iteration + 1}/{args.repeat} {scenario['name']}: {label} rc={completed.returncode}", flush=True)
    unchanged = before == identities()
    accepted = [item for item in results if item['verdict'] == 'PASS' or item['known_failure']]
    known = [item for item in results if item['known_failure']]
    new_failures = [item for item in results
                    if not (item['verdict'] == 'PASS' or item['known_failure'])]
    ok = unchanged and not new_failures
    summary = {'ok': ok, 'profile': args.profile, 'storage': args.storage,
               'backing_metadata_unchanged': unchanged,
               'known_failures': [{'scenario': item['scenario'],
                                   'known_issue': item['known_issue']} for item in known],
               'new_failures': [item['scenario'] for item in new_failures],
               'note': 'Metadata stability is not a full-file cryptographic comparison.', 'runs': results}
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'summary: {directory / "summary.json"}')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
