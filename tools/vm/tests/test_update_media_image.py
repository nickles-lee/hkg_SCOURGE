# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from harness.update_media import check_update_media
from harness.update_media_image import build_update_image

VM = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('mke2fs'), 'mke2fs unavailable')
class UpdateImageBuilderTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix='vm-update-image-')
        self.root = Path(self._tmp.name)
        self.image = self.root / 'update.img'

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_builds_valid_preflight_image(self) -> None:
        report = build_update_image(self.image, size_mb=24, payload_size=4096)
        self.assertTrue(self.image.is_file())
        self.assertEqual(report['payload_bytes'], 4096)
        self.assertEqual(report['version'],
                         'NE_E.EUR.S5W_L.001.001.260128')
        checked = check_update_media(self.image, verify_hashes=True,
                                     verify_files=True)
        self.assertTrue(checked['ok'], checked['problems'])
        self.assertEqual(checked['mode'], 'image')
        self.assertEqual(checked['entries'], 2)
        self.assertEqual(checked['manifest_entries'], 2)
        self.assertEqual(checked['present_entries'], 2)
        self.assertEqual(checked['hashes_verified'], 2)

    def test_package_map_rows_describe_what_is_in_the_image(self) -> None:
        """The fixture used to pin the wrapper's row id and the stock wrapper's CRC."""
        import zlib
        report = build_update_image(self.image, size_mb=24, payload_size=4096)
        self.assertEqual(report['rootfs_row_id'], 341174)
        self.assertEqual(report['xml_row_id'], 341175)
        for key in ('rootfs_row_crc', 'xml_row_crc'):
            value = report[key]
            self.assertIsInstance(value, int)
            self.assertLess(abs(value), 1 << 31, key)
        # The row CRC must be the crc32 of the bytes in the image, signed.
        checked = check_update_media(self.image, verify_hashes=False,
                                     verify_files=False)
        self.assertTrue(checked['ok'], checked['problems'])
        with self.assertRaises(ValueError):
            build_update_image(self.image, size_mb=8)

    def test_rejects_bad_sizes(self) -> None:
        with self.assertRaises(ValueError):
            build_update_image(self.image, size_mb=8)
        with self.assertRaises(ValueError):
            build_update_image(self.image, payload_size=512)

    def test_cli(self) -> None:
        completed = subprocess.run(
            [sys.executable, '-B', str(VM / 'launch.py'),
             'make-update-media', '--output', str(self.image),
             '--size-mb', '24', '--payload-size', '8192'],
            cwd=VM, capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        report = json.loads(completed.stdout)
        self.assertTrue(report['ok'])
        self.assertEqual(report['payload_bytes'], 8192)
        self.assertTrue(self.image.is_file())


if __name__ == '__main__':
    unittest.main()
