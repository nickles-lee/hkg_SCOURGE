#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Fixture checks for the conservative Apple-USB gate."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src/carplay"
CC = "cc"

with tempfile.TemporaryDirectory(prefix="carplay-usb-", dir="/tmp/opencode") as d:
    tmp = Path(d)
    BIN = tmp / "carplay-usb-test"
    subprocess.run([
        CC, "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-I", str(SRC), str(SRC / "cp_usb.c"),
        str(ROOT / "tests/test_carplay_usb.c"), "-o", str(BIN),
    ], check=True, timeout=120)

    def probe(root: Path, expected: str) -> None:
        r = subprocess.run([str(BIN), str(root), expected],
                           capture_output=True, text=True, timeout=20)
        assert r.returncode == 0, (root, r.stdout, r.stderr)

    apple = tmp / "apple" / "1-1"
    apple.mkdir(parents=True)
    (apple / "idVendor").write_text("05ac\n")
    (tmp / "apple" / "1-1" / "idProduct").write_text("1234\n")
    probe(tmp / "apple", "1")

    other = tmp / "other" / "2-1"
    other.mkdir(parents=True)
    (other / "idVendor").write_text("1d6b\n")
    probe(tmp / "other", "0")

    # Interface child without idVendor must not be treated as a device.
    iface = tmp / "other" / "2-1" / "2-1:1.0"
    iface.mkdir(parents=True)
    probe(tmp / "other", "0")

    # A non-interface device entry without idVendor fails closed.
    missing_vendor = tmp / "bad" / "5-1"
    missing_vendor.mkdir(parents=True)
    probe(tmp / "bad", "1")

    # Empty idVendor fails closed.
    empty = tmp / "empty" / "3-1"
    empty.mkdir(parents=True)
    (empty / "idVendor").write_text("")
    probe(tmp / "empty", "1")

    # Unreadable idVendor fails closed for an unprivileged process.
    deny = tmp / "deny" / "4-1"
    deny.mkdir(parents=True)
    (deny / "idVendor").write_text("05ac\n")
    if os.geteuid() != 0:
        (deny / "idVendor").chmod(0)
        probe(tmp / "deny", "1")
        (deny / "idVendor").chmod(0o600)

    # Missing/non-directory root fails closed.
    probe(tmp / "missing", "1")
    (tmp / "plain-file").write_text("not a directory\n")
    probe(tmp / "plain-file", "1")

print("PASS: Apple-USB gate blocks Apple, allows others, fails closed")
