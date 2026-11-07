# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

from pathlib import Path
import runpy
import subprocess
import sys
import unittest

VM = Path(__file__).resolve().parents[1]
if str(VM) not in sys.path:
    sys.path.insert(0, str(VM))
from harness.config import (SCENARIO_NAMES, load_profile, load_scenario,
                            scenario_command)
from harness.results import evaluate

PAYLOAD = VM.parent / 'applauncher_sigstub'
PROBE = VM / 'probes' / 'applauncher-sigstub.sh'
TOOLS_LAUNCHER = VM / 'toolsroot' / 'AppLauncher.sigstub'


def probe_checks(script: str) -> set[str]:
    import re
    return set(re.findall(r"echo 'HARNESS_CHECK ([\w-]+) PASS'", script))


class SigstubScenarioTests(unittest.TestCase):
    def test_scenarios_are_registered_with_split_storage_requirements(self):
        for name in ('applauncher-sigstub', 'vendor-lifecycle-sigstub'):
            self.assertIn(name, SCENARIO_NAMES)
            scenario = load_scenario(name)
            self.assertEqual(scenario['name'], name)
            self.assertEqual(scenario['requires'], ['tools', 'payload'])
            self.assertEqual(scenario['requires_storage'], 'split')
            self.assertTrue(scenario['checks'])

    def test_ab_probe_emits_exactly_the_declared_checks(self):
        scenario = load_scenario('applauncher-sigstub')
        script = PROBE.read_text()
        self.assertEqual(probe_checks(script), set(scenario['checks']))

    def test_ab_probe_only_exercises_the_preload_path(self):
        # On-demand request.launch records skip the sign check even on the
        # stock binary; the A/B must use startup-group preloads only.
        script = PROBE.read_text()
        self.assertNotIn('com.lge.avn.applauncher.request.launch', script)
        self.assertIn('{"0":[{"appName":"ApplicationManager"},{"appName":"AppDMClient"}]}', script)

    def test_ab_probe_asserts_both_refusal_modes_and_both_spawns(self):
        script = PROBE.read_text()
        self.assertIn("grep -Eq 'CheckSign : /app/bin/AppDMClient .*ret = 0'", script)
        self.assertIn("'readSection (elf_begin fail|get data fail|open fail)'", script)
        self.assertIn('execve("/app/bin/AppDMClient"', script)
        self.assertIn('execve("/app/bin/AppSetup"', script)
        self.assertIn('script-marker.txt', script)
        self.assertTrue(script.rstrip().endswith('exit 0'))

    def test_scenario_commands(self):
        research = load_profile('research')
        ab = scenario_command(load_scenario('applauncher-sigstub'), research)
        self.assertEqual(ab, PROBE.read_text())
        full = scenario_command(load_scenario('vendor-lifecycle-sigstub'), research)
        self.assertTrue(full.startswith(
            'export LIFECYCLE_SIGSTUB=1 LIFECYCLE_MANAGER=real LIFECYCLE_ACCEPT_COMPONENT=1'))
        for command in (ab, full):
            syntax = subprocess.run(['bash', '-n'], input=command, text=True, capture_output=True)
            self.assertEqual(syntax.returncode, 0, syntax.stderr)

    def test_scenarios_are_research_profile_only(self):
        conformance = load_profile('conformance')
        for name in ('applauncher-sigstub', 'vendor-lifecycle-sigstub'):
            with self.assertRaises(ValueError):
                scenario_command(load_scenario(name), conformance)

    def test_stock_vendor_lifecycle_command_is_unchanged(self):
        command = scenario_command(load_scenario('vendor-lifecycle'), load_profile('conformance'))
        self.assertTrue(command.startswith('export LIFECYCLE_MANAGER=real LIFECYCLE_ACCEPT_COMPONENT=1'))
        self.assertNotIn('export LIFECYCLE_SIGSTUB', command)
        self.assertIn('HARNESS_UNMODIFIED AppLauncher AppDMClient', command)

    def test_lifecycle_probe_swap_is_opt_in(self):
        script = (VM / 'probes' / 'launcher-lifecycle.sh').read_text()
        self.assertIn('${LIFECYCLE_SIGSTUB:-0}', script)
        self.assertIn('cp /media/tools/AppLauncher.sigstub /app/bin/AppLauncher', script)
        self.assertIn("echo 'HARNESS_UNMODIFIED AppLauncher AppDMClient'", script)

    def test_conformance_profile_forbids_the_substitution(self):
        scenario = load_scenario('applauncher-sigstub')
        text = 'HARNESS_SUBSTITUTION applauncher-sigstub\nHARNESS_CHECK stock-launcher-bus PASS\n'
        verdict = evaluate(text, 0, scenario, load_profile('conformance'))
        self.assertEqual(verdict['verdict'], 'FAIL')
        self.assertIn('profile forbids substitution: applauncher-sigstub', verdict['failures'])
        verdict = evaluate(text, 0, scenario, load_profile('research'))
        self.assertNotIn('profile forbids substitution: applauncher-sigstub', verdict['failures'])


class SigstubPatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = runpy.run_path(str(PAYLOAD / 'scripts' / 'patch_applauncher.py'))

    def synthetic(self):
        data = bytearray(self.module['EXPECTED_SIZE'])
        for _name, off, expect in self.module['PATCHES']:
            data[off:off + len(expect)] = expect
        return data

    def test_apply_and_restore_round_trip(self):
        data = self.synthetic()
        self.module['apply'](data)
        for _name, off, _expect in self.module['PATCHES']:
            self.assertEqual(bytes(data[off:off + 6]), bytes.fromhex('b801000000c3'))
        self.module['apply'](data)  # idempotent
        self.module['restore'](data)
        self.assertEqual(bytes(data), bytes(self.synthetic()))
        self.module['restore'](data)  # idempotent

    def test_size_guard(self):
        with self.assertRaises(SystemExit):
            self.module['apply'](bytearray(10))

    def test_stub_is_true_return_not_zero_return(self):
        # These gates are true-on-success; the stub must be mov eax,1; ret,
        # unlike the AppUpgrade validator stubs (31 C0 C3).
        self.assertEqual(self.module['STUB'], bytes.fromhex('b801000000c3'))

    def test_committed_tools_image_launcher_is_the_patched_stock_size(self):
        data = TOOLS_LAUNCHER.read_bytes()
        self.assertEqual(len(data), self.module['EXPECTED_SIZE'])
        for _name, off, _expect in self.module['PATCHES']:
            self.assertEqual(data[off:off + 6], bytes.fromhex('b801000000c3'),
                             f'stub missing at offset {off}')

    def test_payload_artifacts_agree_with_tools_image(self):
        tools = TOOLS_LAUNCHER.read_bytes()
        artifact = (PAYLOAD / 'artifacts' / 'AppLauncher.sigstub').read_bytes()
        self.assertEqual(tools, artifact)
        stock = (PAYLOAD / 'artifacts' / 'AppLauncher.stock').read_bytes()
        self.assertEqual(len(stock), self.module['EXPECTED_SIZE'])
        self.assertNotEqual(tools, stock)
        self.assertEqual(self.module['sha256'](stock), self.module['STOCK_SHA256'])


if __name__ == '__main__':
    unittest.main()
