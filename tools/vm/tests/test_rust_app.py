# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

from pathlib import Path

import unittest

from harness.config import SCENARIO_NAMES, load_profile, load_scenario, scenario_command
from harness.scenarios import validate_scenario

VM = Path(__file__).resolve().parent.parent


class RustAppScenarioTests(unittest.TestCase):
    def test_scenario_contract(self) -> None:
        self.assertIn('rust-app', SCENARIO_NAMES)
        scenario = load_scenario('rust-app')
        self.assertEqual(scenario['requires'], ['payload'])
        self.assertEqual(scenario['requires_mode'], 'vendor')
        self.assertEqual(
            set(scenario['checks']),
            {'rust-install', 'rust-static', 'rust-daemon', 'rust-status',
             'rust-shutdown', 'rust-smack', 'rust-idempotent', 'rust-restore'},
        )
        self.assertIn('not a car test', scenario['description'].lower())
        self.assertEqual(validate_scenario('rust-app', scenario, VM), [])

    def test_probe_contains_every_check_marker(self) -> None:
        scenario = load_scenario('rust-app')
        command = scenario_command(scenario, load_profile('research'))
        for check in scenario['checks']:
            self.assertIn(f'HARNESS_CHECK {check} PASS', command)

    def test_probe_requires_payload_marker(self) -> None:
        command = (VM / 'probes' / 'rust-app.sh').read_text()
        self.assertIn('/payload/app.env', command)
        self.assertIn('HARNESS_BLOCKED rust-payload missing', command)


if __name__ == '__main__':
    unittest.main()
