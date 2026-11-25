#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Build/run the offline daemon readiness fixture (no wlan/iptables changes)."""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEST = ROOT / "tests/test_carplay_readiness.c"

with tempfile.TemporaryDirectory(prefix="wifi1b-readiness-", dir="/tmp/opencode") as d:
    tmp = Path(d)
    app = tmp / "app"
    app.mkdir()
    broker = tmp / "broker.json"
    binary = tmp / "test_readiness"
    command = [
        "gcc", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-Wno-format-truncation", "-Wno-unused-function",
        "-Wno-unused-variable", "-Wno-unused-but-set-variable",
        "-DWIFI_TEST", f'-DAPP_DIR="{app}"',
        f'-DCARPLAY_BROKER_STATUS="{broker}"',
        '-DWIFI_BUILD_ID="test"', "-static", "-o", str(binary), str(TEST),
    ]
    subprocess.run(command, check=True, timeout=120)
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    result = subprocess.run([str(binary)], check=True, timeout=60,
                            text=True, capture_output=True, env=env)
    assert "PASS:" in result.stdout, result.stdout
print("PASS: CarPlay daemon readiness fixture")
