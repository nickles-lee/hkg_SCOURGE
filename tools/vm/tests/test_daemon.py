# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

import launch
from harness import daemon

VM = Path(__file__).resolve().parents[1]


class DaemonTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-daemon-")
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_restart_argv_preserves_config(self) -> None:
        config = {
            'kernel_target': 'compat', 'state': '/tmp/state',
            'tools': '/tmp/tools.img', 'usb': '/tmp/usb.img',
            'payload': '/tmp/payload.img', 'share': '/tmp/share',
            'display': 'gtk', 'vnc_display': ':7',
            'display_device': 'virtio-vga', 'network': True, 'audio': True,
            'usb_host': ['058f:6366'], 'smp': 2, 'memory_mb': 3072,
            'apic': True, 'label': 'demo', 'cache': 'writethrough',
            'ready_timeout': 42, 'lock_timeout': 43,
            'command_log_max': 1234, 'pin_cpu': 3,
        }
        argv = launch.daemon_restart_argv(config, self.root)
        joined = ' '.join(argv)
        for expected in ('daemon start', '--daemon-dir', '--kernel compat',
                         '--state /tmp/state', '--tools /tmp/tools.img',
                         '--usb /tmp/usb.img', '--payload /tmp/payload.img',
                         '--share /tmp/share', '--display gtk',
                         '--vnc-display :7', '--display-device virtio-vga',
                         '--network', '--audio', '--usb-host 058f:6366',
                         '--smp 2', '--memory 3072', '--apic',
                         '--label demo', '--cache writethrough',
                         '--ready-timeout 42', '--lock-timeout 43',
                         '--command-log-max 1234', '--pin-cpu 3'):
            self.assertIn(expected, joined)
        self.assertNotIn('--reset', argv)
        self.assertIn('--reset', launch.daemon_restart_argv(
            config, self.root, reset=True))
        minimal = launch.daemon_restart_argv({}, self.root)
        self.assertEqual(minimal[-2:], ['--kernel', 'vendor'])
        self.assertNotIn('--network', minimal)
        self.assertNotIn('--audio', minimal)

    def test_start_lock_serializes_and_times_out(self) -> None:
        with daemon._start_lock(self.root, timeout=1):
            with self.assertRaises(RuntimeError):
                with daemon._start_lock(self.root, timeout=0.2):
                    pass
        with daemon._start_lock(self.root, timeout=1):
            pass  # released and reacquirable

    def test_serial_tail_is_bounded_and_decoded(self) -> None:
        class FakeSerial:
            log = bytearray(b"0123456789")
        self.assertEqual(daemon._serial_tail(FakeSerial(), limit=4), "6789")
        self.assertEqual(daemon._serial_tail(None), "")

    @unittest.skipUnless(
        (VM / "rootfs.img").is_file()
        and (VM / "compat" / "initramfs-linux.img").is_file(),
        "VM/compat inputs unavailable (public mirror)")
    def test_boot_failure_records_serial_tail_and_qemu_rc(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vm-daemon-fail-") as tmp:
            root = Path(tmp)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            fake = bin_dir / "qemu-system-x86_64"
            fake.write_text(
                "#!/bin/sh\n"
                "printf 'FAKE_QEMU_BOOT\\n'\n"
                "sleep 0.2\n"
                "exit 1\n")
            fake.chmod(0o755)
            daemon_dir = root / "daemon"
            env = {**os.environ,
                   "PATH": str(bin_dir) + os.pathsep + os.environ.get("PATH", "")}
            completed = subprocess.run(
                [sys.executable, "-B", str(VM / "launch.py"), "daemon", "start",
                 "--kernel", "compat", "--daemon-dir", str(daemon_dir),
                 "--ready-timeout", "3", "--lock-timeout", "10"],
                cwd=VM, env=env, capture_output=True, text=True, timeout=60)
            combined = completed.stdout + completed.stderr
            if "another VM boot holds" in combined or "lock timeout" in combined:
                self.skipTest("VM lab is busy with another run")
            self.assertNotEqual(completed.returncode, 0)
            status = json.loads((daemon_dir / "status.json").read_text())
            self.assertEqual(status["state"], "error")
            self.assertIn("FAKE_QEMU_BOOT", status["serial_tail"])
            self.assertEqual(status["qemu_returncode"], 1)
            self.assertIn("FAKE_QEMU_BOOT", combined)

    def test_status_round_trip_and_pid(self) -> None:
        daemon.write_status(self.root, {"state": "ready", "daemon_pid": os.getpid()})
        self.assertEqual(daemon.read_status(self.root)["state"], "ready")
        self.assertTrue(daemon.pid_alive(os.getpid()))
        self.assertFalse(daemon.pid_alive(99999999))

    def test_pid_identity_guard(self) -> None:
        # The test runner is alive but is not a `daemon _serve` process.
        self.assertFalse(daemon.daemon_pid_matches(os.getpid()))
        self.assertFalse(daemon.daemon_pid_matches(99999999))
        self.assertFalse(daemon.daemon_pid_matches(None))

    def test_command_log_rotation_cap(self) -> None:
        path = self.root / 'commands.log'
        daemon.append_command_log(path, 'a' * 10, 10)
        self.assertEqual(path.stat().st_size, 10)
        daemon.append_command_log(path, 'b' * 10, 10)
        self.assertTrue((self.root / 'commands.log.1').is_file())
        self.assertEqual(path.stat().st_size, 10)
        self.assertEqual((self.root / 'commands.log.1').read_bytes(), b'a' * 10)
        daemon.append_command_log(path, 'c' * 50, 10)
        self.assertEqual(path.stat().st_size, 10)
        self.assertEqual(path.read_bytes(), b'c' * 10)

    def test_missing_control_socket_fails_fast_when_ready(self) -> None:
        daemon.write_status(self.root, {"state": "ready", "daemon_pid": os.getpid()})
        started = time.monotonic()
        with self.assertRaisesRegex(RuntimeError, "control socket missing"):
            daemon.request(self.root, {"action": "status"}, timeout=2)
        self.assertLess(time.monotonic() - started, 1.5)

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
