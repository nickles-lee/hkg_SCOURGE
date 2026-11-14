# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest

from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate


class VehicleStateTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        scenario = load_scenario('vehicle-state')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(set(scenario['checks']),
                         {'vehicle-ready', 'vehicle-decode', 'vehicle-clamp',
                          'vehicle-range-guard', 'vehicle-frame-limit',
                          'vehicle-unknown-method', 'vehicle-bulk',
                          'vehicle-reset', 'vehicle-state-smoke'})
        self.assertNotIn('requires_mode', scenario)

    def test_command_asserts_state_and_lab_interface(self) -> None:
        command = scenario_command(
            load_scenario('vehicle-state'), load_profile('research'))
        for text in ('SetCANFixedMicomRecv', 'SetCANMicomRecv',
                     'org.scourge.vehicle.State.GetState',
                     'org.scourge.vehicle.State.Reset',
                     'TWEAKS_VEHICLE_STATE', 'TWEAKS_VEHICLE_CLAMP',
                     'TWEAKS_VEHICLE_RESET', 'TWEAKS_VEHICLE_BULK',
                     'TWEAKS_VEHICLE_WRITE_IGNORED',
                     'org.scourge.vehicle.State.Bogus',
                     'UnknownMethod', 'reason=frame-limit',
                     'speed=42 gear=3 ignition=2',
                     'climate=22.0 outside=-3'):
            self.assertIn(text, command)

    def test_checks(self) -> None:
        scenario = load_scenario('vehicle-state')
        text = ''.join(f'HARNESS_CHECK {check} PASS\n'
                       for check in scenario['checks'])
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(text.replace('vehicle-clamp PASS',
                                               'vehicle-clamp FAIL'),
                                  0, scenario)['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
