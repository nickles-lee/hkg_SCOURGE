# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from bench import build_argv, result_path, summarize
from harness.config import load_scenario

VM = Path(__file__).resolve().parents[1]


def args(**overrides):
    values = {
        'profile': 'conformance', 'timeout': 120, 'storage': 'flat',
        'storage_size': [], 'cache': None, 'smp': None, 'memory': None,
        'pin_cpu': None, 'apic': False, 'usb': None, 'tools': None,
        'payload': None, 'share': None,
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

    def test_cli_help_exposes_repeats(self) -> None:
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'bench.py'), '--help'],
            capture_output=True, text=True, check=True)
        self.assertIn('--repeats', completed.stdout)


if __name__ == '__main__':
    unittest.main()
