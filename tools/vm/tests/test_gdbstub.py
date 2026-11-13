# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import socket
import tempfile
import threading
import time
import unittest
from pathlib import Path

import launch
from patch_kernel import COMPAT, EXPECTED, TABLE_ENTRY


def _packet(payload: bytes) -> bytes:
    return b"$" + payload + b"#" + f"{sum(payload) & 0xff:02x}".encode()


class FakeProc:
    returncode = None

    def poll(self):
        return None


class FakeStub:
    """Minimal GDB remote stub for patch_sdhci tests."""

    def __init__(self, path: Path, *, status: bytes = b"T05",
                 stop: bytes = b"T05", table: bytes = EXPECTED,
                 breakpoint_reply: bytes = b"OK", listen_delay: float = 0.0):
        self.path = path
        self.status = status
        self.stop = stop
        self.table = table
        self.breakpoint_reply = breakpoint_reply
        self.listen_delay = listen_delay
        self.errors: list[BaseException] = []
        self.commands: list[bytes] = []
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> "FakeStub":
        self.thread.start()
        return self

    def join(self, timeout: float = 3.0) -> None:
        self.thread.join(timeout)
        if self.errors:
            raise self.errors[0]

    def _run(self) -> None:
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            server.bind(str(self.path))
            if self.listen_delay:
                time.sleep(self.listen_delay)
            server.listen(1)
            server.settimeout(5)
            conn, _ = server.accept()
            self._handle(conn)
        except BaseException as exc:  # noqa: BLE001
            self.errors.append(exc)
        finally:
            server.close()

    def _packet(self, conn: socket.socket, buf: bytes) -> tuple[bytes | None, bytes]:
        while b"$" not in buf:
            chunk = conn.recv(4096)
            if not chunk:
                return None, b""
            buf += chunk
        buf = buf[buf.index(b"$") + 1:]
        while b"#" not in buf or len(buf) < buf.index(b"#") + 3:
            chunk = conn.recv(4096)
            if not chunk:
                return None, b""
            buf += chunk
        end = buf.index(b"#")
        return buf[:end], buf[end + 3:]

    def _handle(self, conn: socket.socket) -> None:
        buf = b""
        try:
            while True:
                payload, buf = self._packet(conn, buf)
                if payload is None:
                    return
                self.commands.append(payload)
                if payload == b"?":
                    reply = self.status
                elif payload.startswith(b"Z1,"):
                    reply = self.breakpoint_reply
                elif payload == b"c":
                    reply = self.stop
                elif payload.startswith(b"m"):
                    reply = self.table.hex().encode()
                elif payload.startswith(b"M"):
                    self.table = bytes.fromhex(payload.split(b":", 1)[1].decode())
                    reply = b"OK"
                elif payload.startswith(b"z1,"):
                    reply = b"OK"
                elif payload == b"D":
                    conn.sendall(b"+" + _packet(b"OK"))
                    return
                else:
                    reply = b"E01"
                conn.sendall(b"+" + _packet(reply))
        finally:
            conn.close()


class GdbstubHardeningTests(unittest.TestCase):
    def run_patch(self, stub: FakeStub, timeout: float = 5.0) -> dict:
        with tempfile.TemporaryDirectory(prefix="vm-gdb-") as tmp:
            run_dir = Path(tmp)
            stub.path = run_dir / "gdb.sock"
            stub.start()
            try:
                return launch.patch_sdhci(run_dir, FakeProc(), timeout=timeout)
            finally:
                stub.join()

    def test_patch_round_trip_and_evidence(self) -> None:
        info = self.run_patch(FakeStub(Path("/unused")))
        self.assertEqual(info["stop_signal"], "T05")
        self.assertEqual(info["driver_init"], hex(launch.DRIVER_INIT))
        self.assertEqual(info["table_entry"], hex(TABLE_ENTRY))
        self.assertEqual(info["original"], EXPECTED.hex())
        self.assertEqual(info["replacement"], COMPAT.hex())
        self.assertGreaterEqual(info["connect_attempts"], 1)

    def test_connect_retries_until_stub_is_listening(self) -> None:
        info = self.run_patch(FakeStub(Path("/unused"), listen_delay=0.3))
        self.assertGreaterEqual(info["connect_attempts"], 2)

    def test_wrong_table_bytes_are_refused(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "SDHCI table bytes differ"):
            self.run_patch(FakeStub(Path("/unused"), table=b"\x00" * 20))

    def test_non_trap_stop_is_rejected(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "expected a breakpoint trap"):
            self.run_patch(FakeStub(Path("/unused"), stop=b"T02"))

    def test_failed_breakpoint_is_rejected(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "breakpoint insert failed"):
            self.run_patch(FakeStub(Path("/unused"), breakpoint_reply=b"E01"))

    def test_unexpected_status_is_rejected(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "unexpected gdbstub status"):
            self.run_patch(FakeStub(Path("/unused"), status=b"E99"))


if __name__ == '__main__':
    unittest.main()
