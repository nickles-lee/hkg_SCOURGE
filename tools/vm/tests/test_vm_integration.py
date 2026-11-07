# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Opt-in tests: vendor programs execute only in the isolated QEMU guest."""
from __future__ import annotations

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import unittest

VM = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.environ.get('IONIQ_RUN_VM_TESTS') == '1', 'set IONIQ_RUN_VM_TESTS=1 to boot QEMU')
class VMIntegrationTests(unittest.TestCase):
    def run_guest(self, *args):
        process = subprocess.run([sys.executable, '-B', str(VM / 'launch.py'), 'run',
                                  '--lock-timeout', '100', *args],
                                 cwd=VM, capture_output=True, text=True, timeout=120)
        directory = next(line[5:] for line in process.stdout.splitlines() if line.startswith('run: '))
        result = json.loads((Path(directory) / 'result.json').read_text())
        return process, result

    def assert_cleaned(self, result):
        self.assertFalse(result['overlays_retained'])
        for overlay in result['overlays'].values():
            self.assertFalse(Path(overlay['path']).exists())
        self.assertFalse(result['generated_storage_retained'])
        for item in result['storage']['volumes'].values():
            self.assertFalse(Path(item['backing']).exists())
        if result.get('qemu_pid'):
            self.assertFalse(Path(f"/proc/{result['qemu_pid']}").exists())

    def test_guest_exit_7_propagates(self):
        process, result = self.run_guest('--cmd', 'exit 7', '--timeout', '60')
        self.assertEqual(process.returncode, 7)
        self.assertEqual(result['guest_rc'], 7)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assert_cleaned(result)

    def test_false_success_marker_is_failure(self):
        process, result = self.run_guest('--cmd', "printf 'PHASE4_FAIL intentional\\n'; exit 0", '--timeout', '60')
        self.assertEqual(result['guest_rc'], 0)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertNotEqual(process.returncode, 0)
        self.assert_cleaned(result)

    def test_missing_current_tools_is_failure(self):
        process, result = self.run_guest('--scenario', 'dbus-ready', '--tools',
                                        str(VM / 'tools.img'), '--timeout', '60')
        self.assertNotEqual(process.returncode, 0)
        self.assertEqual(result['verdict'], 'FAIL')
        # A transport timeout is not the intended missing-tools rejection.
        # Preserve the real diagnostic rather than obscuring it with KeyError.
        self.assertIsNone(result.get('error'), process.stdout + process.stderr)
        self.assertIn('missing current scenario tools', '\n'.join(result.get('failures', [])),
                      process.stdout + process.stderr)
        self.assert_cleaned(result)

    def test_stock_gate_is_blocked_without_booting(self):
        process, result = self.run_guest('--profile', 'conformance', '--scenario', 'stock-update-stage')
        self.assertEqual(process.returncode, 77)
        self.assertEqual(result['verdict'], 'BLOCKED')
        self.assertNotIn('qemu_pid', result)
        self.assertFalse(result['overlays'])

    def test_entropy_scenario_blocks_old_tools_instead_of_claiming_readiness(self):
        process, result = self.run_guest('--profile', 'conformance', '--scenario', 'entropy-ready',
                                        '--tools', str(VM / 'tools.dev.img'), '--timeout', '60')
        self.assertEqual(process.returncode, 77, process.stdout + process.stderr)
        self.assertEqual(result['verdict'], 'BLOCKED')
        self.assertNotIn('entropy-ready', result['checks'])
        self.assertIn('rebuild tools with entropy-seed', '\n'.join(result['blockers']))
        self.assert_cleaned(result)

    def test_command_timeout_cleans_up(self):
        start = time.monotonic()
        process, result = self.run_guest('--cmd', 'sleep 60', '--timeout', '10')
        self.assertNotEqual(process.returncode, 0)
        self.assertIn('Timeout', result['error'])
        self.assertLess(time.monotonic() - start, 25)
        self.assert_cleaned(result)

    def test_keep_overlay_retains_only_owned_disposable_image(self):
        process, result = self.run_guest('--scenario', 'boot', '--keep-overlay', '--timeout', '60')
        self.assertEqual(process.returncode, 0)
        self.assertTrue(result['overlays_retained'])
        for name, item in result['overlays'].items():
            path = Path(item['path'])
            self.assertEqual(path, Path(result['run_dir']) / f'{name}.qcow2')
            self.assertTrue(path.is_file())
            # Intentionally retain this test's overlay for post-mortem inspection.

    def test_split_storage_isolation_and_structured_audit(self):
        process, result = self.run_guest('--profile', 'conformance', '--scenario', 'storage-isolation',
                                        '--storage', 'split', '--storage-size', 'update=32',
                                        '--storage-size', 'rwdata=32', '--storage-size', 'logdata=32',
                                        '--audit', '--timeout', '90')
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(result['storage']['setup']['verdict'], 'PASS')
        self.assertEqual(result['checks']['storage-readonly'], 'PASS')
        self.assertEqual(result['checks']['storage-remount-persistence'], 'PASS')
        self.assertIn('synthetic-storage', result['applied_substitutions'])
        audit = json.loads(Path(result['audit']['path']).read_text())
        self.assertTrue(all(audit['writable_mounts'].values()))
        self.assertEqual(len({item['source'] for item in audit['writable_mounts'].values()}), 3)
        self.assertFalse(audit['stock_equivalence_verified'])
        self.assert_cleaned(result)

    def test_flat_environment_audit_requires_no_tools_image(self):
        process, result = self.run_guest('--profile', 'conformance', '--scenario', 'environment-audit',
                                        '--timeout', '60')
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        audit = json.loads(Path(result['audit']['path']).read_text())
        self.assertIsNone(audit['writable_mounts']['/update'])
        self.assertFalse(audit['stock_equivalence_verified'])
        self.assert_cleaned(result)

    def test_successful_audit_does_not_hide_guest_failure(self):
        process, result = self.run_guest('--cmd', 'exit 7', '--audit', '--timeout', '60')
        self.assertEqual(process.returncode, 7)
        self.assertEqual(result['audit']['status'], 'PASS')
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertEqual(result['guest_rc'], 7)
        self.assert_cleaned(result)

    def test_retained_split_overlays_keep_their_owned_backings(self):
        process, result = self.run_guest('--scenario', 'boot', '--storage', 'split', '--keep-overlay',
                                        '--storage-size', 'update=32', '--storage-size', 'rwdata=32',
                                        '--storage-size', 'logdata=32', '--timeout', '90')
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertTrue(result['generated_storage_retained'])
        for item in result['storage']['volumes'].values():
            self.assertTrue(Path(item['backing']).is_file())
            self.assertTrue(Path(result['overlays'][item['overlay_name']]['path']).is_file())

    def test_launcher_sigterm_cleans_owned_qemu(self):
        process = subprocess.Popen([sys.executable, '-B', str(VM / 'launch.py'), 'run',
                                    '--lock-timeout', '100',
                                    '--cmd', 'sleep 60', '--timeout', '90', '--storage', 'split',
                                    '--storage-size', 'update=32', '--storage-size', 'rwdata=32',
                                    '--storage-size', 'logdata=32'], cwd=VM,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        directory = None
        try:
            while True:
                line = process.stdout.readline()
                if not line:
                    self.fail('launcher exited before debugger readiness')
                if line.startswith('run: '):
                    directory = Path(line[5:].strip())
                if 'stopped at driver init' in line:
                    break
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=20)
            self.assertNotEqual(process.returncode, 0)
            result = json.loads((directory / 'result.json').read_text())
            self.assertIn('InterruptedError', result['error'])
            self.assert_cleaned(result)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=20)


if __name__ == '__main__':
    unittest.main()
