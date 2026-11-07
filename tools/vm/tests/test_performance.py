# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

import launch


class PinCpuTests(unittest.TestCase):
    def test_none_is_unpinned(self) -> None:
        self.assertIsNone(launch.validate_pin_cpu(None))

    def test_allowed_cpu_passes(self) -> None:
        allowed = sorted(os.sched_getaffinity(0))
        self.assertEqual(launch.validate_pin_cpu(allowed[0]), allowed[0])
        self.assertTrue(callable(launch.qemu_preexec(allowed[0])))
        self.assertTrue(callable(launch.qemu_preexec()))

    def test_preexec_actually_pins_the_child(self) -> None:
        cpu = sorted(os.sched_getaffinity(0))[0]
        completed = subprocess.run(
            [sys.executable, "-c",
             "import os; print(','.join(map(str, sorted(os.sched_getaffinity(0)))))"],
            capture_output=True, text=True, check=True,
            preexec_fn=launch.qemu_preexec(cpu))
        self.assertEqual(completed.stdout.strip(), str(cpu))

    def test_unavailable_cpu_rejected(self) -> None:
        allowed = os.sched_getaffinity(0)
        missing = max(allowed) + 1
        while missing in allowed:
            missing += 1
        with self.assertRaises(ValueError):
            launch.validate_pin_cpu(missing)

    def test_cli_exposes_pin_cpu(self) -> None:
        vm = Path(__file__).resolve().parents[1]
        for argv in (("run",), ("daemon", "start")):
            help_text = subprocess.run(
                [sys.executable, "-B", str(vm / "launch.py"), *argv, "--help"],
                capture_output=True, text=True, check=True).stdout
            self.assertIn("--pin-cpu", help_text)


if __name__ == '__main__':
    unittest.main()
