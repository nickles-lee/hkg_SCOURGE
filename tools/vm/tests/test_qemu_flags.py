# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import launch


class DeterministicQemuTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-qemu-flags-")
        self.root = Path(self._tmp.name)
        self.overlays = {"rootfs": {"path": str(self.root / "rootfs.qcow2")}}

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_repeated_argv_is_identical_and_host_config_free(self) -> None:
        first = launch.qemu_argv(self.root, overlays=self.overlays)
        second = launch.qemu_argv(self.root, overlays=self.overlays)
        self.assertEqual(first, second)
        self.assertIn("-no-user-config", first)
        self.assertIn("-nodefaults", first)
        self.assertEqual(first[first.index("-name") + 1], "vm-lab")
        self.assertEqual(first[first.index("-uuid") + 1], launch.VM_UUID)
        self.assertEqual(first[first.index("-rtc") + 1], "base=utc")
        self.assertNotIn("-snapshot", first)

    def test_uuid_is_fixed_not_generated(self) -> None:
        self.assertEqual(launch.VM_UUID, "b3f5c8d2-4b2a-5f13-9a7e-6d0c1e8f4a21")
        self.assertEqual(len(launch.VM_UUID), 36)


    def test_smp_memory_and_apic_are_explicit(self) -> None:
        argv = launch.qemu_argv(self.root, overlays=self.overlays, smp=2,
                                memory_mb=2048, noapic=False)
        self.assertEqual(argv[argv.index("-smp") + 1], "2")
        self.assertEqual(argv[argv.index("-m") + 1], "2048")
        self.assertNotIn("noapic", argv[argv.index("-append") + 1])
        argv = launch.qemu_argv(self.root, overlays=self.overlays, smp=4,
                                memory_mb=4096, noapic=True)
        self.assertEqual(argv[argv.index("-smp") + 1], "4")
        self.assertIn("noapic", argv[argv.index("-append") + 1])

    def test_cache_mode_and_persistent_override(self) -> None:
        argv = launch.qemu_argv(self.root, overlays=self.overlays, cache="directsync")
        drive = next(item for item in argv if "rootfs.qcow2" in item)
        self.assertIn("cache=directsync", drive)
        persistent = {"rootfs": {"path": str(self.root / "rootfs.qcow2"),
                                 "persistent": True}}
        argv = launch.qemu_argv(self.root, overlays=persistent, cache="unsafe")
        drive = next(item for item in argv if "rootfs.qcow2" in item)
        self.assertIn("cache=writethrough", drive)

    def test_invalid_smp_and_memory_are_rejected_before_boot(self) -> None:
        with self.assertRaises(ValueError):
            launch.run("true", False, 5, smp=3)
        with self.assertRaises(ValueError):
            launch.run("true", False, 5, memory_mb=512)


if __name__ == "__main__":
    unittest.main()
