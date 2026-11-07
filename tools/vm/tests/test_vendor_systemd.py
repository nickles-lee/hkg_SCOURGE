# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest

import launch
from harness.config import load_scenario, scenario_command
from harness.results import evaluate_systemd_transition, systemd_events

PASS_TEXT = (
    'echo P28_SHELL pid=1 ppid=0\r\n'
    '[    4.5] systemd[1]: systemd 225 running in system mode. '
    '(-PAM +SMACK)\r\n'
    '[   10.0] systemd[1]: Listening on D-Bus System Message Bus Socket.\r\n'
    '[\x1b[32m  OK  \x1b[0m] Started D-Bus System Message Bus.\r\n'
    '[\x1b[32m  OK  \x1b[0m] Reached target Multi-User System.\r\n'
    '[   12.0] systemd[1]: Failed to start CredMan service.\r\n'
)


class VendorSystemdTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('vendor-systemd')
        self.assertEqual(scenario['init_override'], '/lib/systemd/systemd')
        self.assertEqual(scenario['boot_marker'], 'Reached target Multi-User System')
        self.assertEqual(scenario['requires_mode'], 'vendor')
        self.assertEqual(scenario['settle_seconds'], 2)
        self.assertEqual(set(scenario['checks']),
                         {'systemd-boot', 'multi-user-target'})

    def test_vendor_cmdline_override(self) -> None:
        cmdline = launch._vendor_cmdline(True, '/lib/systemd/systemd')
        self.assertIn('init=/lib/systemd/systemd', cmdline)
        self.assertNotIn('init=/init', cmdline)
        self.assertIn('noapic', cmdline)
        self.assertIn('init=/init', launch._vendor_cmdline(True))


class VendorSystemdDbusTests(unittest.TestCase):
    def setUp(self) -> None:
        self.scenario = load_scenario('vendor-systemd-dbus')
        self.transition = self.scenario['systemd_transition']

    def test_scenario_contract(self) -> None:
        self.assertEqual(self.scenario['requires_mode'], 'vendor')
        self.assertEqual(self.transition['exec'], '/lib/systemd/systemd')
        self.assertEqual(self.transition['ordering']['check'],
                         'dbus-before-multi-user')
        self.assertEqual(self.transition['final_marker'],
                         'Reached target Multi-User System')
        covered = {'systemd-prep', 'research-shell-pid1'}
        covered.update(self.transition['required_markers'].values())
        covered.add(self.transition['ordering']['check'])
        self.assertEqual(set(self.scenario['checks']), covered)
        command = scenario_command(self.scenario, {'name': 'conformance'})
        for text in ('/var/run', '/etc/passwd.legacy', 'messagebus',
                     'zz-p28-dbus.conf', 'P28_PREP_OK'):
            self.assertIn(text, command)

    def test_all_markers_and_ordering_pass(self) -> None:
        result = evaluate_systemd_transition(
            self.scenario, 0, 'P28_PREP_OK\n', PASS_TEXT)
        self.assertEqual(result['verdict'], 'PASS')
        self.assertEqual(result['rc'], 0)
        self.assertTrue(all(status == 'PASS' for status in result['checks'].values()))
        self.assertTrue(result['ordering'])
        self.assertEqual(result['exec_shell'], {'pid': 1, 'ppid': 0})
        self.assertIn('D-Bus System Message Bus', result['started_units'])
        self.assertIn('Multi-User System', result['reached_targets'])
        self.assertEqual(result['failed_unit_descriptions'],
                         ['CredMan service'])

    def test_missing_dbus_marker_fails_closed(self) -> None:
        text = PASS_TEXT.replace(
            'Started D-Bus System Message Bus.\r\n',
            'Failed to listen on D-Bus System Message Bus Socket.\r\n')
        result = evaluate_systemd_transition(
            self.scenario, 0, 'P28_PREP_OK\n', text)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertEqual(result['rc'], 1)
        self.assertEqual(result['checks']['dbus-service'], 'FAIL')
        self.assertFalse(result['markers']['Started D-Bus System Message Bus'])
        self.assertTrue(any('Started D-Bus System Message Bus' in failure
                            for failure in result['failures']))

    def test_wrong_ordering_fails(self) -> None:
        text = PASS_TEXT.replace(
            'Started D-Bus System Message Bus.\r\n'
            '[\x1b[32m  OK  \x1b[0m] Reached target Multi-User System.\r\n',
            '[\x1b[32m  OK  \x1b[0m] Reached target Multi-User System.\r\n'
            'Started D-Bus System Message Bus.\r\n')
        result = evaluate_systemd_transition(
            self.scenario, 0, 'P28_PREP_OK\n', text)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertEqual(result['checks']['dbus-before-multi-user'], 'FAIL')

    def test_non_pid1_handoff_fails_closed(self) -> None:
        text = PASS_TEXT.replace('P28_SHELL pid=1 ppid=0', 'P28_SHELL pid=7 ppid=1')
        result = evaluate_systemd_transition(
            self.scenario, 0, 'P28_PREP_OK\n', text)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertEqual(result['checks']['research-shell-pid1'], 'FAIL')
        self.assertEqual(result['exec_shell'], {'pid': 7, 'ppid': 1})

    def test_unmapped_check_fails_closed(self) -> None:
        scenario = dict(self.scenario)
        scenario['checks'] = list(scenario['checks']) + ['unmapped-check']
        result = evaluate_systemd_transition(
            scenario, 0, 'P28_PREP_OK\n', PASS_TEXT)
        self.assertEqual(result['checks']['unmapped-check'], 'FAIL')
        self.assertEqual(result['missing_checks'], ['unmapped-check'])
        self.assertEqual(result['verdict'], 'FAIL')

    def test_prep_failure_fails_closed(self) -> None:
        result = evaluate_systemd_transition(
            self.scenario, 1, 'P28_PREP_FAIL passwd\n', '')
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertEqual(result['checks']['systemd-prep'], 'FAIL')
        self.assertTrue(all(status == 'FAIL' for status in result['checks'].values()))

    def test_kernel_panic_fails(self) -> None:
        result = evaluate_systemd_transition(
            self.scenario, 0, 'P28_PREP_OK\n', PASS_TEXT + 'Kernel panic - not syncing\n')
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertIn('kernel panic', result['failures'])

    def test_systemd_events_strip_ansi_and_dedupe(self) -> None:
        events = systemd_events(PASS_TEXT + PASS_TEXT + '\x1b[?25h')
        self.assertEqual(events['started_units'], ['D-Bus System Message Bus'])
        self.assertEqual(events['reached_targets'], ['Multi-User System'])
        self.assertEqual(events['failed_unit_descriptions'], ['CredMan service'])


if __name__ == '__main__':
    unittest.main()
