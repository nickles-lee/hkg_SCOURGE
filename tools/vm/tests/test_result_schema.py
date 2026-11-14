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
from harness.result_schema import (check_all_results, schema_state,
                                   validate_file, validate_result)

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

    def test_provenance_structures(self) -> None:
        good = valid_result(
            images={'rootfs': {'path': '/x', 'size': 1, 'mtime_ns': 1,
                               'ctime_ns': 2, 'sha256': None,
                               'fresh_digest': False,
                               'trusted_reference_verified': False}},
            overlays={'rootfs': {'path': '/o', 'backing': '/b',
                                 'persistent': True, 'state_dir': '/s'}},
            harness_sources={'launch.py': 'a' * 64})
        self.assertEqual(validate_result(good), [])

        bad = valid_result(
            images={'bad': 'str',
                    'worse': {'path': '', 'size': -1, 'mtime_ns': 'n',
                              'ctime_ns': 1, 'sha256': 1,
                              'fresh_digest': 'no',
                              'trusted_reference_verified': 1}},
            overlays={'bad': {'path': '', 'backing': '',
                              'persistent': 'yes', 'state_dir': 1}},
            harness_sources={'a': 1})
        joined = '\n'.join(validate_result(bad))
        for expected in ('images.bad must be an object',
                         'images.worse.path',
                         'images.worse.size',
                         'images.worse.mtime_ns',
                         'images.worse.sha256',
                         'images.worse.fresh_digest',
                         'overlays.bad.path',
                         'overlays.bad.persistent',
                         'harness_sources.a'):
            self.assertIn(expected, joined)

    def test_verdict_rc_consistency(self) -> None:
        cases = [
            (valid_result(verdict='PASS', rc=1), 'PASS requires rc == 0'),
            (valid_result(verdict='DRY-RUN', rc=1), 'DRY-RUN requires rc == 0'),
            (valid_result(verdict='BLOCKED', rc=1), 'BLOCKED requires rc == 77'),
            (valid_result(verdict='FAIL', rc=0), 'FAIL requires a nonzero rc'),
            (valid_result(verdict='BLOCKED', rc=77, blockers=[]),
             'BLOCKED requires a non-empty blockers list'),
        ]
        for data, expected in cases:
            self.assertIn(expected, '\n'.join(validate_result(data)))
        self.assertEqual(validate_result(
            valid_result(verdict='BLOCKED', rc=77, blockers=['needs hardware'])), [])
        self.assertEqual(validate_result(valid_result(verdict='FAIL', rc=3)), [])

    def test_run_dir_mismatch_detected(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-schema-dir-') as tmp:
            run = Path(tmp) / 'run'
            run.mkdir()
            result_file = run / 'result.json'
            result_file.write_text(json.dumps(
                valid_result(run_dir='/tmp/elsewhere')))
            _, problems = validate_file(result_file)
            self.assertTrue(any('run_dir does not match' in problem
                                for problem in problems))
            result_file.write_text(json.dumps(
                valid_result(run_dir=str(run.resolve()))))
            self.assertEqual(validate_file(result_file)[1], [])

    def test_legacy_state(self) -> None:
        data = valid_result(schema_version=2)
        self.assertEqual(schema_state(data, validate_result(data)), 'legacy')
        self.assertEqual(schema_state(None, ['missing']), 'missing')

    def test_collect_runs_schema_fields(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-schema-') as tmp:
            root = Path(tmp)
            run = root / 'runs' / 'valid'
            run.mkdir(parents=True)
            (run / 'result.json').write_text(json.dumps(valid_result(run_dir=str(run))))
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
            for name, overrides in (
                    ('valid', {}),
                    ('legacy', {'schema_version': 2}),
                    ('invalid', {'verdict': 'BAD'})):
                path = runs_root / name
                path.mkdir()
                data = valid_result(run_dir=str(path), **overrides)
                (path / 'result.json').write_text(json.dumps(data))
            (runs_root / 'missing').mkdir()
            bench_only = runs_root / 'bench-only'
            bench_only.mkdir()
            (bench_only / 'bench.json').write_text('{}')
            report = check_all_results(root)
            self.assertFalse(report['ok'])
            self.assertEqual(report['checked'], 3)
            self.assertEqual(report['valid'], 1)
            self.assertEqual(report['legacy'], 1)
            self.assertEqual(report['invalid'], 1)
            self.assertEqual(report['missing'], 1)
            self.assertEqual(report['bench_only'], 1)
            self.assertEqual(report['problems'][0]['run'], 'invalid')

    def test_list_schema_filter(self) -> None:
        suffix = f'{os.getpid()}-filter'
        legacy_id = f'schema-legacy-{suffix}'
        invalid_id = f'schema-invalid-{suffix}'
        legacy_dir = VM / 'runs' / legacy_id
        invalid_dir = VM / 'runs' / invalid_id
        legacy_dir.mkdir(parents=True)
        invalid_dir.mkdir(parents=True)
        try:
            (legacy_dir / 'result.json').write_text(
                json.dumps(valid_result(schema_version=2)))
            (invalid_dir / 'result.json').write_text(
                json.dumps(valid_result(run_dir=str(invalid_dir), verdict='BAD')))
            legacy = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'list',
                 '--schema', 'legacy', '--json', '--limit', '200'],
                cwd=VM, capture_output=True, text=True, check=True)
            rows = json.loads(legacy.stdout)
            self.assertIn(legacy_id, [row['run'] for row in rows])
            self.assertTrue(all(row['schema_state'] == 'legacy' for row in rows))
            invalid = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'list',
                 '--schema', 'invalid', '--json', '--limit', '200'],
                cwd=VM, capture_output=True, text=True, check=True)
            rows = json.loads(invalid.stdout)
            self.assertIn(invalid_id, [row['run'] for row in rows])
            self.assertTrue(all(row['schema_state'] == 'invalid' for row in rows))
        finally:
            import shutil
            shutil.rmtree(legacy_dir, ignore_errors=True)
            shutil.rmtree(invalid_dir, ignore_errors=True)

    def test_check_run_cli_and_show_strict(self) -> None:
        run_id = f'schema-test-{os.getpid()}'
        run_dir = VM / 'runs' / run_id
        run_dir.mkdir(parents=True, exist_ok=True)
        try:
            (run_dir / 'result.json').write_text(
                json.dumps(valid_result(run_dir=str(run_dir))))
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
            (run_dir / 'result.json').write_text(
                json.dumps(valid_result(run_dir=str(run_dir), verdict='BAD')))
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

            (run_dir / 'result.json').write_text(json.dumps(
                valid_result(run_dir='/tmp/not-this-run')))
            mismatch = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'show', run_id,
                 '--strict'], cwd=VM, capture_output=True, text=True, check=False)
            self.assertEqual(mismatch.returncode, 1)
            self.assertIn('schema: invalid', mismatch.stderr)
        finally:
            import shutil
            shutil.rmtree(run_dir, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
