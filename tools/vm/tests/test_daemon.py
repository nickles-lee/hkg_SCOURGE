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

    def test_list_daemons_describes_directories(self) -> None:
        ready = self.root / 'd1'
        ready.mkdir()
        daemon.write_status(ready, {'state': 'ready', 'daemon_pid': os.getpid(),
                                    'qemu_pid': 99999999})
        (ready / 'config.json').write_text(json.dumps(
            {'kernel_target': 'compat', 'state': '/tmp/state'}))
        stopped = self.root / 'd2'
        stopped.mkdir()
        daemon.write_status(stopped, {'state': 'stopped'})
        rows = {Path(row['dir']).name: row
                for row in daemon.list_daemons(self.root)}
        self.assertEqual(set(rows), {'d1', 'd2'})
        self.assertEqual(rows['d1']['kernel'], 'compat')
        self.assertEqual(rows['d1']['state_dir'], '/tmp/state')
        self.assertFalse(rows['d1']['daemon_alive'])
        self.assertFalse(rows['d1']['qemu_alive'])
        self.assertTrue(rows['d1']['stale'])
        self.assertEqual(rows['d2']['state'], 'stopped')
        self.assertFalse(rows['d2']['stale'])
        self.assertEqual(daemon.list_daemons(self.root / 'missing'), [])

    def test_daemon_list_state_filter(self) -> None:
        root = self.root / 'root'
        ready = root / 'ready'
        ready.mkdir(parents=True)
        daemon.write_status(ready, {'state': 'ready', 'daemon_pid': os.getpid(),
                                    'qemu_pid': 99999999})
        stopped = root / 'stopped'
        stopped.mkdir()
        daemon.write_status(stopped, {'state': 'stopped'})

        def listing(value):
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', 'list',
                 '--root', str(root), '--json', '--state', value],
                cwd=VM, capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            return {Path(row['dir']).name for row in json.loads(completed.stdout)}

        self.assertEqual(listing('ready'), {'ready'})
        self.assertEqual(listing('stale'), {'ready'})
        self.assertEqual(listing('stopped'), {'stopped'})
        self.assertEqual(listing('error'), set())

    def test_exec_rejects_invalid_timeout(self) -> None:
        for value in ('0', '-1', 'nan', 'inf'):
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', 'exec',
                 '--daemon-dir', str(self.root), '--cmd', 'true',
                 '--timeout', value],
                cwd=VM, capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 2, completed.stderr)
            self.assertIn('timeout must be finite and positive',
                          completed.stderr)

    def test_daemon_list_fail_stale_cli(self) -> None:
        stale_root = self.root / 'stale-root'
        stale = stale_root / 'daemon'
        stale.mkdir(parents=True)
        daemon.write_status(stale, {'state': 'ready', 'daemon_pid': os.getpid(),
                                    'qemu_pid': 99999999})
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', 'list',
             '--root', str(stale_root), '--json', '--fail-stale'],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 1, completed.stderr)
        rows = json.loads(completed.stdout)
        self.assertTrue(rows[0]['stale'])

        ok_root = self.root / 'ok-root'
        stopped = ok_root / 'daemon'
        stopped.mkdir(parents=True)
        daemon.write_status(stopped, {'state': 'stopped'})
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', 'list',
             '--root', str(ok_root), '--json', '--fail-stale'],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)

    def test_console_clamps_line_count(self) -> None:
        (self.root / 'serial.log').write_text('one\ntwo\nthree\n')
        for lines, expected in ((0, 'three'), (-5, 'three'),
                                (2, 'two\nthree'), (200, 'one\ntwo\nthree')):
            buffer = StringIO()
            with redirect_stdout(buffer):
                self.assertEqual(daemon.console(self.root, lines), 0)
            self.assertEqual(buffer.getvalue().strip(), expected)

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
            'command_log_max': 1234, 'screenshot_keep': 7,
            'exec_output_max': 4321, 'pin_cpu': 3,
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
                         '--command-log-max 1234', '--screenshot-keep 7',
                         '--exec-output-max 4321', '--pin-cpu 3'):
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

    def test_start_timeout_reaps_daemon(self) -> None:
        script = self.root / 'fake-launch.py'
        script.write_text(
            'import os, sys, time\n'
            'from pathlib import Path\n'
            'config = Path(sys.argv[sys.argv.index("--config") + 1])\n'
            'config.with_name("fake.pid").write_text(str(os.getpid()))\n'
            'time.sleep(60)\n')
        daemon_dir = self.root / 'daemon'
        started = time.monotonic()
        with self.assertRaisesRegex(TimeoutError, 'did not become ready'):
            daemon.start({'daemon_dir': str(daemon_dir)}, script, timeout=0.6)
        self.assertLess(time.monotonic() - started, 5)
        pid = int((daemon_dir / 'fake.pid').read_text())
        self.assertFalse(daemon.pid_alive(pid))

    def test_limit_output_keeps_tail(self) -> None:
        short = 'hello\n'
        self.assertEqual(daemon.limit_output(short, 100), (short, 6, False))
        limited, total, truncated = daemon.limit_output(
            'START' + 'x' * 100 + 'END', 10)
        self.assertTrue(truncated)
        self.assertEqual(total, 108)
        self.assertTrue(limited.endswith('END'))
        self.assertEqual(len(limited.encode()), 10)

    def test_prune_screenshots_keeps_newest(self) -> None:
        for index in range(4):
            path = self.root / f'screenshot-{index}.png'
            path.write_bytes(b'png')
            os.utime(path, (100 + index, 100 + index))
        removed = daemon.prune_screenshots(self.root, 2)
        self.assertEqual(sorted(removed), ['screenshot-0.png', 'screenshot-1.png'])
        remaining = sorted(path.name for path in self.root.glob('screenshot-*.png'))
        self.assertEqual(remaining, ['screenshot-2.png', 'screenshot-3.png'])
        self.assertEqual(daemon.prune_screenshots(self.root, 0), [])
        self.assertEqual(len(list(self.root.glob('screenshot-*.png'))), 2)

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
