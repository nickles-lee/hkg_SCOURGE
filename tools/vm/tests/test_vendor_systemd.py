# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest

import launch
from harness.config import load_scenario


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


if __name__ == '__main__':
    unittest.main()
