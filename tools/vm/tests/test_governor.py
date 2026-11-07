# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness.governor import LockTimeout, run_lock


class GovernorTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-governor-")
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_lock_serializes_and_times_out(self) -> None:
        with run_lock(self.root, timeout=1):
            with self.assertRaises(LockTimeout):
                with run_lock(self.root, timeout=0.2):
                    pass
        with run_lock(self.root, timeout=1):
            pass  # released and reacquirable

    def test_lock_file_is_under_runs(self) -> None:
        with run_lock(self.root, timeout=1) as info:
            path = self.root / "runs" / ".vm-lab.lock"
            self.assertTrue(path.is_file())
            self.assertTrue(info["held"])
            self.assertEqual(info["path"], str(path))
            self.assertGreaterEqual(info["waited_seconds"], 0)
            self.assertEqual(json.loads(path.read_text())["pid"], info["pid"])

    def test_waiter_does_not_clobber_holder_evidence(self) -> None:
        vm = Path(__file__).resolve().parents[1]
        try:
            with run_lock(vm, timeout=1) as info:
                completed = subprocess.run(
                    [sys.executable, "-B", str(vm / "launch.py"), "run",
                     "--cmd", "true", "--lock-timeout", "0"],
                    capture_output=True, text=True, timeout=30)
                self.assertEqual(completed.returncode, 1)
                self.assertIn("another VM boot holds", completed.stderr)
                record = json.loads((vm / "runs" / ".vm-lab.lock").read_text())
                self.assertTrue(record["held"])
                self.assertEqual(record["pid"], info["pid"])
        except LockTimeout:
            self.skipTest("VM lab is busy with another run")

    def test_daemon_start_queues_behind_the_boot_lock(self) -> None:
        vm = Path(__file__).resolve().parents[1]
        daemon_dir = self.root / "daemon"
        try:
            with run_lock(vm, timeout=1):
                completed = subprocess.run(
                    [sys.executable, "-B", str(vm / "launch.py"), "daemon",
                     "start", "--daemon-dir", str(daemon_dir),
                     "--ready-timeout", "2", "--lock-timeout", "0"],
                    capture_output=True, text=True, timeout=30)
                self.assertNotEqual(completed.returncode, 0)
                self.assertIn("lock timeout", completed.stderr)
        except LockTimeout:
            self.skipTest("VM lab is busy with another run")

    def test_invalid_timeout_rejected(self) -> None:
        for value in (float("nan"), float("inf"), -1):
            with self.assertRaises(ValueError):
                with run_lock(self.root, timeout=value):
                    pass

    def test_cli_exposes_lock_controls(self) -> None:
        launch = Path(__file__).resolve().parents[1] / "launch.py"
        run_help = subprocess.run(
            [sys.executable, "-B", str(launch), "run", "--help"],
            capture_output=True, text=True, check=True).stdout
        self.assertIn("--no-lock", run_help)
        self.assertIn("--lock-timeout", run_help)
        power_help = subprocess.run(
            [sys.executable, "-B", str(launch), "recovery-power-loss", "--help"],
            capture_output=True, text=True, check=True).stdout
        self.assertIn("--no-lock", power_help)
        self.assertIn("--lock-timeout", power_help)
        daemon_help = subprocess.run(
            [sys.executable, "-B", str(launch), "daemon", "start", "--help"],
            capture_output=True, text=True, check=True).stdout
        self.assertIn("--lock-timeout", daemon_help)


if __name__ == "__main__":
    unittest.main()
