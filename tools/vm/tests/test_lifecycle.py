# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

from pathlib import Path
import re
import runpy
import subprocess
import sys
import unittest

VM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(VM))
from harness.config import load_profile, load_scenario, scenario_command
from harness.lifecycle import parse_lifecycle, parse_manager_registrations
from harness.results import evaluate


def trace(exit_signal='SIGTERM', timestamp=True):
    times = ('41.000000 ', '41.001000 ', '41.002000 ', '41.003000 ') if timestamp else ('',) * 4
    return ('LIFECYCLE_CLIENT_PID 934\n'
            'HARNESS_CHECK real-manager-bus PASS\n'
            'HARNESS_CHECK client-launcher-ack PASS\n'
            'HARNESS_CHECK appkill-is-bookkeeping PASS\n'
            f'873 {times[0]}kill(934, SIGTERM) = 0\n'
            f'873 {times[1]}kill(934, SIGTERM) = 0\n'
            f'873 {times[2]}kill(934, SIGKILL) = 0\n'
            f'873 {times[3]}wait4(934, [{{WIFSIGNALED(s) && WTERMSIG(s) == {exit_signal}}}], WNOHANG, NULL) = 934\n')


class LifecycleTests(unittest.TestCase):
    def test_sigkill_request_is_not_proof_of_sigkill_exit(self):
        report = parse_lifecycle(trace())
        self.assertTrue(report['sigkill_requested'])
        self.assertEqual(report['observed_exit_signal'], 'SIGTERM')
        self.assertTrue(report['reaped'])
        self.assertFalse(report['normal_exit_observed'])
        self.assertFalse(report['stock_equivalence_verified'])
        self.assertEqual(report['signal_sequence_span_ms'], 2.0)

    def test_sigkill_exit_is_recorded_only_from_wait_status(self):
        self.assertEqual(parse_lifecycle(trace('SIGKILL'))['observed_exit_signal'], 'SIGKILL')

    def test_older_trace_without_timestamps_preserves_unknown_timing(self):
        report = parse_lifecycle(trace(timestamp=False))
        self.assertIsNone(report['signal_sequence_span_ms'])
        self.assertTrue(report['reaped'])

    def test_no_wait_observation_means_exit_is_unknown(self):
        report = parse_lifecycle('\n'.join(trace().splitlines()[:-1]) + '\n')
        self.assertIsNone(report['observed_exit_signal'])
        self.assertIsNone(report['normal_exit_observed'])
        self.assertFalse(report['reaped'])

    def test_other_pids_and_failed_signal_syscalls_do_not_count(self):
        text = 'LIFECYCLE_CLIENT_PID 934\n873 kill(999, SIGKILL) = 0\n873 kill(934, SIGKILL) = -1 ESRCH\n'
        self.assertFalse(parse_lifecycle(text)['sigkill_requested'])

    def test_missing_or_duplicate_identity_is_rejected(self):
        for text in ('', trace() + 'LIFECYCLE_CLIENT_PID 934\n'):
            with self.assertRaises(ValueError):
                parse_lifecycle(text)

    def test_conflicting_wait_status_is_rejected(self):
        text = trace() + '873 wait4(934, [{WIFSIGNALED(s) && WTERMSIG(s) == SIGKILL}], WNOHANG, NULL) = 934\n'
        with self.assertRaises(ValueError):
            parse_lifecycle(text)

    def test_managed_scenario_requires_real_services_and_exact_subset_checks(self):
        scenario = load_scenario('vendor-lifecycle')
        self.assertEqual(scenario['requires_storage'], 'split')
        self.assertIn('client-launcher-ack', scenario['checks'])
        self.assertIn('manager-plane1-registration', scenario['checks'])
        self.assertIn('unset QT_WAYLAND_SHELL_INTEGRATION', scenario_command(scenario, load_profile('conformance')))
        command = scenario_command(scenario, load_profile('conformance'))
        self.assertTrue(command.startswith('export LIFECYCLE_MANAGER=real LIFECYCLE_ACCEPT_COMPONENT=1'))
        syntax = subprocess.run(['bash', '-n'], input=command, text=True, capture_output=True)
        self.assertEqual(syntax.returncode, 0, syntax.stderr)
        self.assertEqual(evaluate('HARNESS_CHECK real-launcher-bus PASS\n', 0, scenario)['verdict'], 'FAIL')

    def test_launcher_ack_does_not_imply_manager_registration(self):
        report = parse_lifecycle(trace())
        self.assertTrue(report['launcher_native_ack_observed'])
        self.assertFalse(report['manager_plane1_registration_observed'])
        self.assertEqual(report['manager_registrations'], [])
        self.assertFalse(report['ivi_shell_selected_observed'])

    def test_plane_results_remain_separate(self):
        prefix = '[INFO] [ void AppEngineBase::registrationAppMain(AppID_t, QString, QString, QString, int) ][ 182 ] '
        log = (prefix + 'plane          : 1\n' + prefix + 'response is:  QVariant(int, 0)\n' +
               prefix + 'plane          : 4\n' + prefix + 'response is:  QVariant(int, -1)\n')
        report = parse_lifecycle(trace() + log)
        self.assertEqual(report['manager_registrations'], [{'plane': 1, 'result': 0}, {'plane': 4, 'result': -1}])
        self.assertTrue(report['manager_plane1_registration_observed'])

    def test_incomplete_registration_is_not_inferred_from_other_replies(self):
        prefix = '[ AppEngineBase::registrationAppMain(AppID_t) ] '
        log = (prefix + 'plane :  1\n[ Other::call() ] response is: QVariant(int, 0)\n' +
               prefix + 'plane :  4\n' + prefix + 'response is: QVariant(int, -1)\n')
        self.assertEqual(parse_manager_registrations(log), [{'plane': 4, 'result': -1}])

    def test_historical_ack_marker_is_only_a_launcher_ack(self):
        report = parse_lifecycle(trace().replace('client-launcher-ack', 'client-native-registration'))
        self.assertTrue(report['launcher_native_ack_observed'])
        self.assertFalse(report['manager_plane1_registration_observed'])

    def test_guest_registration_gate_accepts_padded_plane_and_retains_rejection(self):
        script = (VM / 'probes' / 'launcher-lifecycle.sh').read_text()
        awk = re.search(r"    awk '\n(.*?)\n    ' /log_data", script, re.S)[1]
        prefix = '[ AppEngineBase::registrationAppMain(AppID_t) ] '
        sample = (prefix + 'plane          : 1\n' + prefix + 'response is: QVariant(int, 0)\n' +
                  prefix + 'plane          : 4\n' + prefix + 'response is: QVariant(int, -1)\n')
        run = subprocess.run(['awk', awk], input=sample, text=True, capture_output=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(run.stdout.splitlines(), ['LIFECYCLE_MANAGER_REGISTRATION plane=1 result=0',
                                                  'LIFECYCLE_MANAGER_REGISTRATION plane=4 result=-1'])
        for missing in ('', sample.replace('plane          : 1', 'plane          : 2'),
                        sample.replace('QVariant(int, 0)', 'QVariant(int, -1)')):
            run = subprocess.run(['awk', awk], input=missing, text=True, capture_output=True)
            self.assertNotEqual(run.returncode, 0)

    def test_external_exit_diagnostic_is_syntax_valid_and_cannot_pass_component(self):
        module = runpy.run_path(str(VM / 'probes' / 'launcher-exit-notification.py'))
        command = module['command']()
        run = subprocess.run(['bash', '-n'], input=command, text=True, capture_output=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn('kill -TERM "$client_pid"', command)
        self.assertNotIn('com.lge.avn.applauncher.request.terminate int32:311', command)
        self.assertNotIn('com.lge.avn.applauncher.request.appkill string:AppDMClient', command)
        self.assertTrue(command.rstrip().endswith('exit 77'))


if __name__ == '__main__':
    unittest.main()
