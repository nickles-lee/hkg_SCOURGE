# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import csv
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

VM = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(VM))
import prepare_qt_debug as prepare

spec = importlib.util.spec_from_file_location('qt_backtrace_probe', VM / 'probes/qt-backtrace.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class QtDebugTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='qt-debug-test-', dir='/tmp/opencode')
        self.root = Path(self.temporary.name)
        self.graphics = self.root / 'graphics'
        (self.graphics / 'runtime').mkdir(parents=True)
        (self.graphics / 'GRAPHICS_RUNTIME').write_text('test fixture\n')
        (self.graphics / 'runtime/libfixture.so.1.0').write_bytes(b'inert fixture bytes')
        (self.graphics / 'runtime/libfixture.so.1').symlink_to('libfixture.so.1.0')

    def tearDown(self):
        self.temporary.cleanup()

    def test_prepared_small_runtime_is_accepted(self):
        prepare.validate_graphics(self.graphics)

    def test_non_runtime_entries_are_rejected(self):
        (self.graphics / 'unexpected.img').write_bytes(b'not a VM image')
        with self.assertRaises(ValueError):
            prepare.validate_graphics(self.graphics)

    def test_external_runtime_link_is_rejected(self):
        link = self.graphics / 'runtime/libexternal.so'
        link.symlink_to('/outside')
        with self.assertRaises(ValueError):
            prepare.validate_graphics(self.graphics)

    def test_runtime_directory_symlink_is_rejected(self):
        (self.graphics / 'runtime').rename(self.graphics / 'renamed')
        (self.graphics / 'runtime').symlink_to('renamed')
        with self.assertRaises(ValueError):
            prepare.validate_graphics(self.graphics)

    def test_wrong_package_pin_fails_before_output_is_created(self):
        cache = self.root / 'cache.deb'
        cache.write_bytes(b'not the pinned archive')
        output = self.root / 'bundle'
        with patch.object(sys, 'argv', ['prepare_qt_debug.py', '--output', str(output),
                                       '--graphics', str(self.graphics), '--cache', str(cache)]):
            with self.assertRaisesRegex(ValueError, 'digest/size mismatch'):
                prepare.main()
        self.assertFalse(output.exists())

    def test_existing_output_is_rejected_before_fetch(self):
        with patch.object(sys, 'argv', ['prepare_qt_debug.py', '--output', str(self.graphics),
                                       '--graphics', str(self.graphics)]), \
                patch.object(prepare.urllib.request, 'urlopen') as fetch:
            with self.assertRaises(ValueError):
                prepare.main()
            fetch.assert_not_called()

    def test_symbol_aliases_are_local_and_skip_traversal(self):
        inventory = self.root / 'binaries.csv'
        with inventory.open('w') as file:
            writer = csv.DictWriter(file, fieldnames=['root', 'path', 'soname'])
            writer.writeheader()
            writer.writerows([
                {'root': 'rootfs', 'path': 'usr/lib/libfixture.so.1.0', 'soname': 'libfixture.so.1'},
                {'root': 'rootfs', 'path': '../outside', 'soname': 'libbad.so'},
                {'root': 'rootfs', 'path': '/outside', 'soname': 'libabsolute.so'},
                {'root': 'rootfs', 'path': 'usr/lib/libbad.so', 'soname': '../bad'},
            ])
        output = self.root / 'symbols'
        self.assertEqual(prepare.symbol_links(self.root, inventory, output), 1)
        self.assertEqual((output / 'libfixture.so.1').readlink(), self.root / 'usr/lib/libfixture.so.1.0')
        self.assertEqual(len(list(output.iterdir())), 1)

    def test_probe_adds_only_unix_uart_and_restores_launcher(self):
        def run(*args, **kwargs):
            argv = probe.launch.qemu_argv(self.root)
            self.assertEqual(argv, ['-nic', 'none', '-serial', 'unix:userspace.sock,server=on,wait=off'])
            self.assertIn('GRAPHICS_GDB=1', args[0])
            self.assertTrue(kwargs['collect_audit'])
            self.assertEqual(kwargs['storage_mode'], 'split')
            return 1  # Capturing a hang must not turn the graphics gate into PASS.

        with patch.object(probe.launch, 'qemu_argv', return_value=['-nic', 'none']) as original, \
                patch.object(probe.launch, 'run', side_effect=run), \
                patch.object(sys, 'argv', ['qt-backtrace.py', '--tools', 'fixture-tools', '--payload', str(self.graphics)]):
            self.assertEqual(probe.main(), 1)
            self.assertIs(probe.launch.qemu_argv, original)

    def test_probe_bash_syntax_without_executing_firmware(self):
        text = (VM / 'probes/graphics-diagnose.sh').read_text()
        result = subprocess.run(['bash', '-n'], input=text, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        # Disconnect is intentionally after timed_out=1 and cannot satisfy the
        # normal-exit success branch even if the resulting process status is 0.
        self.assertLess(text.index('timed_out=1'), text.index('GRAPHICS_DISCONNECT_DIAGNOSTIC'))
        self.assertNotIn('timed_out=0', text[text.index('GRAPHICS_DISCONNECT_DIAGNOSTIC'):])


if __name__ == '__main__':
    unittest.main()
