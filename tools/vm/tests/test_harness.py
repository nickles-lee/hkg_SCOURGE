# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

VM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(VM))

import launch
from harness.config import load_profile, load_scenario, scenario_command
from harness.images import cleanup_overlays, create_overlays, drive_path, reserve_outputs, validate_output
from harness.protocol import command_frame, command_output, completion_pattern
from harness.results import evaluate, phase_summary


class TemporaryTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='harness-test-', dir='/tmp/opencode')
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()


class ImageTests(TemporaryTest):
    def test_symlinks_including_dangling_are_rejected(self):
        for existing in (False, True):
            source = self.root / f'source-{existing}'
            if existing:
                source.write_bytes(b'unchanged')
            link = self.root / f'link-{existing}'
            link.symlink_to(source)
            with self.assertRaises(ValueError):
                reserve_outputs([link])
            if existing:
                self.assertEqual(source.read_bytes(), b'unchanged')

    def test_existing_file_and_hardlink_are_rejected(self):
        source = self.root / 'base'
        source.write_bytes(b'unchanged')
        link = self.root / 'hardlink'
        os.link(source, link)
        for path in (source, link):
            with self.assertRaises(ValueError):
                reserve_outputs([path])
        self.assertEqual(source.read_bytes(), b'unchanged')

    def test_protected_and_shared_paths_are_rejected(self):
        with self.assertRaises(ValueError):
            validate_output(self.root / 'base', (self.root / 'base',))
        with self.assertRaises(ValueError):
            validate_output(Path('/mnt/shares/new.img'))
        alias = self.root / 'share'
        alias.symlink_to('/mnt/shares')
        with self.assertRaises(ValueError):
            validate_output(alias / 'new.img')

    def test_reservation_validates_all_paths_first(self):
        out = self.root / 'new'
        with self.assertRaises(ValueError):
            reserve_outputs([out, out])
        self.assertFalse(out.exists())
        reserve_outputs([out])
        self.assertTrue(out.is_file())

    def test_qemu_path_cannot_inject_drive_options(self):
        with self.assertRaises(ValueError):
            drive_path(self.root / 'image,readonly=off')

    def test_explicit_overlays_never_modify_backing(self):
        source = self.root / 'base.raw'
        source.write_bytes(b'base')
        def create(argv, **kwargs):
            Path(argv[-1]).write_bytes(b'overlay')
        with patch('harness.images.subprocess.run', side_effect=create) as run:
            overlays = create_overlays(self.root, {'rootfs': source})
        self.assertIn('-F', run.call_args.args[0])
        self.assertEqual(overlays['rootfs']['backing'], str(source))
        cleanup_overlays(self.root, overlays)
        self.assertFalse((self.root / 'rootfs.qcow2').exists())
        self.assertEqual(source.read_bytes(), b'base')

    def test_cleanup_rejects_backing_or_arbitrary_path(self):
        source = self.root / 'base.raw'
        source.write_bytes(b'base')
        with self.assertRaises(ValueError):
            cleanup_overlays(self.root, {'rootfs': {'path': str(source)}})
        self.assertTrue(source.exists())

    def test_builders_refuse_existing_backing_names(self):
        # No image creation happens: the safety check rejects the real symlinks.
        result = subprocess.run(['sh', 'build_rootfs.sh', '--force'], cwd=VM,
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        result = subprocess.run([sys.executable, '-m', 'harness.images', 'reserve',
                                 str(VM / 'rootfs.img')], cwd=VM, capture_output=True)
        self.assertNotEqual(result.returncode, 0)

    def test_actual_builders_refuse_symlink_outputs_without_touching_inputs(self):
        tree = self.root / 'source'
        tree.mkdir()
        backing = self.root / 'backing'
        backing.write_bytes(b'unchanged')
        link = self.root / 'image.img'
        link.symlink_to(backing)
        env = {**os.environ, 'ROOTFS': str(tree)}
        commands = [
            ['sh', 'build_rootfs.sh', '--output', str(link), '--kernel-output', str(self.root / 'new-kernel')],
            ['sh', 'make_update_usb.sh', str(tree), str(link)],
            ['sh', 'build_tools.sh', str(link)],
        ]
        for argv in commands:
            result = subprocess.run(argv, cwd=VM, env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('symlink output', result.stderr)
            self.assertEqual(backing.read_bytes(), b'unchanged')
            self.assertFalse((self.root / 'new-kernel').exists())

    def test_kernel_output_guard_prevents_partial_rootfs_reservation(self):
        tree = self.root / 'source'
        tree.mkdir()
        kernel = self.root / 'kernel'
        kernel.symlink_to(self.root / 'missing')
        output = self.root / 'new-rootfs'
        result = subprocess.run(['sh', 'build_rootfs.sh', '--output', str(output),
                                 '--kernel-output', str(kernel)], cwd=VM,
                                env={**os.environ, 'ROOTFS': str(tree)}, capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())


class ProtocolTests(unittest.TestCase):
    def test_frame_captures_nonzero_exit(self):
        token, frame = command_frame('exit 7')
        output = subprocess.run(['/bin/bash', '-c', frame], capture_output=True).stdout.decode()
        self.assertEqual(command_output(output, token), ('', 7))

    def test_frame_preserves_quotes_multiline_and_merged_output(self):
        token, frame = command_frame("printf '%s\\n' \"a'b\"\nprintf 'stderr\\n' >&2\nexit 9")
        output = subprocess.run(['/bin/bash', '-c', frame], stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT).stdout.decode()
        body, rc = command_output(output, token)
        self.assertEqual(body, "a'b\nstderr")
        self.assertEqual(rc, 9)

    def test_echo_cannot_satisfy_completion(self):
        token, frame = command_frame('echo hello')
        self.assertIsNone(re.search(completion_pattern(token), frame.encode()))
        with self.assertRaises(ValueError):
            command_output(frame, token)

    def test_wrong_token_and_unordered_frames_fail(self):
        token, _ = command_frame('true')
        with self.assertRaises(ValueError):
            command_output(f'IONIQ_COMMAND_END {token} 0\nIONIQ_COMMAND_BEGIN {token}\n', token)
        with self.assertRaises(ValueError):
            command_frame('true', 'not-a-token')

    def test_partial_exit_status_is_not_a_completed_frame(self):
        token, _ = command_frame('exit 77')
        prefix = f'IONIQ_COMMAND_END {token} 7'
        self.assertIsNone(re.search(completion_pattern(token), prefix.encode()))
        self.assertIsNotNone(re.search(completion_pattern(token), (prefix + '7\r\n').encode()))


class ResultTests(unittest.TestCase):
    def test_exit_and_phase_failures_are_not_success(self):
        for text, rc in [('PHASE4_FAIL missing tools\n', 0), ('RUN_ERROR: missing tools\n', 0),
                         ('', 7), ('PHASE4_OK\nPHASE4_FAIL\n', 0)]:
            self.assertEqual(evaluate(text, rc)['verdict'], 'FAIL')

    def test_scenario_requires_actual_checks(self):
        scenario = load_scenario('updater-launch')
        self.assertEqual(evaluate('APPLAUNCHER_STUB_LAUNCH appid=300\n', 0, scenario)['verdict'], 'FAIL')
        text = 'HARNESS_CHECK updater-exec PASS\nHARNESS_CHECK updater-alive PASS\n'
        self.assertEqual(evaluate(text, 0, scenario)['verdict'], 'PASS')

    def test_launch_request_not_readiness_or_exec(self):
        phase = phase_summary('APPLAUNCHER_STUB_LAUNCH appid=300\n')
        self.assertTrue(phase['launch_requested'])
        self.assertFalse(phase['appupgrade'])

    def test_blocked_has_explicit_reason_and_status(self):
        self.assertEqual(evaluate('HARNESS_BLOCKED missing TEE\n', 77)['verdict'], 'BLOCKED')
        self.assertEqual(evaluate('HARNESS_BLOCKED missing TEE\n', 1)['verdict'], 'FAIL')

    def test_echoed_markers_do_not_count(self):
        self.assertFalse(phase_summary("echo 'PHASE4_OK'\n")['ok'])
        self.assertEqual(evaluate("echo 'HARNESS_CHECK boot PASS'\n", 0,
                                  load_scenario('boot'))['verdict'], 'FAIL')

    def test_substitutions_are_explicit(self):
        result = evaluate('HARNESS_SUBSTITUTION decrypt-pass-through\n', 0)
        self.assertEqual(result['applied_substitutions'], ['decrypt-pass-through'])

    def test_crash_or_failed_check_cannot_be_hidden_by_pass_marker(self):
        for text in ('Aborted awk\n', 'Segmentation fault AppDMClient\n',
                     'HARNESS_CHECK unexpected FAIL\n'):
            self.assertEqual(evaluate(text + 'HARNESS_CHECK boot PASS\n', 0,
                                      load_scenario('boot'))['verdict'], 'FAIL')

    def test_blocked_marker_with_zero_exit_is_not_a_pass(self):
        self.assertEqual(evaluate('HARNESS_BLOCKED missing prerequisite\n', 0)['verdict'], 'FAIL')

    def test_conformance_rejects_reported_bypasses(self):
        result = evaluate('HARNESS_SUBSTITUTION decrypt-pass-through\n', 0,
                          profile=load_profile('conformance'))
        self.assertEqual(result['verdict'], 'FAIL')

    def test_later_pass_does_not_erase_failed_check(self):
        result = evaluate('HARNESS_CHECK boot FAIL\nHARNESS_CHECK boot PASS\n', 0,
                          load_scenario('boot'))
        self.assertEqual(result['verdict'], 'FAIL')


class ConfigTests(unittest.TestCase):
    def test_conformance_disallows_bypasses(self):
        self.assertFalse(any(load_profile('conformance')['allowed_workarounds'].values()))

    def test_scenarios_have_assertions_and_explicit_stock_blockers(self):
        for path in (VM / 'scenarios').glob('*.json'):
            item = load_scenario(path.stem)
            self.assertTrue(item['checks'])
        self.assertTrue(load_scenario('stock-update-stage')['blockers']['research'])
        self.assertTrue(load_scenario('stock-update-stage')['blockers']['conformance'])

    def test_unknown_names_cannot_select_arbitrary_files(self):
        for loader in (load_profile, load_scenario):
            with self.assertRaises(ValueError):
                loader('../outside')

    def test_boot_does_not_require_tools(self):
        command = scenario_command(load_scenario('boot'), load_profile('conformance'))
        self.assertIn('HARNESS_CHECK boot PASS', command)
        self.assertNotIn('/media/tools', command)

    def test_graphics_selection_is_explicit(self):
        command = scenario_command(load_scenario('updater-launch'), load_profile('research'), 'mesa112')
        self.assertIn('USE_MESA=1', command)
        with self.assertRaises(ValueError):
            scenario_command(load_scenario('boot'), load_profile('research'), 'unknown')


class LauncherTests(TemporaryTest):
    def test_qemu_uses_only_overlays_and_has_no_nic_or_host_mount(self):
        overlays = {name: {'path': str(self.root / f'{name}.qcow2')}
                    for name in ('rootfs', 'usb', 'tools')}
        argv = launch.qemu_argv(self.root, VM / 'update-k.img', VM / 'tools.img', overlays=overlays)
        self.assertEqual(argv[argv.index('-nic') + 1], 'none')
        self.assertIn('-no-reboot', argv)
        self.assertNotIn('-virtfs', argv)
        for index, value in enumerate(argv):
            if value == '-drive':
                self.assertIn('format=qcow2', argv[index + 1])
                self.assertNotIn('snapshot=on', argv[index + 1])

    def test_cache_invalidates_after_content_change(self):
        source = self.root / 'input'
        source.write_bytes(b'first')
        with patch.object(launch, 'HASH_CACHE', self.root / 'cache.json'):
            first = launch.cached_sha256(source)
            self.assertEqual(launch.cached_sha256(source, cache_only=True), first)
            source.write_bytes(b'other')
            self.assertIsNone(launch.cached_sha256(source, cache_only=True))
            self.assertNotEqual(launch.cached_sha256(source), first)

    def test_device_and_fifo_inputs_rejected_before_hashing(self):
        fifo = self.root / 'fifo'
        os.mkfifo(fifo)
        for path in (fifo, Path('/dev/null')):
            with self.assertRaises(ValueError):
                launch.image_identity(path)

    def test_invalid_optional_kernel_is_a_failed_check_not_system_exit(self):
        path = self.root / 'invalid-vmlinux'
        path.write_bytes(b'not an ELF')
        self.assertFalse(launch.kernel_is_patched(path))

    def test_blocked_scenario_does_not_start_qemu(self):
        with patch.object(launch, 'VM', self.root), patch.object(launch.subprocess, 'Popen') as popen, \
             patch.object(launch, 'qemu_version', return_value='test'):
            rc = launch.run(None, False, 5, profile_name='conformance', scenario_name='stock-update-stage')
        self.assertEqual(rc, 77)
        popen.assert_not_called()
        result = json.loads(next((self.root / 'runs').glob('*/result.json')).read_text())
        self.assertEqual(result['verdict'], 'BLOCKED')
        self.assertTrue(result['blockers'])

    def test_conformance_arbitrary_commands_rejected(self):
        with self.assertRaises(ValueError):
            launch.run('true', False, 5, profile_name='conformance')

    def test_nonfinite_deadlines_are_rejected(self):
        for value in (0, -1, float('nan'), float('inf')):
            with self.assertRaises(ValueError):
                launch.run('true', False, value)

    def test_gdb_eof_fails_instead_of_spinning(self):
        sock = Mock()
        sock.recv.return_value = b''
        with self.assertRaises(EOFError):
            launch.RSP(sock).cmd('?')


if __name__ == '__main__':
    unittest.main()
