# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import launch
from harness import share


class ShareTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-share-")
        self.root = Path(self._tmp.name)
        self.overlays = {"rootfs": {"path": str(self.root / "rootfs.qcow2")}}

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_mount_script_and_qemu_option(self) -> None:
        script = share.mount_script()
        self.assertIn("mount -t 9p", script)
        self.assertIn("/media/host", script)
        self.assertIn("hostshare", script)
        option = share.qemu_option(self.root)
        self.assertIn("mount_tag=hostshare", option)
        self.assertIn("security_model=none", option)

    def test_qemu_argv_only_adds_virtfs_in_compat(self) -> None:
        compat = launch.qemu_argv(self.root, overlays=self.overlays,
                                  kernel_target="compat", share=self.root)
        self.assertIn("-virtfs", compat)
        self.assertTrue(any("mount_tag=hostshare" in item for item in compat))
        vendor = launch.qemu_argv(self.root, overlays=self.overlays)
        self.assertNotIn("-virtfs", vendor)

    def test_vendor_mode_rejects_share(self) -> None:
        with self.assertRaisesRegex(ValueError, "requires --kernel compat"):
            launch.run("true", False, 5, share=self.root)

    def test_missing_share_directory_is_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "share directory missing"):
            launch.run("true", False, 5, kernel_target="compat",
                       share=self.root / "missing")


if __name__ == "__main__":
    unittest.main()
