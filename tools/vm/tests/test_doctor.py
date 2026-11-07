# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest

from harness.doctor import version_tuple


class DoctorProbeTests(unittest.TestCase):
    def test_version_tuple(self) -> None:
        self.assertEqual(version_tuple("QEMU emulator version 11.1.2"), (11, 1, 2))
        self.assertEqual(version_tuple("QEMU emulator version 8.0.0 (v8.0.0)"), (8, 0, 0))
        self.assertEqual(version_tuple("unavailable"), (0, 0, 0))

    def test_minimum_is_explicit(self) -> None:
        from harness.doctor import MIN_QEMU
        self.assertGreaterEqual(MIN_QEMU, (8, 0, 0))


if __name__ == "__main__":
    unittest.main()
