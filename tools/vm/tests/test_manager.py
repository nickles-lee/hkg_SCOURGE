# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from pathlib import Path
import runpy
import subprocess
import sys
import unittest

VM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(VM))
from harness.manager import parse_manager_snapshots


class ManagerTests(unittest.TestCase):
    def test_paired_query_snapshots_do_not_promote_standby_11_to_update_ready(self):
        text = ('MANAGER_SNAPSHOT initial\nMANAGER_QUERY ask_standby_status\nmethod return sender=:1.2\n   int32 11\n'
                'MANAGER_QUERY ask_acc_status\n   int32 1\nMANAGER_QUERY ask_ign_status\n   int32 1\n')
        report = parse_manager_snapshots(text)
        self.assertEqual(report['snapshots']['initial']['ask_standby_status'], 11)
        self.assertFalse(report['update_ready_observed'])
        self.assertFalse(report['stock_equivalence_verified'])

    def test_missing_and_error_replies_remain_unknown(self):
        text = ('MANAGER_SNAPSHOT initial\nMANAGER_QUERY ask_standby_status\nError org.freedesktop.DBus.Error.UnknownMethod\n'
                'MANAGER_QUERY ask_acc_status\n=== later unrelated output ===\n   int32 1\n')
        report = parse_manager_snapshots(text)
        self.assertEqual(report['snapshots']['initial'], {'ask_standby_status': None, 'ask_acc_status': None})
        self.assertFalse(report['update_ready_observed'])

    def test_duplicate_identity_or_query_is_rejected(self):
        for text in ('MANAGER_SNAPSHOT initial\nMANAGER_SNAPSHOT initial\n',
                     'MANAGER_SNAPSHOT initial\nMANAGER_QUERY foo\nMANAGER_QUERY foo\n'):
            with self.assertRaises(ValueError):
                parse_manager_snapshots(text)

    def test_manager_control_and_ordered_probes_are_bounded_guest_scripts(self):
        module = runpy.run_path(str(VM / 'probes' / 'manager-initialization.py'))
        for ordered in (False, True):
            script = module['command'](ordered)
            syntax = subprocess.run(['bash', '-n'], input=script, text=True, capture_output=True)
            self.assertEqual(syntax.returncode, 0, syntax.stderr)
            self.assertIn('LIFECYCLE_STARTUP=' + ('ordered' if ordered else 'manual'), script)
            self.assertNotIn('appmanager-stub 12', script)
            self.assertNotIn('request.launch int32:311', script)
            self.assertIn('sleep 12', script)
            self.assertTrue(script.rstrip().endswith('exit 77'))

    def test_real_launcher_uses_recovered_manager_group_not_manual_launch_by_default(self):
        script = (VM / 'probes' / 'launcher-lifecycle.sh').read_text()
        self.assertIn('startup=${LIFECYCLE_STARTUP:-ordered}', script)
        self.assertIn('{"0":[{"appName":"ApplicationManager"}]}', script)
        self.assertIn('if [ "$startup" = manual ]; then', script)
        self.assertIn('HARNESS_CHECK manager-preload-complete PASS', script)


if __name__ == '__main__':
    unittest.main()
