# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import launch


class NetworkTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-network-")
        self.root = Path(self._tmp.name)
        self.overlays = {"rootfs": {"path": str(self.root / "rootfs.qcow2")}}

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_network_adds_user_net_and_virtio_nic(self) -> None:
        argv = launch.qemu_argv(self.root, overlays=self.overlays,
                                kernel_target="compat", network=True)
        self.assertIn("-netdev", argv)
        netdev = argv[argv.index("-netdev") + 1]
        self.assertIn("restrict=on", netdev)
        self.assertIn("hostfwd=tcp:127.0.0.1:2222-:22", netdev)
        self.assertIn("virtio-net-pci,netdev=net0", argv)
        self.assertNotIn("-netdev", launch.qemu_argv(
            self.root, overlays=self.overlays))

    def test_vendor_mode_rejects_network(self) -> None:
        with self.assertRaisesRegex(ValueError, "requires --kernel compat"):
            launch.run("true", False, 5, network=True)

    def test_conformance_rejects_network(self) -> None:
        with self.assertRaisesRegex(ValueError, "conformance"):
            launch.run(None, False, 5, profile_name="conformance",
                       scenario_name="boot", kernel_target="compat", network=True)


if __name__ == "__main__":
    unittest.main()
