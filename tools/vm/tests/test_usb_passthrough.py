# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import launch


class UsbPassthroughTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-usb-host-")
        self.root = Path(self._tmp.name)
        self.overlays = {"rootfs": {"path": str(self.root / "rootfs.qcow2")}}

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_usb_host_is_pinned_to_ehci_bus(self) -> None:
        argv = launch.qemu_argv(self.root, overlays=self.overlays,
                                kernel_target="compat", usb_host=["058f:6366"])
        self.assertIn("ich9-usb-ehci1,id=ehci0", argv)
        self.assertIn("usb-host,vendorid=0x058f,productid=0x6366,bus=ehci0.0", argv)
        self.assertNotIn("usb-host", launch.qemu_argv(
            self.root, overlays=self.overlays))

    def test_invalid_id_and_vendor_mode(self) -> None:
        with self.assertRaisesRegex(ValueError, "invalid USB host id"):
            launch.usb_host_option("nope")
        with self.assertRaisesRegex(ValueError, "invalid USB host id"):
            launch.run("true", False, 5, kernel_target="compat",
                       usb_host=["12345:1"])
        with self.assertRaisesRegex(ValueError, "requires --kernel compat"):
            launch.run("true", False, 5, usb_host=["058f:6366"])


if __name__ == "__main__":
    unittest.main()
