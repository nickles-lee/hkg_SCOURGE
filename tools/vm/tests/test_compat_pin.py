# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from harness import compat


class CompatPinTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-compat-pin-")
        self.root = Path(self._tmp.name)
        self.vmlinuz = self.root / "vmlinuz"
        self.vmlinuz.write_bytes(b"kernel")
        self.config = self.root / "config.gz"
        self.config.write_bytes(b"config")
        self.conf = self.root / "mkinitcpio.conf"
        self.conf.write_text("MODULES=()\n")
        self.initramfs = self.root / "initramfs.img"
        self.initramfs.write_bytes(b"initramfs")
        self.lock_path = self.root / "kernel.lock.json"
        self.lock = {
            "schema": 1,
            "kernel": {
                "package": "test",
                "version": os.uname().release,
                "vmlinuz": str(self.vmlinuz),
                "vmlinuz_sha256": compat.sha256_file(self.vmlinuz),
                "config": str(self.config),
                "config_sha256": compat.sha256_file(self.config),
            },
            "modules": {},
            "initramfs": {
                "builder": "compat/build-initramfs.sh",
                "config": str(self.conf),
                "config_sha256": compat.sha256_file(self.conf),
                "image": str(self.initramfs),
            },
        }
        self.lock_path.write_text(json.dumps(self.lock))

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _verify(self) -> dict:
        with patch.object(compat, "LOCK", self.lock_path), \
             patch.object(compat, "COMPAT", self.root), \
             patch.object(compat, "VM", self.root), \
             patch.object(compat, "INITRAMFS", self.initramfs):
            return compat.verify_kernel_pin()

    def test_valid_pin(self) -> None:
        status = self._verify()
        self.assertTrue(status["ok"], status)
        self.assertEqual(status["state"], "pinned")

    def test_tampered_kernel_is_rejected(self) -> None:
        self.vmlinuz.write_bytes(b"different")
        status = self._verify()
        self.assertFalse(status["ok"])
        self.assertIn("vmlinuz hash mismatch", " ".join(status["problems"]))

    def test_missing_lock_is_unpinned(self) -> None:
        self.lock_path.unlink()
        status = self._verify()
        self.assertEqual(status["state"], "unpinned")
        self.assertIn("pin-kernel.sh", status["detail"])

    def test_tampered_initramfs_config_is_rejected(self) -> None:
        self.conf.write_text("MODULES=(other)\n")
        status = self._verify()
        self.assertFalse(status["ok"])
        self.assertIn("mkinitcpio config differs", " ".join(status["problems"]))


if __name__ == "__main__":
    unittest.main()
