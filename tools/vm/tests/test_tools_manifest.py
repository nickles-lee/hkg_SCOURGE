# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from harness import tools


class ToolsManifestTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-tools-test-")
        self.root = Path(self._tmp.name) / "root"
        self.root.mkdir()
        (self.root / "phase4.sh").write_text("#!/bin/sh\necho one\n")
        (self.root / "scenarios.sh").write_text("#!/bin/sh\necho two\n")
        (self.root / "nested").mkdir()
        (self.root / "nested" / "helper").write_bytes(b"\x7fELF-not-really\n")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_manifest_is_deterministic_and_excludes_itself(self) -> None:
        first = tools.build_manifest(self.root)
        second = tools.build_manifest(self.root)
        self.assertEqual(first, second)
        self.assertEqual(first["schema"], tools.SCHEMA_VERSION)
        self.assertNotIn(tools.MANIFEST_NAME, first["files"])
        self.assertIn("phase4.sh", first["files"])
        self.assertIn("nested/helper", first["files"])

        written = tools.write_manifest(self.root)
        self.assertEqual(written, self.root / tools.MANIFEST_NAME)
        self.assertEqual(json.loads(written.read_text()), first)
        # The manifest itself must not change the build id.
        self.assertEqual(tools.build_manifest(self.root), first)

    def test_compare_detects_changed_missing_and_extra(self) -> None:
        expected = tools.build_manifest(self.root)
        (self.root / "phase4.sh").write_text("#!/bin/sh\necho changed\n")
        actual = tools.build_manifest(self.root)
        comparison = tools.compare_manifests(expected, actual)
        self.assertEqual(comparison["state"], "stale")
        self.assertEqual(comparison["changed"], ["phase4.sh"])
        self.assertEqual(comparison["only_in_tree"], [])
        self.assertEqual(comparison["only_in_image"], [])

        actual["files"].pop("scenarios.sh")
        comparison = tools.compare_manifests(expected, actual)
        self.assertEqual(comparison["only_in_tree"], ["scenarios.sh"])
        actual["files"]["extra"] = {"sha256": "0" * 64, "size": 0}
        comparison = tools.compare_manifests(expected, actual)
        self.assertEqual(comparison["only_in_image"], ["extra"])

    def test_unmanifested_image_is_not_fresh(self) -> None:
        image = Path(self._tmp.name) / "plain.img"
        image.write_text("not an ext4 filesystem\n")
        status = tools.check_tools(image, self.root)
        self.assertFalse(status["ok"])
        self.assertEqual(status["state"], "unmanifested")
        self.assertIn("build_tools.sh", status["detail"])

    @unittest.skipUnless(
        all(shutil.which(name) for name in ("fakeroot", "mkfs.ext4", "debugfs")),
        "fakeroot/mkfs.ext4/debugfs required for embedded-manifest test",
    )
    def test_embedded_manifest_is_fresh_then_stale(self) -> None:
        tools.write_manifest(self.root)
        image = Path(self._tmp.name) / "tools.img"
        image.write_bytes(b"")
        subprocess.run(
            ["truncate", "-s", "8M", str(image)],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["fakeroot", "mkfs.ext4", "-F", "-q", "-d", str(self.root), str(image)],
            check=True,
            capture_output=True,
        )

        status = tools.check_tools(image, self.root)
        self.assertTrue(status["ok"], status)
        self.assertEqual(status["state"], "fresh")
        self.assertEqual(status["build_id"], status["expected_build_id"])

        (self.root / "phase4.sh").write_text("#!/bin/sh\necho newer\n")
        status = tools.check_tools(image, self.root)
        self.assertFalse(status["ok"])
        self.assertEqual(status["state"], "stale")
        self.assertEqual(status["stale"], ["phase4.sh"])


if __name__ == "__main__":
    unittest.main()
