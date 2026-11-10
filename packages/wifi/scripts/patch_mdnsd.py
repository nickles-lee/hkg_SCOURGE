#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU 260128. Experimental; disabled without USB marker.
"""Generate the optional uid-retention experiment from owner-provided firmware.

Never execute the vendor ELF. Refuse any input except the pinned stock bytes.
This does not establish the cause of multicast EPERM or CarPlay functionality.
"""
import hashlib
from pathlib import Path
import shlex
import sys


def patch(source, locks):
    if hashlib.sha256(source).hexdigest() != locks["MDNSD_STOCK_SHA256"]:
        raise ValueError("stock mdnsd SHA256 mismatch; refusing patch")
    offset = 0x43C2
    if source[offset:offset + 5] != bytes.fromhex("e819d8ffff"):
        raise ValueError("mdnsd setuid call bytes mismatch")
    result = source[:offset] + b"\x90" * 5 + source[offset + 5:]
    if hashlib.sha256(result).hexdigest() != locks["MDNSD_ROOTED_SHA256"]:
        raise ValueError("patched mdnsd SHA256 mismatch")
    return result


def main():
    lockfile = Path(__file__).resolve().parents[3] / "config/firmware_locks.env"
    locks = {}
    for line in lockfile.read_text().splitlines():
        fields = shlex.split(line, comments=True)
        if fields and "=" in fields[0]:
            key, value = fields[0].split("=", 1)
            locks[key] = value
    source, output = map(Path, sys.argv[1:])
    result = patch(source.read_bytes(), locks)
    output.write_bytes(result)
    output.chmod(0o755)


if __name__ == "__main__":
    main()
