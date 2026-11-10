#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Run first-party C behavior tests in private paths, never the vehicle/host /etc."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="wifi-regressions-", dir="/tmp/opencode") as temporary:
    root = Path(temporary)
    binary = root / "regressions"
    subprocess.run(["gcc", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-function",
                    "-Wno-unused-parameter", "-Wno-format-truncation", "-fsanitize=undefined",
                    f'-DAPP_DIR="{root / "state"}"', str(ROOT / "tests/test_regressions.c"),
                    "-o", str(binary)], check=True)
    (root / "connmanctl").symlink_to(binary)
    import shutil
    shutil.copy2(binary, root / "supplicantmanager")
    env = dict(os.environ, PATH=str(root) + ":" + os.environ["PATH"])
    subprocess.run([str(binary)], env=env, check=True, timeout=30)
subprocess.run(["node", str(ROOT / "tests/test_home.js")], check=True, timeout=15)
