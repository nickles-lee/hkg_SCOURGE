# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import hashlib
import subprocess
import tempfile
import unittest
from pathlib import Path

import launch
from harness.state import StateBusy, create_state_overlays


class StateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-state-")
        self.root = Path(self._tmp.name)
        self.source = self.root / "rootfs.raw"
        subprocess.run(["truncate", "-s", "1M", str(self.source)], check=True)
        self.state_dir = self.root / "state"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_reuse_and_reset_preserve_backing(self) -> None:
        before = hashlib.sha256(self.source.read_bytes()).hexdigest()
        overlays, lock = create_state_overlays(self.state_dir, {"rootfs": self.source})
        lock.close()
        path = Path(overlays["rootfs"]["path"])
        self.assertTrue(path.is_file())
        first_mtime = path.stat().st_mtime_ns

        reused, lock = create_state_overlays(self.state_dir, {"rootfs": self.source})
        lock.close()
        self.assertEqual(reused["rootfs"]["path"], str(path))
        self.assertEqual(path.stat().st_mtime_ns, first_mtime)

        reset, lock = create_state_overlays(self.state_dir, {"rootfs": self.source},
                                            reset=True)
        lock.close()
        self.assertEqual(reset["rootfs"]["path"], str(path))
        self.assertNotEqual(path.stat().st_mtime_ns, first_mtime)
        self.assertEqual(hashlib.sha256(self.source.read_bytes()).hexdigest(), before)

    def test_lock_prevents_concurrent_state(self) -> None:
        _, lock = create_state_overlays(self.state_dir, {"rootfs": self.source})
        try:
            with self.assertRaises(StateBusy):
                create_state_overlays(self.state_dir, {"rootfs": self.source})
        finally:
            lock.close()

    def test_reset_requires_state(self) -> None:
        with self.assertRaisesRegex(ValueError, "reset requires --state"):
            launch.run("true", False, 5, reset=True)

    def test_state_with_split_storage_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "flat storage"):
            launch.run("true", False, 5, state=self.state_dir, storage_mode="split")


if __name__ == "__main__":
    unittest.main()
