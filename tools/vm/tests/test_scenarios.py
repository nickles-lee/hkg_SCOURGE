# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from harness.config import SCENARIO_NAMES
from harness.scenarios import scenario_state, validate_scenario

VM = Path(__file__).resolve().parents[1]


def valid_scenario(**overrides):
    data = {
        'name': 'sample',
        'description': 'sample scenario',
        'checks': ['one', 'two'],
        'requires': ['usb'],
        'blockers': {'conformance': ['not now']},
    }
    data.update(overrides)
    return data


class ScenarioContractTests(unittest.TestCase):
    def test_all_live_scenarios_validate(self) -> None:
        state = scenario_state(VM)
        self.assertTrue(state['ok'], state['problems'])
        self.assertEqual(state['count'], len(SCENARIO_NAMES))
        self.assertEqual(state['unregistered'], [])

    def test_minimal_scenario_is_valid(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-scenarios-') as tmp:
            self.assertEqual(validate_scenario('sample', valid_scenario(), Path(tmp)), [])

    def test_invalid_contract_fields_are_reported(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-scenarios-') as tmp:
            data = valid_scenario(
                name='other', checks=['one', 'one'], requires=['share'],
                requires_mode='other', blockers={'staging': []},
                requires_storage='raid', storage_size={'update': 0},
                known_issue={'probe': 'probes/notes.md'},
                init_override='/init', boot_marker=None,
                expect_qemu_exit='yes', expected_fail=[],
                power_loss={'marker': '', 'delay': -1},
                usb_removal={}, settle_seconds=-1,
                requires_visible_frame='yes',
                systemd_transition={'required_markers': {}, 'ordering': {}})
            problems = validate_scenario('sample', data, Path(tmp))
            joined = '\n'.join(problems)
            for expected in ('does not match', 'checks must be unique',
                             'requires must be a subset', 'requires_mode',
                             'blockers keys', 'requires_storage',
                             'storage_size', 'excluded from the public sync',
                             'requires boot_marker', 'required_markers',
                             'ordering requires', 'expect_qemu_exit must be boolean',
                             'expect_qemu_exit requires',
                             'expected_fail must be boolean',
                             'power_loss.marker', 'power_loss.delay',
                             'usb_removal.marker', 'usb_removal.device_id',
                             'settle_seconds', 'requires_visible_frame'):
                self.assertIn(expected, joined)

    def test_runbook_probe_accepts_both_layouts(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-scenarios-') as tmp:
            root = Path(tmp)
            (root / 'docs').mkdir()
            (root / 'docs' / 'RUNBOOK.md').write_text('runbook\n')
            data = valid_scenario(known_issue={'probe': 'RUNBOOK.md'})
            self.assertEqual(validate_scenario('sample', data, root), [])
        with tempfile.TemporaryDirectory(prefix='vm-scenarios-') as tmp:
            problems = validate_scenario(
                'sample', data, Path(tmp))
            self.assertIn('missing in both layouts', '\n'.join(problems))


if __name__ == '__main__':
    unittest.main()
