# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import base64
import json
import socket
import tempfile
import threading
import unittest
from pathlib import Path

import launch
from harness.qmp import QmpError, screendump

PNG_1X1 = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAFgwJ/lK3Q6wAAAABJRU5ErkJggg=="
)


class FakeQmpServer(threading.Thread):
    def __init__(self, path: Path, error: bool = False) -> None:
        super().__init__(daemon=True)
        self.path = path
        self.error = error
        self.ready = threading.Event()
        self.failure: BaseException | None = None

    def run(self) -> None:
        try:
            server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            server.bind(str(self.path))
            server.listen(1)
            self.ready.set()
            conn, _ = server.accept()
            handle = conn.makefile("rwb", buffering=0)
            handle.write(b'{"QMP": {"version": {"qemu": {"major": 11}}}}\n')
            while True:
                line = handle.readline()
                if not line:
                    break
                request = json.loads(line)
                command = request.get("execute")
                if self.error and command == "screendump":
                    handle.write(b'{"error": {"desc": "synthetic failure"}}\n')
                    continue
                if command == "screendump":
                    output = Path(request["arguments"]["filename"])
                    output.write_bytes(PNG_1X1)
                handle.write(b'{"return": {}}\n')
            handle.close()
            conn.close()
            server.close()
        except BaseException as exc:  # noqa: BLE001
            self.failure = exc
            self.ready.set()


class QmpScreendumpTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-qmp-")
        self.root = Path(self._tmp.name)
        self.sock = self.root / "qmp.sock"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_screendump_writes_png_and_reports_metadata(self) -> None:
        server = FakeQmpServer(self.sock)
        server.start()
        self.assertTrue(server.ready.wait(2))
        output = self.root / "shot.png"
        status = screendump(self.sock, output, timeout=5)
        self.assertEqual((status["width"], status["height"]), (1, 1))
        self.assertEqual(status["size"], len(PNG_1X1))
        self.assertEqual(output.read_bytes(), PNG_1X1)
        server.join(2)
        self.assertIsNone(server.failure)

    def test_qmp_error_is_surfaced(self) -> None:
        server = FakeQmpServer(self.sock, error=True)
        server.start()
        self.assertTrue(server.ready.wait(2))
        with self.assertRaises(QmpError):
            screendump(self.sock, self.root / "shot.png", timeout=5)

    def test_missing_socket_times_out(self) -> None:
        with self.assertRaises(QmpError):
            screendump(self.sock, self.root / "shot.png", timeout=0.2)

    def test_qemu_argv_display_and_qmp(self) -> None:
        overlay = self.root / "rootfs.qcow2"
        common = {"overlays": {"rootfs": {"path": str(overlay)}}}
        std = launch.qemu_argv(self.root, display_device="std", qmp_socket=self.sock, **common)
        self.assertEqual(std[std.index("-vga") + 1], "std")
        self.assertIn(f"unix:{self.sock},server=on,wait=off", std)
        virtio = launch.qemu_argv(self.root, display_device="virtio-vga", **common)
        self.assertEqual(virtio[virtio.index("-vga") + 1], "none")
        self.assertTrue(any(item.startswith("virtio-vga") for item in virtio))
        self.assertIn("virtio-vga,xres=1280,yres=720", virtio)
        none = launch.qemu_argv(self.root, **common)
        self.assertEqual(none[none.index("-vga") + 1], "none")
        self.assertNotIn("-qmp", none)


if __name__ == "__main__":
    unittest.main()
