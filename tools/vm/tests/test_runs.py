# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path

from harness import runs


class RunManagementTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="vm-runs-")
        self.root = Path(self._tmp.name)
        self.runs = self.root / "runs"
        self.runs.mkdir()
        for index in range(5):
            path = self.runs / f"run-{index}"
            path.mkdir()
            (path / "result.json").write_text(json.dumps({
                "label": "smoke" if index % 2 == 0 else "other",
                "verdict": "PASS" if index != 4 else "FAIL",
                "mode": "compat",
                "scenario": {"name": "boot"},
                "keep_overlay": index == 1,
                "screenshot": {},
            }))
            os.utime(path, (1000 + index, 1000 + index))
            (path / "shot.png").write_bytes(b"png")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_collect_and_filter(self) -> None:
        all_runs = runs.collect_runs(self.root)
        self.assertEqual(len(all_runs), 5)
        self.assertEqual(all_runs[0]["run"], "run-4")  # newest mtime
        self.assertEqual([item["run"] for item in runs.filter_runs(all_runs, label="smoke")],
                         ["run-4", "run-2", "run-0"])
        self.assertEqual([item["run"] for item in runs.filter_runs(all_runs, verdict="FAIL")],
                         ["run-4"])

    def test_prune_is_dry_run_by_default(self) -> None:
        report = runs.prune(self.root, keep=2)
        self.assertFalse(report["apply"])
        self.assertEqual(len(report["candidates"]), 2)
        self.assertTrue((self.runs / "run-0").is_dir())
        self.assertTrue(all((self.runs / f"run-{i}").is_dir() for i in range(5)))

    def test_prune_protects_retained_overlays_and_applies(self) -> None:
        report = runs.prune(self.root, keep=2, apply=True)
        self.assertTrue(report["apply"])
        self.assertIn("run-1", [item["run"] for item in report["protected"]])
        self.assertTrue((self.runs / "run-1").is_dir())
        self.assertFalse((self.runs / "run-0").exists())
        self.assertFalse((self.runs / "run-2").exists())
        self.assertTrue((self.runs / "run-3").is_dir())
        self.assertTrue((self.runs / "run-4").is_dir())

    def test_prune_protects_daemon_and_resultless_dirs(self) -> None:
        daemon_dir = self.runs / "daemon"
        daemon_dir.mkdir()
        (daemon_dir / "control.sock").write_text("")
        regression = self.runs / "regression-123"
        regression.mkdir()
        (regression / "summary.json").write_text("{}")
        os.utime(daemon_dir, (1, 1))
        os.utime(regression, (1, 1))
        report = runs.prune(self.root, keep=1, apply=True)
        self.assertTrue(daemon_dir.is_dir())
        self.assertTrue(regression.is_dir())
        reasons = {item["run"]: item["reason"] for item in report["protected"]}
        self.assertEqual(reasons.get("daemon"), "daemon")
        self.assertEqual(reasons.get("regression-123"), "no-result")

    def test_extract_and_path_traversal(self) -> None:
        metadata = runs.extract(self.root, "run-3", "shot.png", self.root / "out.png")
        self.assertEqual((self.root / "out.png").read_bytes(), b"png")
        self.assertEqual(metadata["size"], 3)
        with self.assertRaises(ValueError):
            runs.extract(self.root, "run-3", "../run-2/result.json", self.root / "bad")
        with self.assertRaises(ValueError):
            runs.extract(self.root, "../run-2", "result.json", self.root / "bad")


if __name__ == "__main__":
    unittest.main()
