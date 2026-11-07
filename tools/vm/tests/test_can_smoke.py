# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate


class CanSmokeTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('can-smoke')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(set(scenario['checks']),
                         {'can-ready', 'can-fixed', 'can-bulk', 'can-smoke'})
        self.assertNotIn('requires_mode', scenario)

    def test_command_calls_both_methods_and_asserts_markers(self) -> None:
        command = scenario_command(load_scenario('can-smoke'), load_profile('research'))
        self.assertIn('tweaks-micom-can-sim', command)
        self.assertIn('SetCANFixedMicomRecv', command)
        self.assertIn('SetCANMicomRecv', command)
        self.assertIn('TWEAKS_CAN_SIM_ACK', command)

    def test_checks(self) -> None:
        scenario = load_scenario('can-smoke')
        text = ('HARNESS_CHECK can-ready PASS\n'
                'HARNESS_CHECK can-fixed PASS\n'
                'HARNESS_CHECK can-bulk PASS\n'
                'HARNESS_CHECK can-smoke PASS\n')
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(text.replace('can-bulk PASS', 'can-bulk FAIL'),
                                  0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
