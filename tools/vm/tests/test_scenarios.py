# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness.config import SCENARIO_NAMES
from harness.scenarios import (profile_state, scenario_state,
                               validate_profile, validate_scenario)

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

    def test_live_profiles_validate(self) -> None:
        state = profile_state(VM)
        self.assertTrue(state['ok'], state['problems'])
        self.assertEqual(state['count'], 2)
        self.assertEqual(state['unregistered'], [])

    def test_profile_validation_reports_problems(self) -> None:
        bad = validate_profile('conformance', {
            'name': 'other', 'description': '',
            'allowed_workarounds': {'unknown-bypass': True},
            'limitations': []})
        joined = '\n'.join(bad)
        for expected in ('does not match', 'description', 'unknown keys',
                         'limitations'):
            self.assertIn(expected, joined)
        enabled = validate_profile('conformance', {
            'name': 'conformance', 'description': 'ok',
            'allowed_workarounds': {'dmclient-gui-patch': True},
            'limitations': ['one']})
        self.assertIn('cannot permit workarounds', '\n'.join(enabled))
        self.assertEqual(validate_profile('research', {
            'name': 'research', 'description': 'ok',
            'allowed_workarounds': {'dmclient-gui-patch': True},
            'limitations': ['one']}), [])

    def test_profile_state_flags_missing_and_extra(self) -> None:
        with tempfile.TemporaryDirectory(prefix='vm-profiles-') as tmp:
            root = Path(tmp) / 'profiles'
            root.mkdir()
            (root / 'research.json').write_text(json.dumps({
                'name': 'research', 'description': 'ok',
                'allowed_workarounds': {}, 'limitations': ['one']}))
            (root / 'extra.json').write_text('{}')
            state = profile_state(Path(tmp))
            self.assertFalse(state['ok'])
            self.assertIn('missing conformance.json',
                          '\n'.join(state['problems']))
            self.assertEqual(state['unregistered'], ['extra'])

    def test_check_scenario_all_cli(self) -> None:
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'check-scenario',
             '--all'], cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads(completed.stdout)
        self.assertTrue(report['ok'])
        self.assertEqual(report['count'], len(SCENARIO_NAMES))
        self.assertEqual(report['problems'], [])
        conflict = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'check-scenario',
             'boot', '--all'], cwd=VM, capture_output=True, text=True,
            check=False)
        self.assertEqual(conflict.returncode, 2)
        self.assertIn('not both', conflict.stderr)

    def test_check_scenario_cli(self) -> None:
        good = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'check-scenario',
             'boot'], cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(good.returncode, 0, good.stderr)
        self.assertTrue(json.loads(good.stdout)['ok'])

        with tempfile.TemporaryDirectory(prefix='vm-check-scenario-') as tmp:
            broken = Path(tmp) / 'broken.json'
            broken.write_text(json.dumps(
                {'name': 'broken', 'description': 'x', 'checks': []}))
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'check-scenario',
                 str(broken)], cwd=VM, capture_output=True, text=True,
                check=False)
            self.assertEqual(completed.returncode, 1)
            problems = '\n'.join(json.loads(completed.stdout)['problems'])
            self.assertIn('checks must be a non-empty list', problems)

        missing = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'check-scenario',
             'no-such-scenario'], cwd=VM, capture_output=True, text=True,
            check=False)
        self.assertEqual(missing.returncode, 1)
        self.assertIn('unknown scenario',
                      '\n'.join(json.loads(missing.stdout)['problems']))

    def test_check_profile_cli(self) -> None:
        good = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'check-profile',
             'research'], cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(good.returncode, 0, good.stderr)
        self.assertTrue(json.loads(good.stdout)['ok'])

        with tempfile.TemporaryDirectory(prefix='vm-check-profile-') as tmp:
            bad = Path(tmp) / 'bad-profile.json'
            bad.write_text(json.dumps({'description': ''}))
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'check-profile',
                 str(bad)], cwd=VM, capture_output=True, text=True,
                check=False)
            self.assertEqual(completed.returncode, 1)
            problems = '\n'.join(json.loads(completed.stdout)['problems'])
            self.assertIn('does not match', problems)

        missing = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'check-profile',
             'no-such-profile'], cwd=VM, capture_output=True, text=True,
            check=False)
        self.assertEqual(missing.returncode, 1)
        self.assertIn('unknown profile',
                      '\n'.join(json.loads(missing.stdout)['problems']))

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
