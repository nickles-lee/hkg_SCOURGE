# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness import runs
from harness.result_schema import check_all_results, schema_state, validate_result

VM = Path(__file__).resolve().parents[1]


def valid_result(**overrides):
    data = {
        'schema_version': 3,
        'run_dir': '/tmp/run',
        'profile': {'name': 'conformance'},
        'scenario': {'name': 'boot'},
        'cmd': 'true',
        'timeout': 30,
        'verdict': 'PASS',
        'rc': 0,
        'guest_rc': 0,
        'error': None,
        'boot_seconds': 5.0,
        'readiness_seconds': 5.2,
        'scenario_seconds': 0.1,
        'images': {},
        'overlays': {},
        'applied_substitutions': [],
        'checks': {'boot': 'PASS'},
    }
    data.update(overrides)
    return data


class ResultSchemaTests(unittest.TestCase):
    def test_valid_current_result(self) -> None:
        data = valid_result()
        self.assertEqual(validate_result(data), [])
        self.assertEqual(schema_state(data, []), 'ok')

    def test_invalid_core_fields_are_reported(self) -> None:
        data = valid_result(schema_version='3', run_dir='', verdict='MAYBE',
                            rc='0', timeout=0, profile={}, scenario={'name': 1},
                            checks={'boot': 'OK'}, images=[],
                            applied_substitutions=[1], dry_run='yes',
                            pin_cpu='0', lock=[], failure_bundle='x')
        problems = '\n'.join(validate_result(data))
        for expected in ('schema_version', 'run_dir', 'verdict', 'rc',
                         'timeout', 'profile.name', 'scenario.name',
                         'checks.boot', 'images', 'applied_substitutions',
                         'dry_run', 'pin_cpu', 'lock', 'failure_bundle'):
            self.assertIn(expected, problems)

    def test_legacy_state(self) -> None:
        data = valid_result(schema_version=2)
        self.assertEqual(schema_state(data, validate_result(data)), 'legacy')
        self.assertEqual(schema_state(None, ['missing']), 'missing')

    def test_collect_runs_schema_fields(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-schema-') as tmp:
            root = Path(tmp)
            run = root / 'runs' / 'valid'
            run.mkdir(parents=True)
            (run / 'result.json').write_text(json.dumps(valid_result()))
            missing = root / 'runs' / 'missing'
            missing.mkdir()
            rows = {item['run']: item for item in runs.collect_runs(root)}
            self.assertEqual(rows['valid']['schema_state'], 'ok')
            self.assertTrue(rows['valid']['schema_ok'])
            self.assertEqual(rows['missing']['schema_state'], 'missing')
            self.assertFalse(rows['missing']['schema_ok'])

    def test_bulk_check_counts_and_invalid_gate(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-schema-all-') as tmp:
            root = Path(tmp)
            runs_root = root / 'runs'
            runs_root.mkdir()
            for name, data in (
                    ('valid', valid_result()),
                    ('legacy', valid_result(schema_version=2)),
                    ('invalid', valid_result(verdict='BAD'))):
                path = runs_root / name
                path.mkdir()
                (path / 'result.json').write_text(json.dumps(data))
            (runs_root / 'missing').mkdir()
            report = check_all_results(root)
            self.assertFalse(report['ok'])
            self.assertEqual(report['checked'], 3)
            self.assertEqual(report['valid'], 1)
            self.assertEqual(report['legacy'], 1)
            self.assertEqual(report['invalid'], 1)
            self.assertEqual(report['missing'], 1)
            self.assertEqual(report['problems'][0]['run'], 'invalid')

    def test_check_run_cli_and_show_strict(self) -> None:
        run_id = f'schema-test-{os.getpid()}'
        run_dir = VM / 'runs' / run_id
        run_dir.mkdir(parents=True, exist_ok=True)
        try:
            (run_dir / 'result.json').write_text(json.dumps(valid_result()))
            good = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'check-run', run_id],
                cwd=VM, capture_output=True, text=True, check=False)
            self.assertEqual(good.returncode, 0, good.stderr)
            self.assertTrue(json.loads(good.stdout)['ok'])
            show = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'show', run_id,
                 '--strict'], cwd=VM, capture_output=True, text=True, check=False)
            self.assertEqual(show.returncode, 0, show.stderr)
            self.assertIn('schema: ok', show.stderr)
            (run_dir / 'result.json').write_text(json.dumps(valid_result(verdict='BAD')))
            bad = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'check-run', run_id],
                cwd=VM, capture_output=True, text=True, check=False)
            self.assertEqual(bad.returncode, 1)
            self.assertFalse(json.loads(bad.stdout)['ok'])
            strict = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'show', run_id,
                 '--strict'], cwd=VM, capture_output=True, text=True, check=False)
            self.assertEqual(strict.returncode, 1)
            self.assertIn('schema: invalid', strict.stderr)
        finally:
            import shutil
            shutil.rmtree(run_dir, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
