# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest

from harness.config import load_scenario
from harness.results import evaluate


class KnownIssueTests(unittest.TestCase):
    def test_update_graphics_is_expected_fail(self) -> None:
        scenario = load_scenario('update-graphics')
        self.assertTrue(scenario['expected_fail'])
        self.assertEqual(scenario['known_issue']['id'], 'qt-shutdown-hang')
        self.assertEqual(scenario['known_issue']['probe'], 'RUNBOOK.md')
        result = evaluate('HARNESS_CHECK qt-egl-context FAIL\n', 1, scenario)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertTrue(result['known_failure'])

    def test_entropy_expected_fail_is_storage_scoped(self) -> None:
        scenario = load_scenario('entropy-ready')
        flat = evaluate('HARNESS_CHECK entropy-ready FAIL\n', 1, scenario,
                        context={'storage': 'flat'})
        split = evaluate('HARNESS_CHECK entropy-ready FAIL\n', 1, scenario,
                         context={'storage': 'split'})
        self.assertTrue(flat['known_failure'])
        self.assertFalse(split['known_failure'])

    def test_updater_timeout_note_does_not_mask_failures(self) -> None:
        scenario = load_scenario('updater-launch')
        self.assertFalse(scenario.get('expected_fail', False))
        result = evaluate('HARNESS_CHECK updater-exec FAIL\n', 1, scenario)
        self.assertFalse(result['known_failure'])
        self.assertEqual(result['known_issue']['id'], 'timeout-540')


if __name__ == '__main__':
    unittest.main()
