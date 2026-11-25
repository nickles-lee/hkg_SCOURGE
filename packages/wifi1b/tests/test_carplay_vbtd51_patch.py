#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Read-only vbtd-51 RX-hook builder gate.

Uses the owner-supplied vendor root when present; otherwise reports SKIP.
The OEM binary is read only and never executed.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from build_carplay_spp_patch import TARGETS, build_patch

ROOTFS = Path(os.environ.get("SCOURGE_ROOTFS", ""))
INPUT = ROOTFS / "app/bin/vbtd-51"
if not INPUT.is_file():
    print("SKIP: SCOURGE_ROOTFS/app/bin/vbtd-51 is absent")
    raise SystemExit(0)

with tempfile.TemporaryDirectory(prefix="vbtd51-patch-", dir="/tmp/opencode") as d:
    result = build_patch(INPUT, Path(d), target="51")
    manifest = result["manifest"]
    assert manifest["target"] == "vbtd-51", manifest["target"]
    assert manifest["input_sha256"] == (
        "55e21d077082e97f56fb113a1d3b216c0f2e63aa02fc4cd754a6593bc6cec9e0")
    assert manifest["function_elf_vaddr"] == "0x490ec0"
    assert manifest["function_file_offset"] == "0x490ec0"
    assert manifest["function_length"] == TARGETS["51"]["len"]
    assert manifest["function_preimage_sha256"] == (
        "2f3ba61377b73547026a634511ed238899c37ca06fdb111e0f3e1919e3493f06")
    assert manifest["car_validated"] is False
    assert manifest["installer_activated"] is False
    assert manifest["oem_binary_executed"] is False
    patched = Path(result["output_file"]).read_bytes()
    stock = INPUT.read_bytes()
    assert len(patched) == len(stock)
    off = int(manifest["function_file_offset"], 16)
    length = int(manifest["function_length"])
    assert patched[:off] == stock[:off]
    assert patched[off + length:] == stock[off + length:]
    assert patched[off:off + 4] != b"\x90\x90\x90\x90"
    assert len(patched[off:off + length]) == length

print("PASS: vbtd-51 target gates, exact preimage and outside-span preservation")
