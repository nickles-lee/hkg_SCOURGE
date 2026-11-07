# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

import launch
from harness.artifacts import MAX_ARTIFACT_BYTES, extract_artifacts, wrap_command


class ArtifactChannelTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-artifacts-")
        self.root = Path(self._tmp.name)
        self.serial = self.root / "artifact-serial.bin"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_wrap_command_is_opt_in_and_installs_exit_trap(self) -> None:
        self.assertEqual(wrap_command("true", False), "true")
        wrapped = wrap_command("true", True)
        self.assertIn("trap __vm_artifact EXIT", wrapped)
        self.assertIn("/usr/libexec/weston-screenshooter", wrapped)
        self.assertIn("/dev/ttyS1", wrapped)
        self.assertTrue(wrapped.endswith("true"))

    def test_extracts_single_and_multiple_artifacts(self) -> None:
        png = b"\x89PNG\r\n\x1a\n" + b"\x00" * 32
        log = (
            b"noise\n"
            b"HARNESS_ARTIFACT wayland-screenshot.png " + str(len(png)).encode() + b"\n"
            + png
            + b"HARNESS_ARTIFACT second.log 5\nhello"
        )
        self.serial.write_bytes(log)
        artifacts = extract_artifacts(self.serial, self.root)
        self.assertEqual([item["name"] for item in artifacts],
                         ["wayland-screenshot.png", "second.log"])
        self.assertEqual((self.root / "wayland-screenshot.png").read_bytes(), png)
        self.assertEqual((self.root / "second.log").read_bytes(), b"hello")
        self.assertEqual(artifacts[0]["size"], len(png))

    def test_rejects_path_traversal_and_bad_names(self) -> None:
        payload = b"evil"
        self.serial.write_bytes(
            b"HARNESS_ARTIFACT ../escape " + str(len(payload)).encode() + b"\n" + payload
            + b"HARNESS_ARTIFACT /etc/passwd 4\nnope"
            + b"HARNESS_ARTIFACT ok_name 4\nsafe"
        )
        artifacts = extract_artifacts(self.serial, self.root)
        self.assertEqual([item["name"] for item in artifacts], ["ok_name"])
        self.assertFalse((self.root.parent / "escape").exists())
        self.assertFalse((self.root / "ok_name").read_bytes() == b"nope")

    def test_partial_payload_is_not_written(self) -> None:
        self.serial.write_bytes(b"HARNESS_ARTIFACT wayland-screenshot.png 100\nshort")
        artifacts = extract_artifacts(self.serial, self.root)
        self.assertEqual(len(artifacts), 1)
        self.assertEqual(artifacts[0]["error"], "truncated")
        self.assertEqual(artifacts[0]["expected_size"], 100)
        self.assertFalse((self.root / "wayland-screenshot.png").exists())

    def test_oversize_header_is_ignored(self) -> None:
        self.serial.write_bytes(
            b"HARNESS_ARTIFACT huge.bin " + str(MAX_ARTIFACT_BYTES + 1).encode() + b"\n"
        )
        self.assertEqual(extract_artifacts(self.serial, self.root), [])

    def test_qemu_argv_adds_second_serial(self) -> None:
        overlay = self.root / "rootfs.qcow2"
        serial = self.root / "artifact-serial.bin"
        argv = launch.qemu_argv(
            self.root,
            overlays={"rootfs": {"path": str(overlay)}},
            artifacts_serial=serial,
        )
        self.assertIn("file,id=artifacts,path=" + str(serial.resolve()), argv)
        self.assertIn("chardev:artifacts", argv)
        # The first serial stays the console.
        self.assertEqual(argv[argv.index("-serial") + 1], "stdio")


if __name__ == "__main__":
    unittest.main()
