# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest
from pathlib import Path

import launch
from harness.config import load_profile, load_scenario, scenario_command
from harness.results import evaluate
from harness.screenshots import nonblack_bbox

VM = Path(__file__).resolve().parent.parent


class UiSmokeTests(unittest.TestCase):
    def test_scenario_is_compat_and_visible(self) -> None:
        scenario = load_scenario('ui-smoke')
        self.assertEqual(scenario['requires_mode'], 'compat')
        self.assertTrue(scenario['requires_visible_frame'])
        self.assertEqual(set(scenario['checks']),
                         {'weston-alive', 'wayland-probe', 'qml-ready', 'ui-smoke'})

    def test_command_runs_weston_probe_and_qml(self) -> None:
        command = scenario_command(load_scenario('ui-smoke'), load_profile('conformance'),
                                   kernel_target='compat')
        self.assertIn('drm-backend.so', command)
        self.assertIn('weston-simple-shm', command)
        self.assertIn('qmlscene', command)
        self.assertIn('UI_SMOKE_QML_READY', command)

    def test_vendor_mode_is_blocked_before_boot(self) -> None:
        self.assertEqual(
            launch.run(None, False, 5, profile_name='conformance', scenario_name='ui-smoke'),
            77,
        )

    def test_evaluate_requires_all_ui_checks(self) -> None:
        scenario = load_scenario('ui-smoke')
        text = (
            'HARNESS_CHECK weston-alive PASS\n'
            'HARNESS_CHECK wayland-probe PASS\n'
            'HARNESS_CHECK qml-ready PASS\n'
            'HARNESS_CHECK ui-smoke PASS\n'
        )
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')
        self.assertEqual(evaluate(text.replace('qml-ready PASS', 'qml-ready FAIL'),
                                  0, scenario)['verdict'], 'FAIL')

    def test_committed_probe_frame_has_visible_pixels(self) -> None:
        image = VM / 'docs' / 'screenshots' / 'mode-c-probe.png'
        pixels = nonblack_bbox(image)
        if pixels is None:
            self.skipTest('Pillow unavailable')
        self.assertTrue(pixels['bbox'])
        self.assertGreater(pixels['count'], 1000)


if __name__ == '__main__':
    unittest.main()
