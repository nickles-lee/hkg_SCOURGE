# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

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
        with run_lock(self.root, timeout=1):
            self.assertTrue((self.root / "runs" / ".vm-lab.lock").is_file())


if __name__ == "__main__":
    unittest.main()
