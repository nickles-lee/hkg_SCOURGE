#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Check the optional artifact and reject foreign input; never execute an ELF."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from patch_mdnsd import patch

data = (ROOT / "out/usb/payload/bin/mdnsd.rooted").read_bytes()
stock = data[:0x43C2] + bytes.fromhex("e819d8ffff") + data[0x43C7:]
locks = {
    "MDNSD_STOCK_SHA256": "71122d8f59cdfd248f80414014e22114b0ee486415e5a7de23f5acd1676b9cdd",
    "MDNSD_ROOTED_SHA256": "3f1cfd19210d6ac34ce24a215ed5066054ba4bbf487714f1c969114d86c6525f",
}
assert hashlib.sha256(stock).hexdigest() == locks["MDNSD_STOCK_SHA256"]
assert patch(stock, locks) == data
assert data[0x43C2:0x43C7] == b"\x90" * 5
with tempfile.TemporaryDirectory(prefix="mdns-pin-", dir="/tmp/opencode") as d:
    source, output = Path(d) / "foreign", Path(d) / "output"
    source.write_bytes(b"foreign firmware")
    result = subprocess.run([sys.executable, str(ROOT / "scripts/patch_mdnsd.py"), str(source), str(output)], capture_output=True)
    assert result.returncode != 0 and not output.exists()
print("PASS: mdnsd stock/patched pins, exact five-byte delta, foreign input rejected without output")
