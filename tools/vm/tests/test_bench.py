# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from bench import build_argv, compare_bench, result_path, summarize, variants
from harness.config import load_scenario

VM = Path(__file__).resolve().parents[1]


def args(**overrides):
    values = {
        'profile': 'conformance', 'kernel': 'vendor', 'timeout': 120,
        'storage': 'flat',
        'storage_size': [], 'cache': None, 'smp': None, 'memory': None,
        'pin_cpu': None, 'apic': False, 'usb': None, 'tools': None,
        'payload': None, 'share': None, 'matrix': False,
        'matrix_cache': [], 'matrix_cpu': [],
    }
    values.update(overrides)
    return SimpleNamespace(**values)


class BenchTests(unittest.TestCase):
    def test_summarize(self) -> None:
        self.assertEqual(summarize([]), {})
        self.assertEqual(summarize([2.0, 4.0, 6.0]),
                         {'count': 3, 'mean': 4.0, 'median': 4.0,
                          'min': 2.0, 'max': 6.0})

    def test_build_argv_passes_required_and_optional_flags(self) -> None:
        scenario = load_scenario('can-smoke')
        argv = build_argv(args(cache='writeback', pin_cpu=0, smp=2,
                               memory=3072, payload=VM / 'payload/tweaks-vm',
                               storage_size=['update=32']), scenario)
        joined = ' '.join(argv)
        self.assertIn('--no-lock', argv)
        self.assertIn('--scenario can-smoke', joined)
        self.assertIn('--cache writeback', joined)
        self.assertIn('--pin-cpu 0', joined)
        self.assertIn('--smp 2', joined)
        self.assertIn('--memory 3072', joined)
        self.assertIn('--storage-size update=32', joined)
        self.assertIn('--payload', argv)

    def test_variants_single_and_matrix(self) -> None:
        self.assertEqual(variants(args()), [(None, None)])
        self.assertEqual(variants(args(matrix=True)),
                         [('writeback', None), ('writethrough', None)])
        self.assertEqual(
            variants(args(matrix=True, matrix_cache=['directsync'],
                          matrix_cpu=[0, 1])),
            [('directsync', None), ('directsync', 0), ('directsync', 1)])
        self.assertEqual(variants(args(cache='unsafe', pin_cpu=2)),
                         [('unsafe', 2)])

    def test_build_argv_cache_pin_override(self) -> None:
        scenario = load_scenario('boot')
        argv = build_argv(args(kernel='compat'), scenario,
                          cache='writethrough', pin_cpu=3)
        joined = ' '.join(argv)
        self.assertIn('--kernel compat', joined)
        self.assertIn('--cache writethrough', joined)
        self.assertIn('--pin-cpu 3', joined)

    def test_missing_required_input_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, 'requires --payload'):
            build_argv(args(), load_scenario('can-smoke'))

    def test_result_path_reads_run_line(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-bench-') as tmp:
            log = Path(tmp) / 'case.log'
            log.write_text('noise\nrun: /tmp/example-run\nmore\n')
            self.assertEqual(result_path(log), Path('/tmp/example-run/result.json'))
            log.write_text('no result\n')
            self.assertIsNone(result_path(log))

    def test_compare_bench_statuses(self) -> None:
        base = {'summary': {'a': {'mean': 10.0}, 'b': {'mean': 20.0},
                            'c': {'mean': 30.0}, 'd': {'mean': 0.0}}}
        current = {'summary': {'a': {'mean': 10.2}, 'b': {'mean': 22.0},
                               'c': {'mean': 28.0}, 'd': {'mean': 1.0}}}
        report = compare_bench(base, current, threshold=0.05)
        statuses = {row['key']: row['status'] for row in report['rows']}
        self.assertEqual(statuses, {'a': 'SAME', 'b': 'REGRESSED',
                                    'c': 'IMPROVED', 'd': 'REGRESSED'})
        self.assertFalse(report['ok'])
        self.assertEqual(report['rows'][1]['pct'], 10.0)

    def test_compare_reports_missing_keys(self) -> None:
        report = compare_bench({'summary': {'a': {'mean': 1.0}}},
                               {'summary': {'b': {'mean': 1.0}}})
        self.assertFalse(report['ok'])
        self.assertEqual([row['status'] for row in report['rows']],
                         ['MISSING', 'MISSING'])

    def test_compare_cli_exit_codes(self) -> None:
        baseline = {'summary': {'boot cache=default pin=none': {'mean': 10.0}}}
        current = {'summary': {'boot cache=default pin=none': {'mean': 10.2}}}
        with tempfile.TemporaryDirectory(prefix='vm-bench-compare-') as tmp:
            left = Path(tmp) / 'base.json'
            right = Path(tmp) / 'current.json'
            left.write_text(json.dumps(baseline))
            right.write_text(json.dumps(current))
            same = subprocess.run(
                [sys.executable, '-B', str(VM / 'bench.py'), '--compare',
                 str(left), str(right), '--threshold', '0.05'],
                capture_output=True, text=True, check=False)
            self.assertEqual(same.returncode, 0, same.stderr)
            self.assertEqual(json.loads(same.stdout)['ok'], True)
            worse = subprocess.run(
                [sys.executable, '-B', str(VM / 'bench.py'), '--compare',
                 str(left), str(right), '--threshold', '0.01'],
                capture_output=True, text=True, check=False)
            self.assertEqual(worse.returncode, 1)
            self.assertEqual(json.loads(worse.stdout)['rows'][0]['status'],
                             'REGRESSED')

    def test_failure_path_writes_bench_json_and_exits_nonzero(self) -> None:
        completed = subprocess.run(
            [sys.executable, "-B", str(VM / "bench.py"),
             "--scenario", "stock-update-stage", "--repeats", "1",
             "--usb", "/dev/null", "--tools", "/dev/null",
             "--lock-timeout", "10"],
            cwd=VM, capture_output=True, text=True, timeout=60)
        combined = completed.stdout + completed.stderr
        if "another VM boot holds" in combined or "lock timeout" in combined:
            self.skipTest("VM lab is busy with another run")
        self.assertEqual(completed.returncode, 1, combined)
        line = next(line for line in completed.stdout.splitlines()
                    if line.startswith("bench: "))
        bench_path = Path(line[7:])
        try:
            bench = json.loads(bench_path.read_text())
            self.assertFalse(bench["ok"])
            self.assertEqual(bench["rows"][0]["verdict"], "BLOCKED")
            self.assertEqual(bench["rows"][0]["rc"], 77)
        finally:
            shutil.rmtree(bench_path.parent, ignore_errors=True)

    def test_cli_help_exposes_repeats(self) -> None:
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'bench.py'), '--help'],
            capture_output=True, text=True, check=True)
        self.assertIn('--repeats', completed.stdout)
        self.assertIn('--matrix', completed.stdout)
        self.assertIn('--kernel', completed.stdout)
        self.assertIn('--compare', completed.stdout)


if __name__ == '__main__':
    unittest.main()
