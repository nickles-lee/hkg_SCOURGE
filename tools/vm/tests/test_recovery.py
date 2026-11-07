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

    def test_no_space_scenario_contract(self) -> None:
        scenario = load_scenario('recovery-no-space')
        self.assertEqual(scenario['requires_storage'], 'split')
        self.assertEqual(scenario['storage_size'], {'update': 16})
        command = scenario_command(scenario, load_profile('conformance'))
        self.assertIn('no space', command)
        self.assertIn('/update/.no-space', command)

    def test_corrupt_media_scenario_contract(self) -> None:
        scenario = load_scenario('recovery-corrupt-media')
        self.assertEqual(scenario['requires'], ['usb'])
        self.assertIn('corrupt-detected', scenario['checks'])
        command = scenario_command(scenario, load_profile('research'))
        self.assertIn('MANIFEST.sha256', command)
        self.assertIn('IONIQ5_EU.ver', command)
        self.assertIn('corrupt-detected', command)

    def test_usb_removal_scenario_contract(self) -> None:
        scenario = load_scenario('recovery-usb-removal')
        self.assertEqual(scenario['requires'], ['usb'])
        self.assertEqual(scenario['usb_removal']['device_id'], 'usbdev')
        command = scenario_command(scenario, load_profile('research'))
        self.assertIn('USB_REMOVAL_READY_', command)
        self.assertIn('usb-cleanup', command)

    def test_power_loss_scenario_contract(self) -> None:
        scenario = load_scenario('recovery-power-loss')
        self.assertTrue(scenario['expect_qemu_exit'])
        self.assertEqual(scenario['expect_qemu_exit_marker'], 'POWER_LOSS_READY')
        self.assertEqual(scenario['power_loss']['marker'], 'POWER_LOSS_READY')
        command = scenario_command(scenario, load_profile('conformance'))
        self.assertIn('POWER_LOSS_MARKER', command)
        self.assertIn('sync', command)


if __name__ == '__main__':
    unittest.main()
