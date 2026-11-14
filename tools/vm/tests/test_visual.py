# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from harness import visual

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None


@unittest.skipUnless(Image is not None, "Pillow required")
class VisualTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-visual-")
        self.root = Path(self._tmp.name)
        self.baselines = self.root / "baselines"
        self.base = self.root / "base.png"
        self.same = self.root / "same.png"
        self.changed = self.root / "changed.png"
        Image.new("RGB", (4, 4), (10, 20, 30)).save(self.base)
        Image.new("RGB", (4, 4), (10, 20, 30)).save(self.same)
        image = Image.new("RGB", (4, 4), (10, 20, 30))
        image.putpixel((0, 0), (255, 0, 0))
        image.save(self.changed)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_baseline_and_identical_compare(self) -> None:
        metadata = visual.baseline(self.base, "shot", self.baselines, "run-1")
        self.assertEqual(metadata["width"], 4)
        self.assertTrue((self.baselines / "shot.png").is_file())
        status = visual.compare(self.same, "shot", self.baselines)
        self.assertTrue(status["ok"], status)
        self.assertEqual(status["changed_pixels"], 0)
        self.assertEqual(status["ratio"], 0.0)

    def test_pixel_change_and_threshold(self) -> None:
        visual.baseline(self.base, "shot", self.baselines)
        status = visual.compare(self.changed, "shot", self.baselines, threshold=0.01)
        self.assertFalse(status["ok"])
        self.assertEqual(status["changed_pixels"], 1)
        self.assertGreater(status["ratio"], 0.01)
        ok = visual.compare(self.changed, "shot", self.baselines, threshold=0.1)
        self.assertTrue(ok["ok"])
        self.assertTrue(Path(ok["diff"]).is_file())

    def test_threshold_and_tolerance_bounds(self) -> None:
        visual.baseline(self.base, "shot", self.baselines)
        for threshold, tolerance in ((-0.1, 8), (1.1, 8), (0.01, -1),
                                     (0.01, 256)):
            with self.assertRaises(ValueError):
                visual.compare(self.same, "shot", self.baselines,
                               threshold=threshold, tolerance=tolerance)
        self.assertTrue(visual.compare(self.same, "shot", self.baselines,
                                       tolerance=0)["ok"])
        self.assertTrue(visual.compare(self.same, "shot", self.baselines,
                                       tolerance=255)["ok"])

    def test_size_mismatch_and_missing_baseline(self) -> None:
        visual.baseline(self.base, "shot", self.baselines)
        big = self.root / "big.png"
        Image.new("RGB", (8, 8), (0, 0, 0)).save(big)
        status = visual.compare(big, "shot", self.baselines)
        self.assertFalse(status["ok"])
        self.assertEqual(status["reason"], "size-mismatch")
        with self.assertRaises(ValueError):
            visual.compare(self.same, "missing", self.baselines)


if __name__ == "__main__":
    unittest.main()
