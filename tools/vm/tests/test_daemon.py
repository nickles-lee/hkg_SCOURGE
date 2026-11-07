# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import os
import socket
import tempfile
import threading
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

from harness import daemon


class DaemonTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-daemon-")
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_status_round_trip_and_pid(self) -> None:
        daemon.write_status(self.root, {"state": "ready", "daemon_pid": os.getpid()})
        self.assertEqual(daemon.read_status(self.root)["state"], "ready")
        self.assertTrue(daemon.pid_alive(os.getpid()))
        self.assertFalse(daemon.pid_alive(99999999))

    def test_control_request_round_trip(self) -> None:
        path = self.root / "control.sock"
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(str(path))
        server.listen(1)
        seen = {}

        def serve():
            conn, _ = server.accept()
            handle = conn.makefile("rwb", buffering=0)
            request = json.loads(handle.readline())
            seen.update(request)
            handle.write((json.dumps({"ok": True, "output": "pong", "rc": 0}) + "\n").encode())
            handle.close()
            conn.close()
            server.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        response = daemon.request(self.root, {"action": "exec", "cmd": "echo pong"}, timeout=3)
        thread.join(2)
        self.assertEqual(response["output"], "pong")
        self.assertEqual(seen["action"], "exec")
        self.assertEqual(seen["cmd"], "echo pong")

    def test_console_reads_serial_tail(self) -> None:
        (self.root / "serial.log").write_text("one\ntwo\nthree\n")
        with redirect_stdout(StringIO()):
            self.assertEqual(daemon.console(self.root, lines=2), 0)


if __name__ == "__main__":
    unittest.main()
