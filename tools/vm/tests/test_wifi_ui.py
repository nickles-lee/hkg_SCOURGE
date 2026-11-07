# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest

import launch
from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate


class WifiUiTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('wifi-ui')
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertTrue(scenario['requires_share'])
        self.assertEqual(set(scenario['checks']),
                         {'wifi-install', 'wifi-qml', 'wifi-ui'})

    def test_command_installs_and_evaluates_wifipage(self) -> None:
        command = scenario_command(load_scenario('wifi-ui'), load_profile('conformance'),
                                   kernel_target='compat')
        self.assertIn('run_stuff.sh', command)
        self.assertIn('WifiPage.qml', command)
        self.assertIn('qmlscene', command)

    def test_checks(self) -> None:
        scenario = load_scenario('wifi-ui')
        text = ('HARNESS_CHECK wifi-install PASS\n'
                'HARNESS_CHECK wifi-qml PASS\n'
                'HARNESS_CHECK wifi-ui PASS\n')
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate('HARNESS_CHECK wifi-install FAIL\n', 0,
                                  scenario)['verdict'], 'FAIL')

    def test_requires_compat_mode_and_share(self) -> None:
        self.assertEqual(launch.run(None, False, 5, profile_name='research',
                                    scenario_name='wifi-ui'), 77)
        self.assertEqual(launch.run(None, False, 5, profile_name='research',
                                    scenario_name='wifi-ui', kernel_target='compat'), 77)


if __name__ == '__main__':
    unittest.main()
