# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Opt-in tests: vendor programs execute only in the isolated QEMU guest."""
from __future__ import annotations

import fcntl
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest

VM = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.environ.get('IONIQ_RUN_VM_TESTS') == '1', 'set IONIQ_RUN_VM_TESTS=1 to boot QEMU')
class VMIntegrationTests(unittest.TestCase):
    def run_guest(self, *args):
        process = subprocess.run([sys.executable, '-B', str(VM / 'launch.py'), 'run',
                                  '--lock-timeout', '100', *args],
                                 cwd=VM, capture_output=True, text=True, timeout=120)
        directory = next(line[5:] for line in process.stdout.splitlines() if line.startswith('run: '))
        result = json.loads((Path(directory) / 'result.json').read_text())
        return process, result

    def assert_cleaned(self, result):
        self.assertFalse(result['overlays_retained'])
        for overlay in result['overlays'].values():
            self.assertFalse(Path(overlay['path']).exists())
        self.assertFalse(result['generated_storage_retained'])
        for item in result['storage']['volumes'].values():
            self.assertFalse(Path(item['backing']).exists())
        if result.get('qemu_pid'):
            self.assertFalse(Path(f"/proc/{result['qemu_pid']}").exists())

    def test_daemon_screenshot_copy_failure(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-copyfail-') as tmp:
            root = Path(tmp)
            daemon_dir = root / 'daemon'
            output = root / 'missing-parent' / 'shot.png'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                failed = cli('screenshot', '--daemon-dir', str(daemon_dir),
                             '--output', str(output))
                self.assertNotEqual(failed.returncode, 0,
                                    failed.stdout + failed.stderr)
                self.assertIn('cannot copy screenshot',
                              failed.stdout + failed.stderr)
                self.assertFalse(output.exists())
                self.assertEqual(list(daemon_dir.glob('screenshot-*.png')), [])

                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo COPY_FAILURE_SAFE')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('COPY_FAILURE_SAFE', alive.stdout)

                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)
                restarted = cli('start', '--daemon-dir', str(daemon_dir),
                                '--ready-timeout', '120', '--lock-timeout', '120')
                self.assertEqual(restarted.returncode, 0,
                                 restarted.stdout + restarted.stderr)
                again = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo RESTART_AFTER_COPY_FAIL')
                self.assertEqual(again.returncode, 0,
                                 again.stdout + again.stderr)
                self.assertIn('RESTART_AFTER_COPY_FAIL', again.stdout)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_stop_idempotent_and_post_stop_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-poststop-') as tmp:
            root = Path(tmp)
            daemon_dir = root / 'daemon'
            output = root / 'post-stop.png'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                first = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(first.returncode, 0,
                                 first.stdout + first.stderr)
                second = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(second.returncode, 0,
                                 second.stdout + second.stderr)
                values = json.loads(second.stdout)
                self.assertEqual(values['state'], 'stopped')

                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertNotEqual(status.returncode, 0)
                values = json.loads(status.stdout)
                self.assertEqual(values['state'], 'stopped')
                self.assertFalse(values['reachable'])
                self.assertIn('control socket', values['reachable_error'])

                exec_fail = cli('exec', '--daemon-dir', str(daemon_dir),
                                '--cmd', 'echo SHOULD_NOT_RUN')
                self.assertNotEqual(exec_fail.returncode, 0)
                self.assertIn('control socket',
                              exec_fail.stdout + exec_fail.stderr)

                shot_fail = cli('screenshot', '--daemon-dir', str(daemon_dir),
                                '--output', str(output))
                self.assertNotEqual(shot_fail.returncode, 0)
                self.assertIn('control socket',
                              shot_fail.stdout + shot_fail.stderr)
                self.assertFalse(output.exists())

                restarted = cli('start', '--daemon-dir', str(daemon_dir),
                                '--ready-timeout', '120', '--lock-timeout', '120')
                self.assertEqual(restarted.returncode, 0,
                                 restarted.stdout + restarted.stderr)
                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo POST_STOP_RESTART_OK')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('POST_STOP_RESTART_OK', alive.stdout)
            finally:
                cli('stop', '--daemon-dir', str(daemon_dir))

    def test_run_state_lock_contention(self):
        with tempfile.TemporaryDirectory(prefix='vm-run-state-lock-') as tmp:
            state = Path(tmp) / 'state'
            state.mkdir()
            holder = open(state / 'state.lock', 'w')
            fcntl.flock(holder.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)

            def run(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'run',
                     '--state', str(state), '--timeout', '60', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            try:
                blocked = run('--cmd', 'true')
                combined = blocked.stdout + blocked.stderr
                self.assertNotEqual(blocked.returncode, 0, combined)
                self.assertIn('state is already in use', combined)
                directory = next(Path(line[5:].strip())
                                 for line in blocked.stdout.splitlines()
                                 if line.startswith('run: '))
                result = json.loads((directory / 'result.json').read_text())
                self.assertEqual(result['verdict'], 'FAIL')
                self.assertIn('state is already in use', result.get('error') or '')
                self.assertNotIn('qemu_pid', result)

                fcntl.flock(holder.fileno(), fcntl.LOCK_UN)
                holder.close()

                clean = run('--cmd', 'echo STATE_LOCK_RUN_OK')
                combined = clean.stdout + clean.stderr
                if 'another VM boot holds' in combined or 'lock timeout' in combined:
                    self.skipTest('VM lab is busy with another run')
                self.assertEqual(clean.returncode, 0, combined)
                directory = next(Path(line[5:].strip())
                                 for line in clean.stdout.splitlines()
                                 if line.startswith('run: '))
                self.assertIn('STATE_LOCK_RUN_OK',
                              (directory / 'guest-output.log').read_text())
            finally:
                try:
                    fcntl.flock(holder.fileno(), fcntl.LOCK_UN)
                except (OSError, ValueError):
                    pass
                holder.close()

    def test_run_state_persistence_and_reset(self):
        with tempfile.TemporaryDirectory(prefix='vm-run-state-') as tmp:
            state = Path(tmp) / 'state'

            def run(*args):
                completed = subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'run',
                     '--state', str(state), '--lock-timeout', '100', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)
                combined = completed.stdout + completed.stderr
                if 'another VM boot holds' in combined or 'lock timeout' in combined:
                    self.skipTest('VM lab is busy with another run')
                self.assertEqual(completed.returncode, 0, combined)
                directory = next(Path(line[5:].strip())
                                 for line in completed.stdout.splitlines()
                                 if line.startswith('run: '))
                return json.loads((directory / 'result.json').read_text())

            written = run('--cmd',
                          "mkdir -p /rw_data && printf 'run-state-marker\\n' "
                          "> /rw_data/run-state-marker && sync", '--timeout', '60')
            self.assertEqual(written['verdict'], 'PASS')
            read_back = run('--cmd', 'cat /rw_data/run-state-marker',
                            '--timeout', '60')
            self.assertEqual(read_back['verdict'], 'PASS')
            self.assertIn('run-state-marker',
                          (Path(read_back['run_dir']) / 'guest-output.log').read_text())
            reset = run('--reset', '--cmd',
                        'if [ -e /rw_data/run-state-marker ]; then echo STILL_THERE; '
                        'else echo RESET_CLEAN; fi', '--timeout', '60')
            self.assertEqual(reset['verdict'], 'PASS')
            self.assertIn('RESET_CLEAN',
                          (Path(reset['run_dir']) / 'guest-output.log').read_text())

    def test_failure_bundle_disabled(self):
        process, result = self.run_guest(
            '--cmd', 'echo NO_BUNDLE_MARKER; exit 7',
            '--no-failure-bundle', '--timeout', '60')
        self.assertEqual(process.returncode, 7)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertEqual(result['guest_rc'], 7)
        self.assertIsNone(result.get('failure_bundle'))
        self.assert_cleaned(result)

    def test_failure_bundle_real_fail(self):
        process, result = self.run_guest(
            '--cmd', 'echo BUNDLE_MARKER; exit 7', '--timeout', '60')
        self.assertEqual(process.returncode, 7)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertEqual(result['guest_rc'], 7)
        bundle = result.get('failure_bundle') or {}
        self.assertTrue(bundle.get('path'))
        root = Path(bundle['path'])
        self.assertTrue((root / 'summary.md').is_file())
        self.assertTrue((root / 'serial-tail.log').is_file())
        self.assertIn('dmesg', bundle.get('sections', {}))
        summary = (root / 'summary.md').read_text()
        self.assertIn('Failure bundle', summary)
        self.assertIn('BUNDLE_MARKER', summary)
        self.assert_cleaned(result)

    def test_guest_exit_7_propagates(self):
        process, result = self.run_guest('--cmd', 'exit 7', '--timeout', '60')
        self.assertEqual(process.returncode, 7)
        self.assertEqual(result['guest_rc'], 7)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assert_cleaned(result)

    def test_false_success_marker_is_failure(self):
        process, result = self.run_guest('--cmd', "printf 'PHASE4_FAIL intentional\\n'; exit 0", '--timeout', '60')
        self.assertEqual(result['guest_rc'], 0)
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertNotEqual(process.returncode, 0)
        self.assert_cleaned(result)

    def test_missing_current_tools_is_failure(self):
        process, result = self.run_guest('--scenario', 'dbus-ready', '--tools',
                                        str(VM / 'tools.img'), '--timeout', '60')
        self.assertNotEqual(process.returncode, 0)
        self.assertEqual(result['verdict'], 'FAIL')
        # A transport timeout is not the intended missing-tools rejection.
        # Preserve the real diagnostic rather than obscuring it with KeyError.
        self.assertIsNone(result.get('error'), process.stdout + process.stderr)
        self.assertIn('missing current scenario tools', '\n'.join(result.get('failures', [])),
                      process.stdout + process.stderr)
        self.assert_cleaned(result)

    def test_stock_gate_is_blocked_without_booting(self):
        process, result = self.run_guest('--profile', 'conformance', '--scenario', 'stock-update-stage')
        self.assertEqual(process.returncode, 77)
        self.assertEqual(result['verdict'], 'BLOCKED')
        self.assertNotIn('qemu_pid', result)
        self.assertFalse(result['overlays'])

    def test_entropy_scenario_blocks_old_tools_instead_of_claiming_readiness(self):
        process, result = self.run_guest('--profile', 'conformance', '--scenario', 'entropy-ready',
                                        '--tools', str(VM / 'tools.dev.img'), '--timeout', '60')
        self.assertEqual(process.returncode, 77, process.stdout + process.stderr)
        self.assertEqual(result['verdict'], 'BLOCKED')
        self.assertNotIn('entropy-ready', result['checks'])
        self.assertIn('rebuild tools with entropy-seed', '\n'.join(result['blockers']))
        self.assert_cleaned(result)

    def test_command_timeout_cleans_up(self):
        start = time.monotonic()
        process, result = self.run_guest('--cmd', 'sleep 60', '--timeout', '10')
        self.assertNotEqual(process.returncode, 0)
        self.assertIn('Timeout', result['error'])
        self.assertLess(time.monotonic() - start, 25)
        self.assert_cleaned(result)

    def test_keep_overlay_retains_only_owned_disposable_image(self):
        process, result = self.run_guest('--scenario', 'boot', '--keep-overlay', '--timeout', '60')
        self.assertEqual(process.returncode, 0)
        self.assertTrue(result['overlays_retained'])
        for name, item in result['overlays'].items():
            path = Path(item['path'])
            self.assertEqual(path, Path(result['run_dir']) / f'{name}.qcow2')
            self.assertTrue(path.is_file())
            # Intentionally retain this test's overlay for post-mortem inspection.

    def test_split_storage_isolation_and_structured_audit(self):
        process, result = self.run_guest('--profile', 'conformance', '--scenario', 'storage-isolation',
                                        '--storage', 'split', '--storage-size', 'update=32',
                                        '--storage-size', 'rwdata=32', '--storage-size', 'logdata=32',
                                        '--audit', '--timeout', '90')
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(result['storage']['setup']['verdict'], 'PASS')
        self.assertEqual(result['checks']['storage-readonly'], 'PASS')
        self.assertEqual(result['checks']['storage-remount-persistence'], 'PASS')
        self.assertIn('synthetic-storage', result['applied_substitutions'])
        audit = json.loads(Path(result['audit']['path']).read_text())
        self.assertTrue(all(audit['writable_mounts'].values()))
        self.assertEqual(len({item['source'] for item in audit['writable_mounts'].values()}), 3)
        self.assertFalse(audit['stock_equivalence_verified'])
        self.assert_cleaned(result)

    def test_flat_environment_audit_requires_no_tools_image(self):
        process, result = self.run_guest('--profile', 'conformance', '--scenario', 'environment-audit',
                                        '--timeout', '60')
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        audit = json.loads(Path(result['audit']['path']).read_text())
        self.assertIsNone(audit['writable_mounts']['/update'])
        self.assertFalse(audit['stock_equivalence_verified'])
        self.assert_cleaned(result)

    def test_successful_audit_does_not_hide_guest_failure(self):
        process, result = self.run_guest('--cmd', 'exit 7', '--audit', '--timeout', '60')
        self.assertEqual(process.returncode, 7)
        self.assertEqual(result['audit']['status'], 'PASS')
        self.assertEqual(result['verdict'], 'FAIL')
        self.assertEqual(result['guest_rc'], 7)
        self.assert_cleaned(result)

    def test_retained_split_overlays_keep_their_owned_backings(self):
        process, result = self.run_guest('--scenario', 'boot', '--storage', 'split', '--keep-overlay',
                                        '--storage-size', 'update=32', '--storage-size', 'rwdata=32',
                                        '--storage-size', 'logdata=32', '--timeout', '90')
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertTrue(result['generated_storage_retained'])
        for item in result['storage']['volumes'].values():
            self.assertTrue(Path(item['backing']).is_file())
            self.assertTrue(Path(result['overlays'][item['overlay_name']]['path']).is_file())

    def test_daemon_pin_cpu_is_applied(self):
        cpu = sorted(os.sched_getaffinity(0))[0]
        with tempfile.TemporaryDirectory(prefix='vm-daemon-pin-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'
            start = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', 'start',
                 '--daemon-dir', str(daemon_dir), '--pin-cpu', str(cpu),
                 '--ready-timeout', '60', '--lock-timeout', '100'],
                cwd=VM, capture_output=True, text=True, timeout=120)
            self.assertEqual(start.returncode, 0, start.stdout + start.stderr)
            try:
                status = json.loads((daemon_dir / 'status.json').read_text())
                self.assertEqual(status['pin_cpu'], cpu)
                limits = Path(f"/proc/{status['qemu_pid']}/status").read_text()
                self.assertIn(f"Cpus_allowed_list:\t{cpu}", limits)
            finally:
                subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', 'stop',
                     '--daemon-dir', str(daemon_dir)],
                    cwd=VM, capture_output=True, text=True, timeout=60)

    def test_daemon_recovers_from_corrupt_state(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-corrupt-') as tmp:
            root = Path(tmp)
            state = root / 'state'
            daemon_dir = root / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            try:
                started = cli('start', '--daemon-dir', str(daemon_dir),
                              '--state', str(state),
                              '--ready-timeout', '120', '--lock-timeout', '120')
                combined = started.stdout + started.stderr
                if 'another VM boot holds' in combined or 'lock timeout' in combined:
                    self.skipTest('VM lab is busy with another run')
                self.assertEqual(started.returncode, 0, combined)
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

                overlay = state / 'rootfs.qcow2'
                self.assertTrue(overlay.is_file())
                overlay.write_bytes(b'not a qcow2 image')
                failed = cli('start', '--daemon-dir', str(daemon_dir),
                             '--state', str(state),
                             '--ready-timeout', '30', '--lock-timeout', '30')
                self.assertNotEqual(failed.returncode, 0,
                                    failed.stdout + failed.stderr)
                status = json.loads((daemon_dir / 'status.json').read_text())
                self.assertEqual(status['state'], 'error')
                self.assertNotIn('qemu_pid', status)

                reset = cli('start', '--daemon-dir', str(daemon_dir),
                            '--state', str(state), '--reset',
                            '--ready-timeout', '120', '--lock-timeout', '120')
                self.assertEqual(reset.returncode, 0,
                                 reset.stdout + reset.stderr)
                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo CORRUPT_STATE_RECOVERED')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('CORRUPT_STATE_RECOVERED', alive.stdout)
            finally:
                cli('stop', '--daemon-dir', str(daemon_dir))

    def test_daemon_rejects_state_backing_mismatch(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-mismatch-') as tmp:
            root = Path(tmp)
            state = root / 'state'
            state.mkdir()
            daemon_dir = root / 'daemon'
            wrong = root / 'wrong.raw'
            wrong.write_bytes(b'\0' * 1024 * 1024)
            subprocess.run(
                ['qemu-img', 'create', '-f', 'qcow2', '-F', 'raw',
                 '-b', str(wrong), str(state / 'rootfs.qcow2')],
                check=True, capture_output=True)

            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', 'start',
                 '--daemon-dir', str(daemon_dir), '--state', str(state),
                 '--ready-timeout', '30', '--lock-timeout', '30'],
                cwd=VM, capture_output=True, text=True, timeout=120)
            combined = completed.stdout + completed.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertNotEqual(completed.returncode, 0, combined)
            self.assertIn('backing mismatch', combined)
            status = json.loads((daemon_dir / 'status.json').read_text())
            self.assertEqual(status['state'], 'error')
            self.assertNotIn('qemu_pid', status)

    def test_daemon_screenshot_outside_dir_rejected(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-outside-') as tmp:
            root = Path(tmp)
            daemon_dir = root / 'daemon'
            outside = root / 'outside.png'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                from harness import daemon as daemon_mod
                response = daemon_mod.request(
                    daemon_dir, {'action': 'screenshot', 'output': str(outside)},
                    timeout=5)
                self.assertFalse(response['ok'])
                self.assertIn('outside daemon dir', response['error'])
                self.assertFalse(outside.exists())

                sneaky = daemon_dir / 'sneaky.png'
                sneaky.symlink_to(outside)
                linked = daemon_mod.request(
                    daemon_dir, {'action': 'screenshot', 'output': str(sneaky)},
                    timeout=5)
                self.assertFalse(linked['ok'])
                self.assertIn('outside daemon dir', linked['error'])
                self.assertFalse(outside.exists())
                sneaky.unlink()

                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo OUTSIDE_REJECT_SAFE')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('OUTSIDE_REJECT_SAFE', alive.stdout)
                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(status.returncode, 0, status.stderr)
                values = json.loads(status.stdout)
                self.assertEqual(values['state'], 'ready')
                self.assertTrue(values['qemu_alive'])
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_duplicate_start_rejected(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-duplicate-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                duplicate = cli('start', '--daemon-dir', str(daemon_dir),
                                '--ready-timeout', '120', '--lock-timeout', '120')
                self.assertNotEqual(duplicate.returncode, 0,
                                    duplicate.stdout + duplicate.stderr)
                self.assertIn('already running',
                              duplicate.stdout + duplicate.stderr)

                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo DUPLICATE_START_SAFE')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('DUPLICATE_START_SAFE', alive.stdout)
                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(status.returncode, 0, status.stderr)
                values = json.loads(status.stdout)
                self.assertEqual(values['state'], 'ready')
                self.assertTrue(values['qemu_alive'])
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_qmp_input(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-input-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            def input_cmd(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'input', *args],
                    cwd=VM, capture_output=True, text=True, timeout=60)

            started = cli('start', '--kernel', 'compat',
                          '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            qmp = daemon_dir / 'qmp.sock'
            try:
                key = input_cmd('--qmp', str(qmp), '--key', 'a')
                self.assertEqual(key.returncode, 0, key.stdout + key.stderr)
                key_result = json.loads(key.stdout)
                self.assertTrue(key_result['ok'])
                self.assertEqual(key_result['qcode'], 'a')

                click = input_cmd('--qmp', str(qmp), '--click', '100,100',
                                  '--width', '1280', '--height', '720')
                self.assertEqual(click.returncode, 0, click.stdout + click.stderr)
                click_result = json.loads(click.stdout)
                self.assertTrue(click_result['ok'])
                self.assertEqual((click_result['x'], click_result['y']), (100, 100))

                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(status.returncode, 0, status.stderr)
                values = json.loads(status.stdout)
                self.assertEqual(values['state'], 'ready')
                self.assertTrue(values['qemu_alive'])
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_recovers_from_corrupt_status(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-badstatus-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                (daemon_dir / 'status.json').write_text('{not json')
                observed = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertNotEqual(observed.returncode, 0)
                values = json.loads(observed.stdout)
                self.assertTrue(values.get('reachable'))
                self.assertTrue(values.get('qemu_alive'))

                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo CORRUPT_STATUS_SAFE')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('CORRUPT_STATUS_SAFE', alive.stdout)

                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)
                after = json.loads((daemon_dir / 'status.json').read_text())
                self.assertEqual(after['state'], 'stopped')

                restarted = cli('start', '--daemon-dir', str(daemon_dir),
                                '--ready-timeout', '120', '--lock-timeout', '120')
                self.assertEqual(restarted.returncode, 0,
                                 restarted.stdout + restarted.stderr)
                again = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo RESTARTED_FROM_BAD_STATUS')
                self.assertEqual(again.returncode, 0,
                                 again.stdout + again.stderr)
                self.assertIn('RESTARTED_FROM_BAD_STATUS', again.stdout)
            finally:
                cli('stop', '--daemon-dir', str(daemon_dir))

    def test_run_state_missing_overlay_rebuilt(self):
        with tempfile.TemporaryDirectory(prefix='vm-run-missing-') as tmp:
            state = Path(tmp) / 'state'

            def run(*args):
                completed = subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'run',
                     '--state', str(state), '--timeout', '60', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)
                combined = completed.stdout + completed.stderr
                if 'another VM boot holds' in combined or 'lock timeout' in combined:
                    self.skipTest('VM lab is busy with another run')
                self.assertEqual(completed.returncode, 0, combined)
                directory = next(Path(line[5:].strip())
                                 for line in completed.stdout.splitlines()
                                 if line.startswith('run: '))
                return json.loads((directory / 'result.json').read_text())

            written = run('--cmd',
                          "mkdir -p /rw_data && printf 'missing-overlay-marker\\n' "
                          "> /rw_data/missing-overlay-marker && sync")
            self.assertEqual(written['verdict'], 'PASS')
            overlay = state / 'rootfs.qcow2'
            self.assertTrue(overlay.is_file())
            overlay.unlink()

            rebuilt = run('--cmd',
                          'if [ -e /rw_data/missing-overlay-marker ]; then '
                          'echo STILL_THERE; else echo MISSING_OVERLAY_CLEAN; fi')
            self.assertEqual(rebuilt['verdict'], 'PASS')
            self.assertIn('MISSING_OVERLAY_CLEAN',
                          (Path(rebuilt['run_dir']) / 'guest-output.log').read_text())
            self.assertTrue(overlay.is_file())

    def test_run_state_backing_mismatch(self):
        with tempfile.TemporaryDirectory(prefix='vm-run-mismatch-') as tmp:
            root = Path(tmp)
            state = root / 'state'
            state.mkdir()
            wrong = root / 'wrong.raw'
            wrong.write_bytes(b'\0' * 1024 * 1024)
            subprocess.run(
                ['qemu-img', 'create', '-f', 'qcow2', '-F', 'raw',
                 '-b', str(wrong), str(state / 'rootfs.qcow2')],
                check=True, capture_output=True)

            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'run',
                 '--state', str(state), '--cmd', 'true', '--timeout', '30'],
                cwd=VM, capture_output=True, text=True, timeout=120)
            combined = completed.stdout + completed.stderr
            self.assertNotEqual(completed.returncode, 0, combined)
            self.assertIn('backing mismatch', combined)
            directory = next(Path(line[5:].strip())
                             for line in completed.stdout.splitlines()
                             if line.startswith('run: '))
            result = json.loads((directory / 'result.json').read_text())
            self.assertEqual(result['verdict'], 'FAIL')
            self.assertIn('backing mismatch', result.get('error') or '')
            self.assertNotIn('qemu_pid', result)

    def test_dry_run_ignores_boot_lock(self):
        from harness.governor import LockTimeout, run_lock
        try:
            with run_lock(VM, timeout=5):
                completed = subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'run',
                     '--dry-run', '--scenario', 'boot', '--timeout', '30'],
                    cwd=VM, capture_output=True, text=True, timeout=60)
        except LockTimeout:
            self.skipTest('VM lab is busy with another run')
        self.assertEqual(completed.returncode, 0,
                         completed.stdout + completed.stderr)
        self.assertIn('DRY-RUN', completed.stdout)
        self.assertIn('dry-run', completed.stdout)

    def test_dry_run_state_lock_release(self):
        with tempfile.TemporaryDirectory(prefix='vm-dry-state-') as tmp:
            state = Path(tmp) / 'state'

            def launch(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'run',
                     '--state', str(state), '--timeout', '60', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            dry = launch('--dry-run', '--no-lock', '--scenario', 'boot')
            self.assertEqual(dry.returncode, 0, dry.stdout + dry.stderr)
            directory = next(Path(line[5:].strip())
                             for line in dry.stdout.splitlines()
                             if line.startswith('run: '))
            result = json.loads((directory / 'result.json').read_text())
            self.assertEqual(result['verdict'], 'DRY-RUN')
            self.assertIsNone(result.get('qemu_pid'))
            self.assertTrue((state / 'rootfs.qcow2').is_file())

            real = launch('--cmd', 'echo DRY_STATE_OK')
            combined = real.stdout + real.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(real.returncode, 0, combined)
            directory = next(Path(line[5:].strip())
                             for line in real.stdout.splitlines()
                             if line.startswith('run: '))
            self.assertIn('DRY_STATE_OK',
                          (directory / 'guest-output.log').read_text())

    def test_daemon_screenshot_copy_directory(self):
        if os.geteuid() == 0:
            self.skipTest('permission failure is not reliable as root')
        with tempfile.TemporaryDirectory(prefix='vm-daemon-copydir-') as tmp:
            root = Path(tmp)
            daemon_dir = root / 'daemon'
            destination = root / 'readonly-dir'
            destination.mkdir()
            destination.chmod(0o500)

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                failed = cli('screenshot', '--daemon-dir', str(daemon_dir),
                             '--output', str(destination))
                self.assertNotEqual(failed.returncode, 0,
                                    failed.stdout + failed.stderr)
                self.assertIn('cannot copy screenshot',
                              failed.stdout + failed.stderr)
                self.assertEqual(list(daemon_dir.glob('screenshot-*.png')), [])

                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo COPY_DIRECTORY_SAFE')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('COPY_DIRECTORY_SAFE', alive.stdout)
            finally:
                destination.chmod(0o700)
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_command_log_rotation(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-logrotate-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--command-log-max', '256',
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                command = "head -c 400 /dev/zero | tr '\\\\0' X"
                first = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', command)
                self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
                log = daemon_dir / 'daemon-commands.log'
                backup = daemon_dir / 'daemon-commands.log.1'
                self.assertLessEqual(log.stat().st_size, 256)
                self.assertFalse(backup.exists())

                second = cli('exec', '--daemon-dir', str(daemon_dir),
                             '--cmd', command)
                self.assertEqual(second.returncode, 0,
                                 second.stdout + second.stderr)
                self.assertTrue(backup.is_file())
                self.assertLessEqual(log.stat().st_size, 256)
                self.assertLessEqual(backup.stat().st_size, 256)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_state_mode_mismatch_requires_reset(self):
        with tempfile.TemporaryDirectory(prefix='vm-state-mode-') as tmp:
            state = Path(tmp) / 'state'
            process, result = self.run_guest(
                '--state', str(state), '--cmd', 'echo VENDOR_MODE',
                '--timeout', '90')
            self.assertEqual(process.returncode, 0,
                             process.stdout + process.stderr)
            self.assertEqual(result['verdict'], 'PASS')
            self.assertTrue((state / 'state.json').is_file())

            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'run',
                 '--state', str(state), '--kernel', 'compat',
                 '--cmd', 'true', '--timeout', '60', '--lock-timeout', '100'],
                cwd=VM, capture_output=True, text=True, timeout=120)
            combined = completed.stdout + completed.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertNotEqual(completed.returncode, 0, combined)
            self.assertIn('state mode mismatch', combined)
            directory = next(Path(line[5:].strip())
                             for line in completed.stdout.splitlines()
                             if line.startswith('run: '))
            detail = json.loads((directory / 'result.json').read_text())
            self.assertEqual(detail['verdict'], 'FAIL')
            self.assertNotIn('qemu_pid', detail)

    def test_recovery_power_loss_helper(self):
        with tempfile.TemporaryDirectory(prefix='vm-powerloss-') as tmp:
            state = Path(tmp) / 'state'
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'),
                 'recovery-power-loss', '--state', str(state), '--reset',
                 '--timeout', '120'], cwd=VM, capture_output=True, text=True,
                timeout=400)
            combined = completed.stdout + completed.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(completed.returncode, 0, combined[-4000:])
            start = completed.stdout.rfind('\n{\n')
            self.assertNotEqual(start, -1, completed.stdout[-2000:])
            summary = json.loads(completed.stdout[start:])
            self.assertTrue(summary['ok'])
            self.assertEqual(summary['phase1']['verdict'], 'PASS')
            self.assertTrue(summary['phase1']['power_loss']['marker_seen'])
            self.assertTrue(summary['phase1']['power_loss']['killed'])
            self.assertEqual(summary['phase2']['verdict'], 'PASS')
            self.assertTrue(summary['phase2']['recovered_marker'])
            self.assertEqual(summary['state'], str(state))

    def test_compat_audio_card_and_playback(self):
        process, result = self.run_guest(
            '--kernel', 'compat', '--audio', '--timeout', '90',
            '--cmd',
            "grep -q HDA /proc/asound/cards && "
            "aplay -D hw:0,0 -d 1 -f S16_LE -r 48000 -c 2 /dev/zero && "
            "echo AUDIO_OK")
        self.assertEqual(process.returncode, 0,
                         process.stdout + process.stderr)
        self.assertEqual(result['verdict'], 'PASS')
        self.assertTrue(result['audio'])
        output = (Path(result['run_dir']) / 'guest-output.log').read_text()
        self.assertIn('AUDIO_OK', output)
        self.assert_cleaned(result)

    def test_daemon_restart_command(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-restart-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            first = json.loads(started.stdout)
            try:
                restarted = cli('restart', '--daemon-dir', str(daemon_dir))
                self.assertEqual(restarted.returncode, 0,
                                 restarted.stdout + restarted.stderr)
                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(status.returncode, 0, status.stderr)
                values = json.loads(status.stdout)
                self.assertEqual(values['state'], 'ready')
                self.assertNotEqual(values['qemu_pid'], first['qemu_pid'])
                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo RESTART_COMMAND_OK')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('RESTART_COMMAND_OK', alive.stdout)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_capped_output_is_logged_and_rotated(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-caplog-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--exec-output-max', '500', '--command-log-max', '700',
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            command = ("printf 'START_MARKER\\n'; "
                       "yes OUTPUT_LINE | head -c 4000; "
                       "printf '\\nEND_MARKER\\n'")
            try:
                first = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', command)
                self.assertEqual(first.returncode, 0,
                                 first.stdout + first.stderr)
                self.assertIn('END_MARKER', first.stdout)
                self.assertNotIn('START_MARKER', first.stdout)
                self.assertIn('truncated', first.stderr.lower())
                log = daemon_dir / 'daemon-commands.log'
                backup = daemon_dir / 'daemon-commands.log.1'
                self.assertLessEqual(log.stat().st_size, 700)
                self.assertIn('END_MARKER', log.read_text())
                self.assertNotIn('START_MARKER', log.read_text())
                self.assertFalse(backup.exists())

                second = cli('exec', '--daemon-dir', str(daemon_dir),
                             '--cmd', command)
                self.assertEqual(second.returncode, 0,
                                 second.stdout + second.stderr)
                self.assertTrue(backup.is_file())
                self.assertLessEqual(log.stat().st_size, 700)
                self.assertLessEqual(backup.stat().st_size, 700)
                status = cli('status', '--daemon-dir', str(daemon_dir))
                values = json.loads(status.stdout)
                self.assertEqual(values['command_log_bytes'],
                                 log.stat().st_size)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_status_counters_recompute_after_restart(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-counters-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            def status():
                completed = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(completed.returncode, 0, completed.stderr)
                return json.loads(completed.stdout)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                executed = cli('exec', '--daemon-dir', str(daemon_dir),
                               '--cmd', 'echo COUNTER_MARKER')
                self.assertEqual(executed.returncode, 0,
                                 executed.stdout + executed.stderr)
                for _ in range(2):
                    shot = cli('screenshot', '--daemon-dir', str(daemon_dir))
                    self.assertEqual(shot.returncode, 0,
                                     shot.stdout + shot.stderr)
                before = status()
                self.assertGreater(before['command_log_bytes'], 0)
                self.assertEqual(before['screenshot_count'], 2)

                restarted = cli('restart', '--daemon-dir', str(daemon_dir))
                self.assertEqual(restarted.returncode, 0,
                                 restarted.stdout + restarted.stderr)
                after = status()
                self.assertEqual(after['command_log_bytes'], 0)
                self.assertEqual(after['screenshot_count'], 0)
                self.assertGreater(after['serial_bytes'], 0)

                again = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo AFTER_RESTART')
                self.assertEqual(again.returncode, 0,
                                 again.stdout + again.stderr)
                refreshed = status()
                self.assertGreater(refreshed['command_log_bytes'], 0)
                self.assertEqual(refreshed['screenshot_count'], 0)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_restart_preserves_caps(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-restart-caps-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            cpu = sorted(os.sched_getaffinity(0))[0]
            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--exec-output-max', '1000', '--screenshot-keep', '2',
                          '--command-log-max', '900', '--pin-cpu', str(cpu),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                restarted = cli('restart', '--daemon-dir', str(daemon_dir))
                self.assertEqual(restarted.returncode, 0,
                                 restarted.stdout + restarted.stderr)
                config = json.loads((daemon_dir / 'config.json').read_text())
                self.assertEqual(config['exec_output_max'], 1000)
                self.assertEqual(config['screenshot_keep'], 2)
                self.assertEqual(config['command_log_max'], 900)
                self.assertEqual(config['pin_cpu'], cpu)
                status = cli('status', '--daemon-dir', str(daemon_dir))
                values = json.loads(status.stdout)
                self.assertEqual(values['exec_output_max'], 1000)
                self.assertEqual(values['screenshot_keep'], 2)
                self.assertEqual(values['command_log_max'], 900)
                self.assertEqual(values['pin_cpu'], cpu)

                executed = cli(
                    'exec', '--daemon-dir', str(daemon_dir), '--cmd',
                    "printf 'START_MARKER\\n'; "
                    "yes OUTPUT_LINE | head -c 4000; "
                    "printf '\\nEND_MARKER\\n'")
                self.assertEqual(executed.returncode, 0,
                                 executed.stdout + executed.stderr)
                self.assertIn('END_MARKER', executed.stdout)
                self.assertNotIn('START_MARKER', executed.stdout)
                self.assertIn('truncated', executed.stderr.lower())

                captures = []
                for _ in range(3):
                    shot = cli('screenshot', '--daemon-dir', str(daemon_dir))
                    self.assertEqual(shot.returncode, 0,
                                     shot.stdout + shot.stderr)
                    captures.append(Path(json.loads(shot.stdout)['screenshot']['path']))
                self.assertEqual(
                    len(list(daemon_dir.glob('screenshot-*.png'))), 2)
                self.assertFalse(captures[0].exists())
                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(json.loads(status.stdout)['screenshot_count'], 2)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_restart_reset(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-restart-reset-') as tmp:
            root = Path(tmp)
            state = root / 'state'
            daemon_dir = root / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--state', str(state),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                written = cli('exec', '--daemon-dir', str(daemon_dir),
                              '--cmd',
                              "mkdir -p /rw_data && printf 'restart-reset\\n' "
                              "> /rw_data/restart-reset-marker && sync")
                self.assertEqual(written.returncode, 0,
                                 written.stdout + written.stderr)
                reset = cli('restart', '--daemon-dir', str(daemon_dir),
                            '--reset')
                self.assertEqual(reset.returncode, 0,
                                 reset.stdout + reset.stderr)
                read_back = cli('exec', '--daemon-dir', str(daemon_dir),
                                '--cmd',
                                'if [ -e /rw_data/restart-reset-marker ]; '
                                'then echo STILL_THERE; else echo RESET_CLEAN; fi')
                self.assertEqual(read_back.returncode, 0,
                                 read_back.stdout + read_back.stderr)
                self.assertIn('RESET_CLEAN', read_back.stdout)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_restart_reset_without_state_rejected(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-reset-nostate-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                rejected = cli('restart', '--daemon-dir', str(daemon_dir),
                               '--reset')
                self.assertNotEqual(rejected.returncode, 0,
                                    rejected.stdout + rejected.stderr)
                self.assertIn('--reset requires a daemon started with --state',
                              rejected.stdout + rejected.stderr)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_list_reports_live_and_stopped(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-list-') as tmp:
            root = Path(tmp)
            daemon_dir = root / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                listing = cli('list', '--root', str(root), '--json')
                self.assertEqual(listing.returncode, 0, listing.stderr)
                rows = json.loads(listing.stdout)
                self.assertEqual(len(rows), 1)
                self.assertEqual(rows[0]['state'], 'ready')
                self.assertTrue(rows[0]['daemon_alive'])
                self.assertTrue(rows[0]['qemu_alive'])
                self.assertFalse(rows[0]['stale'])
                self.assertEqual(rows[0]['kernel'], 'vendor')
                self.assertEqual(rows[0]['command_log_max'], 1024 * 1024)
                self.assertEqual(rows[0]['screenshot_keep'], 10)
                self.assertEqual(rows[0]['exec_output_max'], 1024 * 1024)
                ready = cli('list', '--root', str(root), '--json',
                            '--state', 'ready')
                self.assertEqual(len(json.loads(ready.stdout)), 1)
                stopped_filter = cli('list', '--root', str(root), '--json',
                                     '--state', 'stopped')
                self.assertEqual(json.loads(stopped_filter.stdout), [])
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)
            listing = cli('list', '--root', str(root), '--json')
            rows = json.loads(listing.stdout)
            self.assertEqual(rows[0]['state'], 'stopped')
            self.assertFalse(rows[0]['qemu_alive'])
            stopped_filter = cli('list', '--root', str(root), '--json',
                                 '--state', 'stopped')
            self.assertEqual(len(json.loads(stopped_filter.stdout)), 1)
            ready_filter = cli('list', '--root', str(root), '--json',
                               '--state', 'ready')
            self.assertEqual(json.loads(ready_filter.stdout), [])

    def test_daemon_restart_edge_cases(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-restart-edge-') as tmp:
            root = Path(tmp)
            daemon_dir = root / 'daemon'
            broken_dir = root / 'broken'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)
                restarted = cli('restart', '--daemon-dir', str(daemon_dir))
                self.assertEqual(restarted.returncode, 0,
                                 restarted.stdout + restarted.stderr)
                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo RESTART_WHILE_STOPPED_OK')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('RESTART_WHILE_STOPPED_OK', alive.stdout)

                config_file = daemon_dir / 'config.json'
                good_config = json.loads(config_file.read_text())
                bad = dict(good_config)
                bad['kernel_target'] = 'bogus'
                config_file.write_text(json.dumps(bad))
                rejected = cli('restart', '--daemon-dir', str(daemon_dir))
                self.assertNotEqual(rejected.returncode, 0,
                                    rejected.stdout + rejected.stderr)
                self.assertIn('unsupported kernel_target',
                              rejected.stdout + rejected.stderr)
                still = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(json.loads(still.stdout)['state'], 'ready')

                allowed = sorted(os.sched_getaffinity(0))
                missing = max(allowed) + 1
                while missing in allowed:
                    missing += 1
                pinned = dict(good_config)
                pinned['pin_cpu'] = missing
                config_file.write_text(json.dumps(pinned))
                pin_rejected = cli('restart', '--daemon-dir', str(daemon_dir))
                self.assertNotEqual(pin_rejected.returncode, 0,
                                    pin_rejected.stdout + pin_rejected.stderr)
                self.assertIn('not available',
                              pin_rejected.stdout + pin_rejected.stderr)
                still = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(json.loads(still.stdout)['state'], 'ready')

                broken_dir.mkdir()
                (broken_dir / 'config.json').write_text('{not json')
                broken = cli('restart', '--daemon-dir', str(broken_dir))
                self.assertNotEqual(broken.returncode, 0,
                                    broken.stdout + broken.stderr)
                self.assertIn('cannot read daemon config',
                              broken.stdout + broken.stderr)
            finally:
                cli('stop', '--daemon-dir', str(daemon_dir))

    def test_daemon_control_protocol_rejects_bad_requests(self):
        import socket
        with tempfile.TemporaryDirectory(prefix='vm-daemon-badctl-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            control = daemon_dir / 'control.sock'
            try:
                def send(payload):
                    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    sock.settimeout(10)
                    sock.connect(str(control))
                    try:
                        sock.sendall(payload)
                        data = b''
                        while not data.endswith(b'\n'):
                            chunk = sock.recv(4096)
                            if not chunk:
                                break
                            data += chunk
                    finally:
                        sock.close()
                    return json.loads(data)

                unknown = send(b'{"action": "bogus"}\n')
                self.assertFalse(unknown['ok'])
                self.assertIn('unknown action', unknown['error'])

                malformed = send(b'{not json}\n')
                self.assertFalse(malformed['ok'])
                self.assertIn('error', malformed)

                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo BAD_REQUEST_SAFE')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('BAD_REQUEST_SAFE', alive.stdout)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_qmp_right_click(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-rightclick-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            started = cli('start', '--kernel', 'compat',
                          '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            qmp = daemon_dir / 'qmp.sock'
            try:
                clicked = subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'input',
                     '--qmp', str(qmp), '--click', '200,150',
                     '--width', '1280', '--height', '720', '--button', 'right'],
                    cwd=VM, capture_output=True, text=True, timeout=60)
                self.assertEqual(clicked.returncode, 0,
                                 clicked.stdout + clicked.stderr)
                value = json.loads(clicked.stdout)
                self.assertTrue(value['ok'])
                self.assertEqual(value['button'], 'right')
                self.assertEqual((value['x'], value['y']), (200, 150))
                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(status.returncode, 0, status.stderr)
                self.assertTrue(json.loads(status.stdout)['qemu_alive'])
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_exec_output_cap(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-outcap-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--exec-output-max', '1000',
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                executed = cli(
                    'exec', '--daemon-dir', str(daemon_dir), '--cmd',
                    "printf 'START_MARKER\\n'; "
                    "yes OUTPUT_LINE | head -c 4000; "
                    "printf '\\nEND_MARKER\\n'")
                self.assertEqual(executed.returncode, 0,
                                 executed.stdout + executed.stderr)
                self.assertIn('END_MARKER', executed.stdout)
                self.assertNotIn('START_MARKER', executed.stdout)
                self.assertLessEqual(len(executed.stdout.encode()), 1100)
                self.assertIn('truncated', executed.stderr.lower())
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_command_log_corruption(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-badlog-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                log_path = daemon_dir / 'daemon-commands.log'
                log_path.mkdir()
                executed = cli('exec', '--daemon-dir', str(daemon_dir),
                               '--cmd', 'echo LOG_CORRUPTION_SAFE')
                self.assertEqual(executed.returncode, 0,
                                 executed.stdout + executed.stderr)
                self.assertIn('LOG_CORRUPTION_SAFE', executed.stdout)
                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(status.returncode, 0, status.stderr)
                values = json.loads(status.stdout)
                self.assertEqual(values['state'], 'ready')
                self.assertTrue(values['qemu_alive'])
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_missing_status_recovery(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-nostatus-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                (daemon_dir / 'status.json').unlink()
                observed = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertNotEqual(observed.returncode, 0)
                values = json.loads(observed.stdout)
                self.assertTrue(values.get('reachable'))
                self.assertTrue(values.get('qemu_alive'))

                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo MISSING_STATUS_SAFE')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('MISSING_STATUS_SAFE', alive.stdout)

                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)
                after = json.loads((daemon_dir / 'status.json').read_text())
                self.assertEqual(after['state'], 'stopped')

                restarted = cli('start', '--daemon-dir', str(daemon_dir),
                                '--ready-timeout', '120', '--lock-timeout', '120')
                self.assertEqual(restarted.returncode, 0,
                                 restarted.stdout + restarted.stderr)
                again = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo RESTARTED_FROM_MISSING_STATUS')
                self.assertEqual(again.returncode, 0,
                                 again.stdout + again.stderr)
                self.assertIn('RESTARTED_FROM_MISSING_STATUS', again.stdout)
            finally:
                cli('stop', '--daemon-dir', str(daemon_dir))

    def test_daemon_config_missing_recovery(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-noconfig-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                (daemon_dir / 'config.json').unlink()
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)
                values = json.loads(stopped.stdout)
                self.assertEqual(values['state'], 'stopped')

                restarted = cli('start', '--daemon-dir', str(daemon_dir),
                                '--ready-timeout', '120', '--lock-timeout', '120')
                self.assertEqual(restarted.returncode, 0,
                                 restarted.stdout + restarted.stderr)
                self.assertTrue((daemon_dir / 'config.json').is_file())
                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo CONFIG_MISSING_RECOVERED')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('CONFIG_MISSING_RECOVERED', alive.stdout)
            finally:
                cli('stop', '--daemon-dir', str(daemon_dir))

    def test_daemon_console_missing_serial(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-noserial-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                (daemon_dir / 'serial.log').unlink()
                console = cli('console', '--daemon-dir', str(daemon_dir),
                              '--lines', '20')
                self.assertNotEqual(console.returncode, 0,
                                    console.stdout + console.stderr)
                self.assertIn('serial log missing',
                              console.stdout + console.stderr)

                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo NO_SERIAL_CONSOLE_SAFE')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('NO_SERIAL_CONSOLE_SAFE', alive.stdout)
                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(status.returncode, 0, status.stderr)
                values = json.loads(status.stdout)
                self.assertEqual(values['state'], 'ready')
                self.assertTrue(values['qemu_alive'])
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_qmp_input_invalid(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-input-bad-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            def input_cmd(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'input', *args],
                    cwd=VM, capture_output=True, text=True, timeout=60)

            started = cli('start', '--kernel', 'compat',
                          '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            qmp = daemon_dir / 'qmp.sock'
            try:
                click = input_cmd('--qmp', str(qmp), '--click', '2000,100',
                                  '--width', '1280', '--height', '720')
                self.assertNotEqual(click.returncode, 0,
                                    click.stdout + click.stderr)
                self.assertIn('click outside', click.stderr + click.stdout)

                key = input_cmd('--qmp', str(qmp), '--key', 'KEY_A')
                self.assertNotEqual(key.returncode, 0,
                                    key.stdout + key.stderr)
                self.assertIn('input-send-event', key.stderr + key.stdout)

                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(status.returncode, 0, status.stderr)
                values = json.loads(status.stdout)
                self.assertEqual(values['state'], 'ready')
                self.assertTrue(values['qemu_alive'])
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_concurrent_exec(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-concurrent-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                slow = subprocess.Popen(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', 'exec',
                     '--daemon-dir', str(daemon_dir), '--cmd',
                     'sleep 1; echo CONCURRENT_SLOW', '--timeout', '30'],
                    cwd=VM, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True)
                fast = subprocess.Popen(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', 'exec',
                     '--daemon-dir', str(daemon_dir), '--cmd',
                     'echo CONCURRENT_FAST', '--timeout', '30'],
                    cwd=VM, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True)
                slow_out, _ = slow.communicate(timeout=40)
                fast_out, _ = fast.communicate(timeout=40)
                self.assertEqual(slow.returncode, 0, slow_out)
                self.assertEqual(fast.returncode, 0, fast_out)
                self.assertIn('CONCURRENT_SLOW', slow_out)
                self.assertNotIn('CONCURRENT_FAST', slow_out)
                self.assertIn('CONCURRENT_FAST', fast_out)
                self.assertNotIn('CONCURRENT_SLOW', fast_out)

                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(status.returncode, 0, status.stderr)
                values = json.loads(status.stdout)
                self.assertEqual(values['state'], 'ready')
                self.assertTrue(values['qemu_alive'])
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_stop_during_inflight_exec(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-inflight-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            first = json.loads(started.stdout)
            qemu_pid = first['qemu_pid']
            inflight = subprocess.Popen(
                [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', 'exec',
                 '--daemon-dir', str(daemon_dir), '--cmd',
                 'sleep 20; echo TOO_LATE', '--timeout', '60'],
                cwd=VM, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            try:
                time.sleep(1)
                began = time.monotonic()
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                elapsed = time.monotonic() - began
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)
                self.assertLess(elapsed, 15, 'stop blocked on the in-flight exec')
                output, _ = inflight.communicate(timeout=20)
                self.assertNotEqual(inflight.returncode, 0, output)
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline and Path(f'/proc/{qemu_pid}').exists():
                    time.sleep(0.1)
                self.assertFalse(Path(f'/proc/{qemu_pid}').exists())
                values = json.loads(stopped.stdout)
                self.assertEqual(values['state'], 'stopped')
            finally:
                if inflight.poll() is None:
                    inflight.kill()
                    inflight.communicate(timeout=20)
                cli('stop', '--daemon-dir', str(daemon_dir))

    def test_daemon_state_lock_contention(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-lock-') as tmp:
            root = Path(tmp)
            state = root / 'state'
            state.mkdir()
            failed_dir = root / 'failed-daemon'
            clean_dir = root / 'clean-daemon'
            lock_path = state / 'state.lock'
            holder = open(lock_path, 'w')
            fcntl.flock(holder.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            try:
                failed = cli('start', '--daemon-dir', str(failed_dir),
                             '--state', str(state),
                             '--ready-timeout', '120', '--lock-timeout', '120')
                combined = failed.stdout + failed.stderr
                self.assertNotEqual(failed.returncode, 0, combined)
                self.assertIn('state is already in use', combined)
                status = json.loads((failed_dir / 'status.json').read_text())
                self.assertEqual(status['state'], 'error')
                self.assertNotIn('qemu_pid', status)

                fcntl.flock(holder.fileno(), fcntl.LOCK_UN)
                holder.close()

                started = cli('start', '--daemon-dir', str(clean_dir),
                              '--state', str(state),
                              '--ready-timeout', '120', '--lock-timeout', '120')
                combined = started.stdout + started.stderr
                if 'another VM boot holds' in combined or 'lock timeout' in combined:
                    self.skipTest('VM lab is busy with another run')
                self.assertEqual(started.returncode, 0, combined)
                alive = cli('exec', '--daemon-dir', str(clean_dir),
                            '--cmd', 'echo STATE_LOCK_RECOVERED')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('STATE_LOCK_RECOVERED', alive.stdout)
                stopped = cli('stop', '--daemon-dir', str(clean_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)
            finally:
                try:
                    fcntl.flock(holder.fileno(), fcntl.LOCK_UN)
                except (OSError, ValueError):
                    pass
                holder.close()

    def test_daemon_state_reset(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-reset-') as tmp:
            root = Path(tmp)
            state = root / 'state'
            daemon_dir = root / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            def start(reset=False):
                argv = ['start', '--daemon-dir', str(daemon_dir),
                        '--state', str(state),
                        '--ready-timeout', '120', '--lock-timeout', '120']
                if reset:
                    argv.append('--reset')
                completed = cli(*argv)
                combined = completed.stdout + completed.stderr
                if 'another VM boot holds' in combined or 'lock timeout' in combined:
                    self.skipTest('VM lab is busy with another run')
                self.assertEqual(completed.returncode, 0, combined)

            try:
                start()
                written = cli('exec', '--daemon-dir', str(daemon_dir),
                              '--cmd',
                              "mkdir -p /rw_data && printf 'reset-me\\n' "
                              "> /rw_data/reset-marker && sync && echo MARKER_WRITTEN")
                self.assertEqual(written.returncode, 0,
                                 written.stdout + written.stderr)
                self.assertIn('MARKER_WRITTEN', written.stdout)
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

                start(reset=True)
                read_back = cli('exec', '--daemon-dir', str(daemon_dir),
                                '--cmd',
                                'if [ -e /rw_data/reset-marker ]; then '
                                'echo STILL_THERE; else echo RESET_CLEAN; fi')
                self.assertEqual(read_back.returncode, 0,
                                 read_back.stdout + read_back.stderr)
                self.assertIn('RESET_CLEAN', read_back.stdout)
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)
            finally:
                cli('stop', '--daemon-dir', str(daemon_dir))

    def test_daemon_exec_timeout_recovery(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-timeout-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                timed_out = cli('exec', '--daemon-dir', str(daemon_dir),
                                '--cmd', 'sleep 3; echo TOO_LATE', '--timeout', '1')
                self.assertNotEqual(timed_out.returncode, 0,
                                    timed_out.stdout + timed_out.stderr)
                self.assertIn('timed out', (timed_out.stderr + timed_out.stdout).lower())
                self.assertNotIn('usage:', timed_out.stderr)

                recovered = cli('exec', '--daemon-dir', str(daemon_dir),
                                '--cmd', 'echo TIMEOUT_RECOVERED', '--timeout', '15')
                self.assertEqual(recovered.returncode, 0,
                                 recovered.stdout + recovered.stderr)
                self.assertIn('TIMEOUT_RECOVERED', recovered.stdout)

                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(status.returncode, 0, status.stderr)
                values = json.loads(status.stdout)
                self.assertEqual(values['state'], 'ready')
                self.assertTrue(values['reachable'])
                self.assertTrue(values['qemu_alive'])
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_state_survives_sigkill(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-state-kill-') as tmp:
            root = Path(tmp)
            state = root / 'state'
            daemon_dir = root / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--state', str(state),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            first = json.loads(started.stdout)
            daemon_pid = first['daemon_pid']
            qemu_pid = first['qemu_pid']
            try:
                written = cli('exec', '--daemon-dir', str(daemon_dir),
                              '--cmd',
                              "mkdir -p /rw_data && printf 'state-survives-sigkill\\n' "
                              "> /rw_data/sigkill-marker && sync && echo MARKER_WRITTEN")
                self.assertEqual(written.returncode, 0,
                                 written.stdout + written.stderr)
                self.assertIn('MARKER_WRITTEN', written.stdout)

                os.kill(daemon_pid, signal.SIGKILL)
                for pid in (daemon_pid, qemu_pid):
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline and Path(f'/proc/{pid}').exists():
                        time.sleep(0.1)
                    self.assertFalse(Path(f'/proc/{pid}').exists())

                restarted = cli('start', '--daemon-dir', str(daemon_dir),
                                '--state', str(state),
                                '--ready-timeout', '120', '--lock-timeout', '120')
                self.assertEqual(restarted.returncode, 0,
                                 restarted.stdout + restarted.stderr)
                read_back = cli('exec', '--daemon-dir', str(daemon_dir),
                                '--cmd', 'cat /rw_data/sigkill-marker')
                self.assertEqual(read_back.returncode, 0,
                                 read_back.stdout + read_back.stderr)
                self.assertIn('state-survives-sigkill', read_back.stdout)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_sigkill_recovery(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-sigkill-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            first = json.loads(started.stdout)
            daemon_pid = first['daemon_pid']
            qemu_pid = first['qemu_pid']
            try:
                os.kill(daemon_pid, signal.SIGKILL)
                for pid in (daemon_pid, qemu_pid):
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline and Path(f'/proc/{pid}').exists():
                        time.sleep(0.1)
                    self.assertFalse(Path(f'/proc/{pid}').exists(),
                                     f'pid {pid} survived daemon SIGKILL')

                status = cli('status', '--daemon-dir', str(daemon_dir))
                values = json.loads(status.stdout)
                self.assertFalse(values['reachable'])

                restarted = cli('start', '--daemon-dir', str(daemon_dir),
                                '--ready-timeout', '120', '--lock-timeout', '120')
                self.assertEqual(restarted.returncode, 0,
                                 restarted.stdout + restarted.stderr)
                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo RESTARTED_AFTER_SIGKILL')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('RESTARTED_AFTER_SIGKILL', alive.stdout)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_stale_control_socket_recovery(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-stale-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            first = json.loads(started.stdout)
            daemon_pid = first['daemon_pid']
            qemu_pid = first['qemu_pid']
            try:
                control = daemon_dir / 'control.sock'
                self.assertTrue(control.exists())
                control.unlink()

                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(status.returncode, 0, status.stderr)
                values = json.loads(status.stdout)
                self.assertEqual(values['state'], 'ready')
                self.assertFalse(values['reachable'])
                self.assertIn('control socket', values['reachable_error'])

                failed = cli('exec', '--daemon-dir', str(daemon_dir),
                             '--cmd', 'echo SHOULD_NOT_RUN')
                self.assertNotEqual(failed.returncode, 0)
                self.assertIn('control socket', failed.stderr + failed.stdout)

                os.kill(daemon_pid, signal.SIGTERM)
                for pid in (daemon_pid, qemu_pid):
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline and Path(f'/proc/{pid}').exists():
                        time.sleep(0.1)
                    self.assertFalse(Path(f'/proc/{pid}').exists())

                restarted = cli('start', '--daemon-dir', str(daemon_dir),
                                '--ready-timeout', '120', '--lock-timeout', '120')
                self.assertEqual(restarted.returncode, 0,
                                 restarted.stdout + restarted.stderr)
                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo RESTARTED_AFTER_STALE_CONTROL')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('RESTARTED_AFTER_STALE_CONTROL', alive.stdout)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_dead_qemu_recovery(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-dead-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            first = json.loads(started.stdout)
            qemu_pid = first['qemu_pid']
            daemon_pid = first['daemon_pid']
            try:
                os.kill(qemu_pid, signal.SIGKILL)
                deadline = time.monotonic() + 15
                detected = False
                while time.monotonic() < deadline:
                    status = cli('status', '--daemon-dir', str(daemon_dir))
                    try:
                        values = json.loads(status.stdout)
                    except json.JSONDecodeError:
                        values = {}
                    if (values.get('state') in ('stopped', 'error')
                            or values.get('qemu_alive') is False):
                        detected = True
                        break
                    time.sleep(0.2)
                self.assertTrue(detected, 'daemon never reported its dead QEMU')
                for pid in (qemu_pid, daemon_pid):
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline and Path(f'/proc/{pid}').exists():
                        time.sleep(0.1)
                    self.assertFalse(Path(f'/proc/{pid}').exists())

                restarted = cli('start', '--daemon-dir', str(daemon_dir),
                                '--ready-timeout', '120', '--lock-timeout', '120')
                self.assertEqual(restarted.returncode, 0,
                                 restarted.stdout + restarted.stderr)
                alive = cli('exec', '--daemon-dir', str(daemon_dir),
                            '--cmd', 'echo RESTARTED_AFTER_DEAD_QEMU')
                self.assertEqual(alive.returncode, 0,
                                 alive.stdout + alive.stderr)
                self.assertIn('RESTARTED_AFTER_DEAD_QEMU', alive.stdout)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_screenshot_retention(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-retention-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--screenshot-keep', '2',
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            captures = []
            try:
                for _ in range(3):
                    captured = cli('screenshot', '--daemon-dir', str(daemon_dir))
                    self.assertEqual(captured.returncode, 0,
                                     captured.stdout + captured.stderr)
                    response = json.loads(captured.stdout)
                    path = Path(response['screenshot']['path'])
                    captures.append(path)
                    self.assertLessEqual(
                        len(list(daemon_dir.glob('screenshot-*.png'))), 2)
                self.assertFalse(captures[0].exists())
                self.assertTrue(captures[1].exists())
                self.assertTrue(captures[2].exists())
                self.assertEqual(
                    len(list(daemon_dir.glob('screenshot-*.png'))), 2)
                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(status.returncode, 0, status.stderr)
                self.assertEqual(json.loads(status.stdout)['screenshot_count'], 2)
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_daemon_screenshot_failure_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-shot-fail-') as tmp:
            root = Path(tmp)
            daemon_dir = root / 'daemon'
            output = root / 'failed-shot.png'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            qemu_pid = None
            try:
                status = cli('status', '--daemon-dir', str(daemon_dir))
                values = json.loads(status.stdout)
                qemu_pid = values['qemu_pid']
                qmp = daemon_dir / 'qmp.sock'
                self.assertTrue(qmp.exists())
                qmp.unlink()

                captured = cli('screenshot', '--daemon-dir', str(daemon_dir),
                               '--output', str(output))
                self.assertNotEqual(captured.returncode, 0,
                                    captured.stdout + captured.stderr)
                self.assertIn('QMP', captured.stderr + captured.stdout)
                self.assertFalse(output.exists())
                self.assertEqual(list(daemon_dir.glob('screenshot-*.png')), [])

                after = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(after.returncode, 0, after.stderr)
                current = json.loads(after.stdout)
                self.assertEqual(current['state'], 'ready')
                self.assertTrue(current['reachable'])
                self.assertTrue(current['qemu_alive'])
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)
            if qemu_pid:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and Path(f'/proc/{qemu_pid}').exists():
                    time.sleep(0.1)
                self.assertFalse(Path(f'/proc/{qemu_pid}').exists())

    def test_daemon_screenshot_artifact(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-shot-') as tmp:
            root = Path(tmp)
            daemon_dir = root / 'daemon'
            output = root / 'daemon-shot.png'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=180)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                captured = cli('screenshot', '--daemon-dir', str(daemon_dir),
                               '--output', str(output))
                self.assertEqual(captured.returncode, 0,
                                 captured.stdout + captured.stderr)
                response = json.loads(captured.stdout)
                self.assertTrue(response['ok'])
                meta = response['screenshot']
                self.assertGreater(meta['width'], 0)
                self.assertGreater(meta['height'], 0)
                self.assertGreater(meta['size'], 0)
                self.assertEqual(Path(response['copied_to']), output)
                data = output.read_bytes()
                self.assertEqual(data[:8], b'\x89PNG\r\n\x1a\n')
                width, height = struct.unpack('>II', data[16:24])
                self.assertEqual((width, height), (meta['width'], meta['height']))
                self.assertEqual(len(data), meta['size'])
                self.assertFalse(Path(meta['path']).exists())
                self.assertEqual(list(daemon_dir.glob('screenshot-*.png')), [])
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_qmp_screendump_failure_diagnostics(self):
        process = subprocess.Popen(
            [sys.executable, '-B', str(VM / 'launch.py'), 'run',
             '--kernel', 'compat', '--scenario', 'ui-smoke',
             '--qmp-screenshot', '--timeout', '180', '--lock-timeout', '100'],
            cwd=VM, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        run_dir = None
        deadline = time.monotonic() + 60
        qmp = None
        try:
            while time.monotonic() < deadline:
                line = process.stdout.readline()
                if not line:
                    break
                if line.startswith('run: '):
                    run_dir = Path(line[5:].strip())
                    break
            self.assertIsNotNone(run_dir, 'launcher never reported a run directory')
            qmp = run_dir / 'qmp.sock'
            while time.monotonic() < deadline and process.poll() is None:
                if qmp.exists():
                    qmp.unlink()
                    break
                time.sleep(0.05)
            self.assertTrue(qmp is not None, 'qmp socket path missing')
            output, _ = process.communicate(timeout=240)
            self.assertNotEqual(process.returncode, 0,
                                output[-4000:])
            result = json.loads((run_dir / 'result.json').read_text())
            self.assertEqual(result['verdict'], 'FAIL')
            self.assertIsNone(result['screenshot'].get('qmp'))
            self.assertIn('QMP', result['screenshot'].get('qmp_error') or '')
            self.assertTrue(any('visible frame required' in item
                                for item in result.get('failures', [])))
            self.assert_cleaned(result)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=30)

    def test_benchmark_compat_mode(self):
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'bench.py'),
             '--scenario', 'boot', '--repeats', '1', '--kernel', 'compat',
             '--timeout', '120', '--lock-timeout', '100'],
            cwd=VM, capture_output=True, text=True, timeout=240)
        combined = completed.stdout + completed.stderr
        if 'another VM boot holds' in combined or 'lock timeout' in combined:
            self.skipTest('VM lab is busy with another run')
        self.assertEqual(completed.returncode, 0, combined)
        path = next(Path(line[7:]) for line in completed.stdout.splitlines()
                    if line.startswith('bench: '))
        try:
            from harness.result_schema import validate_bench
            data = json.loads(path.read_text())
            self.assertEqual(validate_bench(data), [])
            self.assertEqual(len(data['rows']), 1)
            self.assertEqual(data['rows'][0]['verdict'], 'PASS')
            key = 'boot cache=default pin=none'
            self.assertIn(key, data['summary'])
            self.assertIn(key, data['metrics'])
        finally:
            shutil.rmtree(path.parent, ignore_errors=True)

    def test_benchmark_matrix_warmup(self):
        cpu = sorted(os.sched_getaffinity(0))[0]
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'bench.py'),
             '--scenario', 'boot', '--repeats', '1', '--warmup', '1',
             '--matrix', '--matrix-cache', 'writeback',
             '--matrix-cpu', str(cpu), '--timeout', '120',
             '--lock-timeout', '100'],
            cwd=VM, capture_output=True, text=True, timeout=300)
        combined = completed.stdout + completed.stderr
        if 'another VM boot holds' in combined or 'lock timeout' in combined:
            self.skipTest('VM lab is busy with another run')
        self.assertEqual(completed.returncode, 0, combined)
        path = next(Path(line[7:]) for line in completed.stdout.splitlines()
                    if line.startswith('bench: '))
        try:
            from harness.result_schema import validate_bench
            data = json.loads(path.read_text())
            self.assertEqual(validate_bench(data), [])
            expected = {'writeback': [None, cpu]}
            variants = {(item['cache'], item['pin_cpu'])
                        for item in data['variants']}
            self.assertEqual(variants, {(cache, pin) for cache in expected
                                        for pin in expected[cache]})
            self.assertEqual(len(data['warmups']), 2)
            self.assertEqual(len(data['rows']), 2)
            self.assertTrue(all(row['verdict'] == 'PASS'
                                for row in data['warmups'] + data['rows']))
            self.assertEqual(len(data['summary']), 2)
            self.assertEqual(len(data['metrics']), 2)
        finally:
            shutil.rmtree(path.parent, ignore_errors=True)

    def test_benchmark_warmup_and_provenance(self):
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'bench.py'),
             '--scenario', 'boot', '--repeats', '1', '--warmup', '1',
             '--timeout', '120', '--lock-timeout', '100'],
            cwd=VM, capture_output=True, text=True, timeout=240)
        combined = completed.stdout + completed.stderr
        if 'another VM boot holds' in combined or 'lock timeout' in combined:
            self.skipTest('VM lab is busy with another run')
        self.assertEqual(completed.returncode, 0, combined)
        path = next(Path(line[7:]) for line in completed.stdout.splitlines()
                    if line.startswith('bench: '))
        try:
            from harness.result_schema import validate_bench
            data = json.loads(path.read_text())
            self.assertEqual(validate_bench(data), [])
            self.assertEqual(data['warmup'], 1)
            self.assertEqual(len(data['warmups']), 1)
            self.assertEqual(data['warmups'][0]['verdict'], 'PASS')
            self.assertEqual(len(data['rows']), 1)
            self.assertEqual(data['rows'][0]['verdict'], 'PASS')
            self.assertTrue(data['source']['git_head'])
            self.assertIsInstance(data['source']['dirty'], bool)
            key = 'boot cache=default pin=none'
            self.assertIn(key, data['metrics'])
            self.assertEqual(
                set(data['metrics'][key]),
                {'boot_seconds', 'readiness_seconds', 'scenario_seconds'})
        finally:
            shutil.rmtree(path.parent, ignore_errors=True)

    def test_benchmark_matrix_end_to_end(self):
        cpu = sorted(os.sched_getaffinity(0))[0]
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'bench.py'),
             '--scenario', 'boot', '--repeats', '1',
             '--matrix', '--matrix-cache', 'writeback',
             '--matrix-cache', 'writethrough', '--matrix-cpu', str(cpu),
             '--timeout', '120', '--lock-timeout', '100'],
            cwd=VM, capture_output=True, text=True, timeout=300)
        combined = completed.stdout + completed.stderr
        if 'another VM boot holds' in combined or 'lock timeout' in combined:
            self.skipTest('VM lab is busy with another run')
        self.assertEqual(completed.returncode, 0, combined)
        path = next(Path(line[7:]) for line in completed.stdout.splitlines()
                    if line.startswith('bench: '))
        try:
            from harness.result_schema import validate_bench
            data = json.loads(path.read_text())
            self.assertEqual(validate_bench(data), [])
            expected = {(cache, pin) for cache in ('writeback', 'writethrough')
                        for pin in (None, cpu)}
            variants = {(item['cache'], item['pin_cpu'])
                        for item in data['variants']}
            self.assertEqual(variants, expected)
            self.assertEqual(len(data['rows']), 4)
            self.assertEqual(len(data['summary']), 4)
            self.assertEqual(len(data['metrics']), 4)
            for row in data['rows']:
                self.assertEqual(row['verdict'], 'PASS')
                self.assertIn((row['cache'], row['pin_cpu']), expected)
            for cache, pin in expected:
                key = (f"boot cache={cache} "
                       f"pin={pin if pin is not None else 'none'}")
                self.assertIn(key, data['summary'])
                self.assertIn(key, data['metrics'])
                self.assertEqual(
                    set(data['metrics'][key]),
                    {'boot_seconds', 'readiness_seconds', 'scenario_seconds'})
        finally:
            shutil.rmtree(path.parent, ignore_errors=True)

    def test_daemon_console_and_status_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-diag-') as tmp:
            daemon_dir = Path(tmp) / 'daemon'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            started = cli('start', '--daemon-dir', str(daemon_dir),
                          '--ready-timeout', '120', '--lock-timeout', '120')
            combined = started.stdout + started.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(started.returncode, 0, combined)
            try:
                executed = cli('exec', '--daemon-dir', str(daemon_dir),
                               '--cmd', 'echo DIAG_MARKER; exit 3')
                self.assertEqual(executed.returncode, 3,
                                 executed.stdout + executed.stderr)
                self.assertIn('DIAG_MARKER', executed.stdout)
                command_log = (daemon_dir / 'daemon-commands.log').read_text()
                self.assertIn('rc=3', command_log)
                self.assertIn('DIAG_MARKER', command_log)

                console = cli('console', '--daemon-dir', str(daemon_dir),
                              '--lines', '200')
                self.assertEqual(console.returncode, 0, console.stderr)
                self.assertIn('DIAG_MARKER', console.stdout)

                status = cli('status', '--daemon-dir', str(daemon_dir))
                self.assertEqual(status.returncode, 0, status.stderr)
                values = json.loads(status.stdout)
                self.assertEqual(values['state'], 'ready')
                self.assertTrue(values['reachable'])
                self.assertTrue(values['qemu_alive'])
                self.assertGreater(values['serial_bytes'], 0)
                self.assertGreater(values['command_log_bytes'], 0)
                self.assertEqual(values['screenshot_count'], 0)
                self.assertTrue(values['lock']['held'])
            finally:
                stopped = cli('stop', '--daemon-dir', str(daemon_dir))
                self.assertEqual(stopped.returncode, 0,
                                 stopped.stdout + stopped.stderr)

    def test_benchmark_comparison_end_to_end(self):
        def bench():
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'bench.py'),
                 '--scenario', 'boot', '--repeats', '1',
                 '--timeout', '120', '--lock-timeout', '100'],
                cwd=VM, capture_output=True, text=True, timeout=240)
            combined = completed.stdout + completed.stderr
            if 'another VM boot holds' in combined or 'lock timeout' in combined:
                self.skipTest('VM lab is busy with another run')
            self.assertEqual(completed.returncode, 0, combined)
            path = next(Path(line[7:]) for line in completed.stdout.splitlines()
                        if line.startswith('bench: '))
            self.assertTrue(path.is_file())
            return path

        def compare(left, right, threshold, *extra):
            completed = subprocess.run(
                [sys.executable, '-B', str(VM / 'bench.py'), '--compare',
                 str(left), str(right), '--threshold', str(threshold), *extra],
                cwd=VM, capture_output=True, text=True, timeout=60)
            self.assertIn(completed.returncode, (0, 1), completed.stderr)
            return completed.returncode, json.loads(completed.stdout)

        first = bench()
        second = bench()
        mutated = second.with_name('mutated-bench.json')
        source_bad = second.with_name('source-mismatch-bench.json')
        try:
            rc, report = compare(first, first, 0.0)
            self.assertEqual(rc, 0)
            self.assertTrue(report['ok'])
            self.assertTrue(all(row['status'] == 'SAME' for row in report['rows']))

            rc, report = compare(first, second, 10.0)
            self.assertEqual(rc, 0)
            self.assertTrue(report['ok'])
            self.assertTrue(report['source_match'])

            source_data = json.loads(second.read_text())
            source_data['source']['git_head'] = '0' * 40
            source_bad.write_text(json.dumps(source_data, indent=2) + '\n')
            rc, report = compare(first, source_bad, 10.0)
            self.assertEqual(rc, 0)
            self.assertFalse(report['source_match'])
            rc, report = compare(first, source_bad, 10.0,
                                 '--require-source-match')
            self.assertEqual(rc, 1)
            self.assertFalse(report['ok'])

            data = json.loads(second.read_text())
            for summary in data['summary'].values():
                summary['mean'] = round(summary['mean'] * 2, 3)
            for metrics in (data.get('metrics') or {}).values():
                for summary in metrics.values():
                    summary['mean'] = round(summary['mean'] * 2, 3)
            mutated.write_text(json.dumps(data, indent=2) + '\n')
            rc, report = compare(first, mutated, 0.05)
            self.assertEqual(rc, 1)
            self.assertFalse(report['ok'])
            self.assertTrue(any(row['status'] == 'REGRESSED'
                                for row in report['rows']))
        finally:
            mutated.unlink(missing_ok=True)
            source_bad.unlink(missing_ok=True)
            for path in (first, second):
                shutil.rmtree(path.parent, ignore_errors=True)

    def test_qmp_screenshot_visual_regression(self):
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow required for visual regression')
        name = f'ui-smoke-test-{os.getpid()}'
        baselines = VM / 'baselines'
        baseline_png = baselines / f'{name}.png'
        baseline_json = baselines / f'{name}.png.json'
        diff_png = baselines / f'{name}.png.diff.png'
        temp_id = f'visual-diff-{os.getpid()}'
        temp_dir = VM / 'runs' / temp_id
        process, result = self.run_guest(
            '--kernel', 'compat', '--scenario', 'ui-smoke',
            '--qmp-screenshot', '--timeout', '180')
        self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
        self.assertEqual(result['verdict'], 'PASS')
        shot = Path(result['screenshot']['qmp']['path'])
        self.assertTrue(shot.is_file())
        self.assertTrue((result['screenshot'].get('qmp_pixels') or {}).get('bbox'))
        run_id = Path(result['run_dir']).name

        def visual(*args):
            return subprocess.run(
                [sys.executable, '-B', str(VM / 'launch.py'), 'visual', *args],
                cwd=VM, capture_output=True, text=True, timeout=60)

        try:
            created = visual('baseline', run_id, 'qmp-screenshot.png', name)
            self.assertEqual(created.returncode, 0, created.stdout + created.stderr)
            same = visual('compare', run_id, 'qmp-screenshot.png', name,
                          '--threshold', '0.0')
            self.assertEqual(same.returncode, 0, same.stdout + same.stderr)
            same_status = json.loads(same.stdout)
            self.assertTrue(same_status['ok'])
            self.assertEqual(same_status['changed_pixels'], 0)

            temp_dir.mkdir(parents=True, exist_ok=True)
            with Image.open(shot) as image:
                changed = image.convert('RGB')
            changed.putpixel((0, 0), (255, 0, 0))
            changed.save(temp_dir / 'qmp-screenshot.png')
            different = visual('compare', temp_id, 'qmp-screenshot.png', name,
                               '--threshold', '0.0')
            self.assertEqual(different.returncode, 1,
                             different.stdout + different.stderr)
            changed_status = json.loads(different.stdout)
            self.assertFalse(changed_status['ok'])
            self.assertGreater(changed_status['changed_pixels'], 0)
            self.assertTrue(Path(changed_status['diff']).is_file())
        finally:
            for path in (baseline_png, baseline_json, diff_png):
                path.unlink(missing_ok=True)
            shutil.rmtree(temp_dir, ignore_errors=True)

    def test_daemon_state_survives_restart(self):
        with tempfile.TemporaryDirectory(prefix='vm-daemon-state-') as tmp:
            root = Path(tmp)
            state = root / 'state'
            daemon_one = root / 'daemon-1'
            daemon_two = root / 'daemon-2'

            def cli(*args):
                return subprocess.run(
                    [sys.executable, '-B', str(VM / 'launch.py'), 'daemon', *args],
                    cwd=VM, capture_output=True, text=True, timeout=240)

            def start(directory):
                completed = cli('start', '--kernel', 'vendor',
                                '--state', str(state),
                                '--daemon-dir', str(directory),
                                '--ready-timeout', '120', '--lock-timeout', '120')
                combined = completed.stdout + completed.stderr
                if 'another VM boot holds' in combined or 'lock timeout' in combined:
                    self.skipTest('VM lab is busy with another run')
                self.assertEqual(completed.returncode, 0, combined)

            try:
                start(daemon_one)
                written = cli('exec', '--daemon-dir', str(daemon_one),
                              '--cmd',
                              "mkdir -p /rw_data && printf 'daemon-state-survives\\n' "
                              "> /rw_data/daemon-marker && sync && echo MARKER_WRITTEN")
                self.assertEqual(written.returncode, 0, written.stderr)
                self.assertIn('MARKER_WRITTEN', written.stdout)
                stopped = cli('stop', '--daemon-dir', str(daemon_one))
                self.assertEqual(stopped.returncode, 0, stopped.stdout + stopped.stderr)

                start(daemon_two)
                read_back = cli('exec', '--daemon-dir', str(daemon_two),
                                '--cmd', 'cat /rw_data/daemon-marker')
                self.assertEqual(read_back.returncode, 0, read_back.stderr)
                self.assertIn('daemon-state-survives', read_back.stdout)
                stopped = cli('stop', '--daemon-dir', str(daemon_two))
                self.assertEqual(stopped.returncode, 0, stopped.stdout + stopped.stderr)
            finally:
                for directory in (daemon_one, daemon_two):
                    subprocess.run(
                        [sys.executable, '-B', str(VM / 'launch.py'), 'daemon',
                         'stop', '--daemon-dir', str(directory)],
                        cwd=VM, capture_output=True, text=True, timeout=60)

    def test_launcher_sigterm_cleans_owned_qemu(self):
        process = subprocess.Popen([sys.executable, '-B', str(VM / 'launch.py'), 'run',
                                    '--lock-timeout', '100',
                                    '--cmd', 'sleep 60', '--timeout', '90', '--storage', 'split',
                                    '--storage-size', 'update=32', '--storage-size', 'rwdata=32',
                                    '--storage-size', 'logdata=32'], cwd=VM,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        directory = None
        try:
            while True:
                line = process.stdout.readline()
                if not line:
                    self.fail('launcher exited before debugger readiness')
                if line.startswith('run: '):
                    directory = Path(line[5:].strip())
                if 'stopped at driver init' in line:
                    break
            process.send_signal(signal.SIGTERM)
            process.communicate(timeout=20)
            self.assertNotEqual(process.returncode, 0)
            result = json.loads((directory / 'result.json').read_text())
            self.assertIn('InterruptedError', result['error'])
            self.assert_cleaned(result)
        finally:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=20)


if __name__ == '__main__':
    unittest.main()
