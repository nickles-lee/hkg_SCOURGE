# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

from harness.update_media import (check_update_media, parse_manifest,
                                  parse_ver)

VM = Path(__file__).resolve().parents[1]


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def crc32(data: bytes) -> int:
    value = zlib.crc32(data)
    return value - (1 << 32) if value >= (1 << 31) else value


def make_media(root: Path) -> dict[str, bytes]:
    (root / 'HU/images').mkdir(parents=True)
    (root / 'HU/firmware').mkdir(parents=True)
    encrypted = b'\x0f\x09\x21\x8f' + bytes(range(32))
    archive = b'rootfs-payload'
    firmware = b'firmware-payload'
    (root / '.lge.upgrade.xml').write_bytes(encrypted)
    (root / 'HU/images/mango-rootfs.tar.gz').write_bytes(archive)
    (root / 'HU/firmware/update.tar.gz').write_bytes(firmware)
    ver = (
        '+|21393|NE_E.EUR.S5W_L.001.001.260128|HM|IONIQ5_EU|1489|1\n'
        f'IONIQ5_EU\\HU\\images|mango-rootfs.tar.gz|341174|'
        f'{crc32(archive)}|{len(archive)}|1\n'
        f'IONIQ5_EU\\HU\\firmware|update.tar.gz|341146|{crc32(firmware)}|'
        f'{len(firmware)}|1\n'
        f'IONIQ5_EU|.lge.upgrade.xml|341175|{crc32(encrypted)}|'
        f'{len(encrypted)}|1\n'
    ).encode()
    (root / 'IONIQ5_EU.ver').write_bytes(ver)
    manifest = (f'{digest(ver)}  IONIQ5_EU.ver\n'
                f'{digest(archive)}  HU/images/mango-rootfs.tar.gz\n').encode()
    (root / 'MANIFEST.sha256').write_bytes(manifest)
    return {'ver': ver, 'manifest': manifest, 'archive': archive,
            'firmware': firmware, 'encrypted': encrypted}


class UpdateMediaTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix='vm-update-media-')
        self.root = Path(self._tmp.name) / 'media'
        self.root.mkdir()
        self.files = make_media(self.root)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_valid_directory(self) -> None:
        report = check_update_media(self.root, verify_hashes=True,
                                    verify_files=True)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['mode'], 'dir')
        self.assertEqual(report['header']['product'], 'IONIQ5_EU')
        self.assertEqual(report['header']['version'],
                         'NE_E.EUR.S5W_L.001.001.260128')
        self.assertEqual(report['entries'], 3)
        self.assertEqual(report['manifest_entries'], 2)
        self.assertEqual(report['present_entries'], 3)
        self.assertEqual(report['hashes_verified'], 2)
        self.assertEqual(report['missing_manifest_targets'], [])
        self.assertEqual(report['mismatched'], [])

    def test_missing_required_file(self) -> None:
        (self.root / '.lge.upgrade.xml').unlink()
        report = check_update_media(self.root)
        self.assertFalse(report['ok'])
        self.assertIn('missing required file: .lge.upgrade.xml',
                      '\n'.join(report['problems']))

    def test_missing_manifest_target(self) -> None:
        (self.root / 'HU/images/mango-rootfs.tar.gz').unlink()
        report = check_update_media(self.root)
        self.assertFalse(report['ok'])
        self.assertEqual(report['missing_manifest_targets'],
                         ['HU/images/mango-rootfs.tar.gz'])

    def test_hash_mismatch(self) -> None:
        with (self.root / 'HU/images/mango-rootfs.tar.gz').open('ab') as handle:
            handle.write(b'!')
        report = check_update_media(self.root, verify_hashes=True)
        self.assertFalse(report['ok'])
        self.assertEqual(report['mismatched'], ['HU/images/mango-rootfs.tar.gz'])

    def test_manifest_traversal_and_duplicates_are_rejected(self) -> None:
        manifest = self.files['manifest'].decode()
        manifest += f'{digest(b"x")}  ../outside\n'
        manifest += manifest.splitlines()[0] + '\n'
        (self.root / 'MANIFEST.sha256').write_text(manifest)
        report = check_update_media(self.root)
        joined = '\n'.join(report['problems'])
        self.assertFalse(report['ok'])
        self.assertIn('path traversal', joined)
        self.assertIn('duplicate path', joined)

    def test_unsafe_package_map_entries_are_rejected(self) -> None:
        ver = self.files['ver'].decode()
        ver += 'IONIQ5_EU\\..\\evil|payload|1|1|1|1\n'
        ver += 'IONIQ5_EU\\HU|bad/name|1|1|1|1\n'
        ver += 'IONIQ5_EU\\HU|short|1|1|1\n'
        (self.root / 'IONIQ5_EU.ver').write_text(ver)
        report = check_update_media(self.root)
        joined = '\n'.join(report['problems'])
        self.assertFalse(report['ok'])
        self.assertIn('unsafe package path', joined)
        self.assertIn('unsafe filename', joined)
        self.assertIn('expected 6', joined)

    def test_declared_size_mismatch(self) -> None:
        ver = self.files['ver'].decode().replace(
            f'|{len(self.files["firmware"])}|1',
            f'|{len(self.files["firmware"]) + 1}|1')
        (self.root / 'IONIQ5_EU.ver').write_text(ver)
        report = check_update_media(self.root, verify_files=True)
        self.assertFalse(report['ok'])
        self.assertIn('size mismatch: HU/firmware/update.tar.gz',
                      '\n'.join(report['problems']))

    def test_manifest_parser_handles_binary_marker(self) -> None:
        parsed = parse_manifest(f'{digest(b"x")} *HU/images/a.bin\n')
        self.assertEqual(parsed['entries'], [(digest(b'x'), 'HU/images/a.bin')])

    def test_package_map_parser_reports_bad_header(self) -> None:
        parsed = parse_ver('not-a-header\n')
        self.assertFalse(parsed['entries'])
        self.assertTrue(parsed['problems'])

    def test_cli(self) -> None:
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'check-update',
             str(self.root), '--verify-hashes', '--verify-files'],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(json.loads(completed.stdout)['ok'])
        (self.root / 'MANIFEST.sha256').write_text('bad\n')
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'), 'check-update',
             str(self.root)], cwd=VM, capture_output=True, text=True,
            check=False)
        self.assertEqual(completed.returncode, 1)
        self.assertFalse(json.loads(completed.stdout)['ok'])

    @unittest.skipUnless(shutil.which('mke2fs'), 'mke2fs unavailable')
    def test_ext4_image_roundtrip(self) -> None:
        image = Path(self._tmp.name) / 'update.img'
        completed = subprocess.run(
            ['mke2fs', '-q', '-t', 'ext4', '-d', str(self.root),
             str(image), '16M'], capture_output=True, text=True)
        if completed.returncode != 0:
            self.skipTest(f'mke2fs -d unavailable: {completed.stderr[:120]}')
        report = check_update_media(image, verify_hashes=True,
                                    verify_files=True)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['mode'], 'image')
        self.assertEqual(report['manifest_entries'], 2)
        self.assertEqual(report['present_entries'], 3)

    @unittest.skipUnless(os.environ.get('VM_REAL_UPDATE_MEDIA') == '1',
                         'set VM_REAL_UPDATE_MEDIA=1 for the captured stick')
    def test_real_update_image(self) -> None:
        image = VM / 'update-k.img'
        if not image.is_file():
            self.skipTest('update-k.img unavailable')
        report = check_update_media(image)
        self.assertTrue(report['ok'], report['problems'])
        self.assertEqual(report['mode'], 'image')
        self.assertEqual(report['header']['product'], 'IONIQ5_EU')
        self.assertEqual(report['entries'], 35)


if __name__ == '__main__':
    unittest.main()
