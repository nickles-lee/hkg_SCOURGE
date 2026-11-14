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

    def test_evidence_structures(self) -> None:
        good = valid_result(
            storage={
                'mode': 'split',
                'volumes': {'update': {
                    'mountpoint': '/update', 'size_mib': 16,
                    'backing': '/b', 'overlay_name': 'storage_update',
                    'filesystem': 'ext4', 'mount_options': 'rw'}},
                'setup': {}, 'setup_seconds': 1.0, 'nonce': 'n',
                'seed': 's', 'transport': 't', 'synthetic': True,
                'production_layout_verified': False},
            power_loss={'requested': True, 'marker': 'M',
                        'marker_seen': True, 'killed': True},
            usb_removal={'requested': True, 'device_id': 'usbdev',
                         'marker': 'M', 'marker_seen': True,
                         'qmp': {'ok': True}})
        self.assertEqual(validate_result(good), [])

        bad = valid_result(
            storage={
                'mode': 'raid',
                'volumes': {'bad': 'str', 'worse': {
                    'mountpoint': '', 'size_mib': 0, 'backing': 1,
                    'overlay_name': '', 'filesystem': '',
                    'mount_options': 1}},
                'setup': [], 'setup_seconds': 'x', 'nonce': 1,
                'synthetic': 'yes', 'production_layout_verified': 1},
            power_loss={'requested': 'yes', 'marker': '',
                        'marker_seen': 1, 'killed': 1, 'error': 1},
            usb_removal={'requested': 'yes', 'device_id': '', 'marker': 1,
                         'marker_seen': 1, 'qmp': 'x', 'error': 1})
        joined = '\n'.join(validate_result(bad))
        for expected in ('storage.mode', 'storage.volumes.bad',
                         'storage.volumes.worse.mountpoint',
                         'storage.volumes.worse.size_mib',
                         'storage.setup must be an object',
                         'storage.setup_seconds', 'storage.nonce',
                         'storage.synthetic', 'power_loss.requested',
                         'power_loss.marker', 'usb_removal.device_id',
                         'usb_removal.qmp', 'usb_removal.error'):
            self.assertIn(expected, joined)

    def test_screenshot_evidence(self) -> None:
        good = valid_result(screenshot={
            'requested': True, 'ok': True,
            'artifacts': [{'name': 'x.png', 'path': '/x.png', 'size': 1,
                           'sha256': 'a' * 64}],
            'qmp': {'path': '/q.png', 'size': 2, 'sha256': 'b' * 64,
                    'width': 10, 'height': 20},
            'qmp_error': None, 'qmp_socket': '/q.sock',
            'qmp_pixels': {'count': 1, 'bbox': [0, 0, 1, 1],
                           'width': 10, 'height': 20}})
        self.assertEqual(validate_result(good), [])

        bad = valid_result(screenshot={
            'requested': 'yes', 'ok': 1, 'artifacts': [{'name': '',
            'path': '', 'size': -1, 'sha256': 1}],
            'qmp': {'path': '', 'size': -1, 'sha256': 1,
                    'width': 0, 'height': 'x'},
            'qmp_error': 1, 'qmp_socket': 1,
            'qmp_pixels': {'count': -1, 'bbox': [0, 0], 'width': 0,
                           'height': 'x'}})
        joined = '\n'.join(validate_result(bad))
        for expected in ('screenshot.requested', 'screenshot.ok',
                         'screenshot.artifacts[0].name',
                         'screenshot.artifacts[0].size',
                         'screenshot.qmp.path', 'screenshot.qmp.size',
                         'screenshot.qmp.width', 'screenshot.qmp_error',
                         'screenshot.qmp_socket', 'screenshot.qmp_pixels.count',
                         'screenshot.qmp_pixels.bbox'):
            self.assertIn(expected, joined)

    def test_patch_phase_audit_evidence(self) -> None:
        good = valid_result(
            patch={'driver_init': '0x1', 'table_entry': '0x2', 'stop': 'T05',
                   'stop_signal': 'T05', 'connect_attempts': 2,
                   'original': 'aa', 'replacement': 'bb'},
            phase={'ok': True, 'fail': False, 'steps': ['a'],
                   'timeouts': [], 'waits': ['w'], 'dmclient_standby': [],
                   'verify_key': [], 'appupgrade': False,
                   'launch_requested': True, 'usb_mount': True},
            audit={'status': 'PASS', 'path': '/env.json', 'gaps': ['g'],
                   'missing_required': []})
        self.assertEqual(validate_result(good), [])

        bad = valid_result(
            patch={'connect_attempts': 0, 'stop_signal': '',
                   'driver_init': 1, 'original': 2},
            phase={'ok': 'yes', 'fail': 1, 'steps': 'x', 'waits': {}},
            audit={'status': '', 'path': 1, 'gaps': [1],
                   'missing_required': 'x'})
        joined = '\n'.join(validate_result(bad))
        for expected in ('patch.connect_attempts', 'patch.stop_signal',
                         'patch.driver_init', 'patch.original',
                         'phase.ok', 'phase.fail', 'phase.steps',
                         'phase.waits', 'audit.status', 'audit.path',
                         'audit.gaps', 'audit.missing_required'):
            self.assertIn(expected, joined)

    def test_lock_and_failure_bundle_evidence(self) -> None:
        good = valid_result(
            lock={'held': True, 'path': '/l', 'pid': 1,
                  'waited_seconds': 0.5},
            failure_bundle={
                'path': '/b', 'summary': '/b/summary.md', 'excerpts': 1,
                'sections': {'dmesg': {'path': '/b/dmesg.log', 'bytes': 10,
                                       'rc': 0, 'seconds': 0.1}},
                'deadline_seconds': 45, 'deadline_exceeded': False,
                'elapsed_seconds': 0.2})
        self.assertEqual(validate_result(good), [])

        bad = valid_result(
            lock={'held': 'yes', 'path': 1, 'pid': 'x',
                  'waited_seconds': -1},
            failure_bundle={
                'path': '', 'summary': '', 'excerpts': -1,
                'sections': {'bad': 'str', 'worse': {
                    'path': '', 'bytes': -1, 'rc': 'x', 'seconds': 'y',
                    'error': 1, 'skipped': 1}},
                'deadline_seconds': 'x', 'deadline_exceeded': 'no',
                'elapsed_seconds': 'y'})
        joined = '\n'.join(validate_result(bad))
        for expected in ('lock.held', 'lock.path', 'lock.pid',
                         'lock.waited_seconds', 'failure_bundle.path',
                         'failure_bundle.summary', 'failure_bundle.excerpts',
                         'failure_bundle.sections.bad',
                         'failure_bundle.sections.worse.path',
                         'failure_bundle.sections.worse.bytes',
                         'failure_bundle.sections.worse.rc',
                         'failure_bundle.sections.worse.seconds',
                         'failure_bundle.deadline_seconds',
                         'failure_bundle.deadline_exceeded'):
            self.assertIn(expected, joined)

    def test_tools_manifest_and_compat_pin(self) -> None:
        good = valid_result(
            tools_manifest={'ok': True, 'state': 'manifest',
                            'image': 'tools.img', 'expected_build_id': 'x',
                            'detail': 'd', 'build_id': 'b',
                            'only_in_image': ['a'], 'only_in_tree': [],
                            'changed': [], 'stale': []},
            compat_pin={'ok': True, 'problems': [], 'kernel': 'k',
                        'initramfs': 'i', 'version': 'v', 'state': 'ok'})
        self.assertEqual(validate_result(good), [])

        bad = valid_result(
            tools_manifest={'ok': 'yes', 'state': 1, 'image': 1,
                            'expected_build_id': 1, 'detail': 1,
                            'build_id': 1, 'only_in_image': 'x',
                            'only_in_tree': [1], 'changed': 1,
                            'stale': {}},
            compat_pin={'ok': 'yes', 'problems': 'x', 'kernel': 1,
                        'initramfs': 1, 'version': 1, 'state': 1})
        joined = '\n'.join(validate_result(bad))
        for expected in ('tools_manifest.ok', 'tools_manifest.state',
                         'tools_manifest.image',
                         'tools_manifest.expected_build_id',
                         'tools_manifest.detail', 'tools_manifest.build_id',
                         'tools_manifest.only_in_image',
                         'tools_manifest.only_in_tree',
                         'tools_manifest.changed', 'tools_manifest.stale',
                         'compat_pin.ok', 'compat_pin.problems',
                         'compat_pin.kernel', 'compat_pin.initramfs',
                         'compat_pin.version', 'compat_pin.state'):
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

    def test_list_limit_validation(self) -> None:
        invalid = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'list',
             '--limit', '-1'], cwd=VM, capture_output=True, text=True,
            check=False)
        self.assertEqual(invalid.returncode, 2)
        self.assertIn('--limit must be non-negative', invalid.stderr)
        empty = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'list',
             '--limit', '0', '--json'], cwd=VM, capture_output=True, text=True,
            check=False)
        self.assertEqual(empty.returncode, 0, empty.stderr)
        self.assertEqual(json.loads(empty.stdout), [])

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
