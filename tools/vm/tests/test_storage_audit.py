# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

VM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(VM))

import launch
from harness.audit import DEVICE_PATHS, REQUIRED, SECTIONS, audit_script, parse_audit
from harness.config import load_profile, load_scenario, scenario_command
from harness.storage import VOLUMES, cleanup_storage, create_storage, mount_script, parse_sizes, probe_script


class StorageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='storage-test-', dir='/tmp/opencode')
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_size_arguments_are_bounded_unique_and_typed(self):
        self.assertEqual(parse_sizes(['update=64', 'rwdata=128']), {'update': 64, 'rwdata': 128})
        for args in (['update=15'], ['update=8193'], ['bad=64'], ['update=64', 'update=128'],
                     ['update=-1'], ['update=nan'], ['../root=64']):
            with self.assertRaises(ValueError):
                parse_sizes(args)

    def test_builds_only_small_synthetic_images_and_explicit_minimal_seed(self):
        with patch('harness.storage.subprocess.run') as run:
            storage = create_storage(self.root, {name: 16 for name in VOLUMES})
        self.assertTrue(storage['synthetic'])
        self.assertFalse(storage['production_layout_verified'])
        self.assertEqual(run.call_count, 3)
        for name, item in storage['volumes'].items():
            self.assertEqual(Path(item['backing']).stat().st_size, 16 * 1024 ** 2)
            marker = (self.root / f'storage-seed-{name}' / '.harness-volume').read_text()
            self.assertEqual(marker, f"IONIQ_VOLUME_V1 {storage['nonce']} {name}\n")
        self.assertTrue((self.root / 'storage-seed-rwdata/var/run/dbus').is_dir())
        cleanup_storage(self.root, storage)
        self.assertFalse(any(self.root.glob('storage-*.raw')))

    def test_failed_build_cleans_only_images_it_created(self):
        keep = self.root / 'keep'
        keep.write_text('user data')
        with patch('harness.storage.subprocess.run', side_effect=subprocess.CalledProcessError(1, ['mkfs'])):
            with self.assertRaises(subprocess.CalledProcessError):
                create_storage(self.root, {'update': 16})
        self.assertFalse(any(self.root.glob('storage-*.raw')))
        self.assertEqual(keep.read_text(), 'user data')

    def test_cleanup_rejects_arbitrary_backing(self):
        user = self.root / 'user.raw'
        user.write_text('data')
        with self.assertRaises(ValueError):
            cleanup_storage(self.root, {'volumes': {'update': {'backing': str(user)}}})
        self.assertTrue(user.exists())

    def test_shell_scripts_are_valid_without_executing_them_on_host(self):
        storage = {'mode': 'split', 'nonce': 'a' * 32}
        for script in (mount_script(storage), probe_script(storage), audit_script()):
            result = subprocess.run(['bash', '-n'], input=script, text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
        with self.assertRaises(ValueError):
            mount_script({'nonce': '$(unsafe)'})

    def test_storage_scenario_requires_split_mode(self):
        with self.assertRaises(ValueError):
            scenario_command(load_scenario('storage-isolation'), load_profile('research'))

    def test_split_disks_use_overlays_and_offline_usb_transport(self):
        overlays = {'rootfs': {'path': str(self.root / 'rootfs.qcow2')}}
        for name in VOLUMES:
            overlays[f'storage_{name}'] = {'path': str(self.root / f'{name}.qcow2'),
                                          'backing': str(self.root / f'{name}.raw')}
        argv = launch.qemu_argv(self.root, overlays=overlays)
        self.assertEqual(argv[argv.index('-nic') + 1], 'none')
        self.assertEqual(sum('usb-storage,drive=storage_' in arg for arg in argv), 3)
        for index, value in enumerate(argv):
            if value == '-drive':
                self.assertIn('format=qcow2', argv[index+1])

    def test_split_interactive_and_flat_size_overrides_are_rejected(self):
        with self.assertRaises(ValueError):
            launch.run(None, True, 60, storage_mode='split')
        with self.assertRaises(ValueError):
            launch.run('true', False, 60, storage_sizes={'update': 64})


def fixture_audit(changes=None, errors=None):
    sections = {name: '' for name in SECTIONS}
    sections.update(kernel='Linux test 4.1.27-abl', pid1='bash', cmdline='init=/init',
                    mounts='/dev/mmcblk0 / ext4 rw 0 0',
                    devices='\n'.join(path + ' absent' for path in DEVICE_PATHS))
    sections.update(changes or {})
    return ''.join(f'HARNESS_AUDIT_BEGIN {name}\n{value}\nHARNESS_AUDIT_END {name} {(errors or {}).get(name, 0)}\n'
                   for name, value in sections.items())


class AuditTests(unittest.TestCase):
    def test_parses_data_and_reports_gaps_without_claiming_stock_equivalence(self):
        result = parse_audit(fixture_audit())
        self.assertFalse(result['stock_equivalence_verified'])
        self.assertIsNone(result['writable_mounts']['/update'])
        self.assertTrue(any('PID 1' in gap for gap in result['gaps']))
        self.assertFalse(result['missing_required'])

    def test_split_mounts_are_observed_not_assumed(self):
        text = '/dev/mmcblk0 / ext4 rw 0 0\n'
        for name, item in VOLUMES.items():
            text += f"/dev/{name} {item['mountpoint']} ext4 rw,noatime 0 0\n"
        result = parse_audit(fixture_audit({'mounts': text}))
        self.assertTrue(all(result['writable_mounts'].values()))
        self.assertFalse(any('not separate' in gap for gap in result['gaps']))
        self.assertFalse(result['stock_equivalence_verified'])

    def test_missing_optional_policy_stays_unknown(self):
        result = parse_audit(fixture_audit(errors={'smack_rules': 1}))
        self.assertEqual(result['observations']['smack_rules']['rc'], 1)
        self.assertFalse(result['missing_required'])

    def test_failed_required_observation_is_not_hidden(self):
        result = parse_audit(fixture_audit(errors={'mounts': 1}))
        self.assertEqual(result['missing_required'], ['mounts'])

    def test_duplicate_truncated_or_out_of_order_sections_rejected(self):
        valid = fixture_audit()
        for bad in (valid + 'HARNESS_AUDIT_BEGIN kernel\n', valid.replace('HARNESS_AUDIT_END mounts 0', ''),
                    valid.replace('HARNESS_AUDIT_BEGIN pid1\nbash\nHARNESS_AUDIT_END pid1 0',
                                  'HARNESS_AUDIT_END pid1 0\nbash\nHARNESS_AUDIT_BEGIN pid1')):
            with self.assertRaises(ValueError):
                parse_audit(bad)

    def test_shell_looking_target_content_is_only_data(self):
        value = '$(echo never-execute); arbitrary target content'
        result = parse_audit(fixture_audit({'version': value}))
        self.assertEqual(result['observations']['version']['text'], value)

    def test_empty_required_or_incomplete_device_observations_fail(self):
        result = parse_audit(fixture_audit({'cmdline': '', 'devices': '/dev/keystore absent'}))
        self.assertIn('cmdline', result['missing_required'])
        self.assertIn('devices', result['missing_required'])

    def test_policy_empty_and_permissive_are_explicit_gaps(self):
        result = parse_audit(fixture_audit({'smack_bringup': '1'}))
        self.assertTrue(any('no rules' in item for item in result['gaps']))
        self.assertTrue(any('bring-up switch' in item for item in result['gaps']))


if __name__ == '__main__':
    unittest.main()
