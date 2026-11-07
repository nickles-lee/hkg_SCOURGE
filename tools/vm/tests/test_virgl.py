# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import launch


class VirglTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-virgl-")
        self.root = Path(self._tmp.name)
        self.overlays = {"rootfs": {"path": str(self.root / "rootfs.qcow2")}}

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_egl_headless_virtio_vga_gl_argv(self) -> None:
        argv = launch.qemu_argv(self.root, overlays=self.overlays,
                                kernel_target="compat", display="egl-headless",
                                display_device="virtio-vga-gl")
        self.assertEqual(argv[argv.index("-display") + 1], "egl-headless")
        self.assertIn("virtio-vga-gl,xres=1280,yres=720", argv)
        self.assertNotIn("virtio-vga,", argv)


if __name__ == "__main__":
    unittest.main()
