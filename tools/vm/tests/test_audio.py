# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import launch


class AudioTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-audio-")
        self.root = Path(self._tmp.name)
        self.overlays = {"rootfs": {"path": str(self.root / "rootfs.qcow2")}}

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_audio_adds_hda_devices(self) -> None:
        argv = launch.qemu_argv(self.root, overlays=self.overlays,
                                kernel_target="compat", audio=True)
        self.assertIn("intel-hda", argv)
        self.assertIn("hda-duplex,audiodev=audio0", argv)
        self.assertIn("none,id=audio0", argv)
        self.assertNotIn("-audiodev", launch.qemu_argv(
            self.root, overlays=self.overlays))

    def test_vendor_mode_rejects_audio(self) -> None:
        with self.assertRaisesRegex(ValueError, "requires --kernel compat"):
            launch.run("true", False, 5, audio=True)


if __name__ == "__main__":
    unittest.main()
