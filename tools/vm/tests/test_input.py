# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import launch
from harness import qmp


class InputTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-input-")
        self.root = Path(self._tmp.name)
        self.overlays = {"rootfs": {"path": str(self.root / "rootfs.qcow2")}}

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_click_converts_to_absolute_tablet_coordinates(self) -> None:
        with patch.object(qmp, "send_events", return_value={"ok": True}) as send:
            result = qmp.click(self.root / "qmp.sock", 640, 360, 1280, 720)
        events = send.call_args[0][1]
        self.assertEqual(events[0]["data"]["axis"], "x")
        self.assertEqual(events[0]["data"]["value"], round(640 * 32767 / 1279))
        self.assertEqual(events[1]["data"]["value"], round(360 * 32767 / 719))
        self.assertEqual(events[2]["data"], {"down": True, "button": "left"})
        self.assertEqual(result["x"], 640)

    def test_click_rejects_out_of_bounds(self) -> None:
        with self.assertRaises(ValueError):
            qmp.click(self.root / "qmp.sock", 1280, 10, 1280, 720)

    def test_click_rejects_invalid_button(self) -> None:
        with self.assertRaisesRegex(ValueError, "invalid button"):
            qmp.click(self.root / "qmp.sock", 10, 10, button="bogus")
        with patch.object(qmp, "send_events", return_value={"ok": True}):
            result = qmp.click(self.root / "qmp.sock", 10, 10,
                               button="middle")
        self.assertEqual(result["button"], "middle")

    def test_parse_click(self) -> None:
        self.assertEqual(launch.parse_click('10,20'), (10, 20))
        self.assertEqual(launch.parse_click(' 3 , 4 '), (3, 4))
        for value in ('10', '1,2,3', 'a,b', ''):
            with self.assertRaises(ValueError):
                launch.parse_click(value)

    def test_key_sends_down_and_up(self) -> None:
        with patch.object(qmp, "send_events", return_value={"ok": True}) as send:
            qmp.key(self.root / "qmp.sock", "ret")
        events = send.call_args[0][1]
        self.assertEqual(events[0]["data"]["down"], True)
        self.assertEqual(events[1]["data"]["down"], False)
        self.assertEqual(events[0]["data"]["key"], {"type": "qcode", "data": "ret"})

    def test_display_backends_and_input_devices(self) -> None:
        vnc = launch.qemu_argv(self.root, overlays=self.overlays,
                               kernel_target="compat", display="vnc",
                               vnc_display=":7")
        self.assertEqual(vnc[vnc.index("-vnc") + 1], ":7")
        self.assertIn("virtio-tablet-pci", vnc)
        self.assertIn("virtio-keyboard-pci", vnc)
        gtk = launch.qemu_argv(self.root, overlays=self.overlays, display="gtk")
        self.assertEqual(gtk[gtk.index("-display") + 1], "gtk")
        self.assertNotIn("virtio-tablet-pci", gtk)
        none = launch.qemu_argv(self.root, overlays=self.overlays)
        self.assertEqual(none[none.index("-display") + 1], "none")


if __name__ == "__main__":
    unittest.main()
