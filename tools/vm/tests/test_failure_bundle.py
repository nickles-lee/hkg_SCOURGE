# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import launch
from harness import failures


class FailureBundleTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-failure-")
        self.root = Path(self._tmp.name)
        (self.root / "serial.log").write_text("boot ok\nHARNESS_FAIL example\n")
        (self.root / "guest-output.log").write_text("guest output\n")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_bundle_collects_sections_and_excerpts(self) -> None:
        calls = []

        def fake_execute(ser, command, timeout):
            calls.append(command)
            return "token", "line one\nERROR: synthetic failure\ntail", 0, 0.01

        result = {"run_dir": str(self.root), "verdict": "FAIL", "rc": 1,
                  "guest_rc": 7, "scenario": {"name": "boot"},
                  "profile": {"name": "research"}, "error": "synthetic"}
        with patch.object(launch, "execute_guest", fake_execute):
            metadata = failures.collect_failure_bundle(object(), self.root, result, timeout=1)

        bundle = self.root / "failure"
        self.assertTrue((bundle / "summary.md").is_file())
        self.assertTrue((bundle / "dmesg.log").is_file())
        self.assertTrue((bundle / "weston.log").is_file())
        self.assertTrue((bundle / "serial-tail.log").is_file())
        self.assertIn("dmesg", metadata["sections"])
        self.assertEqual(len(calls), len(failures.SECTIONS))
        summary = (bundle / "summary.md").read_text()
        self.assertIn("synthetic failure", summary)
        self.assertIn("HARNESS_FAIL example", summary)
        self.assertEqual(result["failure_bundle"]["path"], str(bundle))

    def test_dead_shell_still_produces_bundle(self) -> None:
        def broken_execute(ser, command, timeout):
            raise TimeoutError("shell wedged")

        result = {"run_dir": str(self.root), "verdict": "FAIL", "rc": 1}
        with patch.object(launch, "execute_guest", broken_execute):
            metadata = failures.collect_failure_bundle(object(), self.root, result, timeout=0.1)
        self.assertIn("error", metadata["sections"]["dmesg"])
        self.assertTrue((self.root / "failure" / "summary.md").is_file())


if __name__ == "__main__":
    unittest.main()
