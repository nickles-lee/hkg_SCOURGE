# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest

from harness.config import load_profile, load_scenario, scenario_command


class RecoveryTests(unittest.TestCase):
    def test_reboot_scenario_contract(self) -> None:
        scenario = load_scenario('recovery-reboot')
        self.assertTrue(scenario['expect_qemu_exit'])
        self.assertEqual(scenario['checks'], ['reboot-marker'])
        command = scenario_command(scenario, load_profile('conformance'))
        self.assertIn('reboot-marker PASS', command)
        self.assertIn('reboot -f', command)


if __name__ == '__main__':
    unittest.main()
