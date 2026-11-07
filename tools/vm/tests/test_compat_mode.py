# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import launch
from harness.config import load_profile, load_scenario, scenario_command


class CompatModeTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-compat-mode-")
        self.root = Path(self._tmp.name)
        self.overlays = {"rootfs": {"path": str(self.root / "rootfs.qcow2")}}

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_compat_argv_uses_initrd_virtio_blk_and_no_gdb(self) -> None:
        argv = launch.qemu_argv(self.root, overlays=self.overlays,
                                kernel_target="compat", noapic=True)
        self.assertEqual(argv[argv.index("-initrd") + 1],
                         str(launch.COMPAT_INITRAMFS))
        self.assertIn("virtio-blk-pci,drive=rootfs", argv)
        self.assertNotIn("sdhci-pci,id=sdhci0", argv)
        self.assertNotIn("-gdb", argv)
        self.assertIn("lsm=smack", argv[argv.index("-append") + 1])
        self.assertIn("noapic", argv[argv.index("-append") + 1])

    def test_vendor_argv_keeps_gdbstub_and_sdhci(self) -> None:
        argv = launch.qemu_argv(self.root, overlays=self.overlays)
        self.assertIn("sdhci-pci,id=sdhci0", argv)
        self.assertIn("sd-card,drive=rootfs", argv)
        self.assertIn("-gdb", argv)
        self.assertNotIn("-initrd", argv)

    def test_boot_scenario_is_mode_aware(self) -> None:
        profile = load_profile("conformance")
        scenario = load_scenario("boot")
        vendor = scenario_command(scenario, profile, kernel_target="vendor")
        compat = scenario_command(scenario, profile, kernel_target="compat")
        self.assertIn("4.1.27-abl", vendor)
        self.assertIn("/dev/vda", compat)
        self.assertNotIn("4.1.27-abl", compat)

    def test_compat_bootstrap_does_not_modprobe_vendor_modules(self) -> None:
        profile = load_profile("conformance")
        scenario = load_scenario("usb-discovery")
        command = scenario_command(scenario, profile, kernel_target="compat")
        self.assertNotIn("modprobe ehci-pci", command)
        self.assertIn("/media/tools", command)

    def test_compat_requires_initramfs(self) -> None:
        with patch.object(launch, "COMPAT_INITRAMFS", self.root / "missing.img"):
            with self.assertRaisesRegex(ValueError, "compat initramfs"):
                launch.run("true", False, 5, kernel_target="compat")

    def test_compat_refuses_kernel_pin_mismatch(self) -> None:
        with patch.object(launch, "verify_kernel_pin",
                          return_value={"ok": False, "problems": ["version changed"]}):
            with self.assertRaisesRegex(ValueError, "pin mismatch"):
                launch.run("true", False, 5, kernel_target="compat")


if __name__ == "__main__":
    unittest.main()
