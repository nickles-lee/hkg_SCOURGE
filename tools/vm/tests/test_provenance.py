# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import unittest

from launch import source_identities


class ProvenanceTests(unittest.TestCase):
    def test_probes_guest_tools_and_compat_sources_are_hashed(self) -> None:
        identities = source_identities()
        for name in (
            'guest/phase3.sh',
            'guest/applauncher_stub.c',
            'probes/ui-smoke.sh',
            'probes/qt-shutdown.md',
            'toolsroot/phase4.sh',
            'toolsroot/scenarios.sh',
            'compat/spike.py',
            'compat/mkinitcpio.conf',
            'launch.py',
        ):
            self.assertIn(name, identities, name)
            self.assertEqual(len(identities[name]), 64)
        self.assertNotIn('toolsroot/manifest.json', identities)


if __name__ == '__main__':
    unittest.main()
