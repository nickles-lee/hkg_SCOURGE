#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""carve_engine.py — extract and verify the embedded updater engine.

TARGET: Gen5W_L firmware NE_E.EUR.S5W_L.001.001.260128 (other firmwares:
parameterize and re-verify — see docs/09-porting.md).

/app/bin/AppUpgrade is a host ELF with the AppScomoUpgrade engine (itself an
ELF) embedded at file offset 0x7C64 (31,844) on 260128. Every validator
patch offset in payload/patch_validators.py is engine-relative, so:

    file_offset = ENGINE_BASE + engine_offset
    e.g. 0x7C64 + 0x1F5A00 = 0x1FD664 = 2,086,500

This tool:
  1. verifies the input's sha256 against config/firmware_locks.env
     (fast path: hash match -> the pinned patch table applies as-is),
  2. locates the embedded engine ELF (default offset 0x7C64, or --scan),
  3. carves it to a file for Ghidra import (docs/10),
  4. dumps the prologue bytes at each pinned patch offset.

Usage:
  carve_engine.py PLAIN_APPUPGRADE [--out engine.elf] [--base 0x7C64]
  carve_engine.py PLAIN_APPUPGRADE --scan
"""
import argparse
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "payload"))
from patch_validators import (ENGINE_PATCHES, ENGINE_SIZE,  # noqa: E402
                              ENGINE_STOCK_SHA256)


def find_elf_magics(data, limit=None):
    offs = []
    start = 4  # skip the host ELF's own header at 0
    end = limit or len(data)
    i = start
    while i < end - 4:
        i = data.find(b"\x7fELF", i)
        if i < 0:
            break
        offs.append(i)
        i += 4
    return offs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("--out")
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0x7C64)
    ap.add_argument("--scan", action="store_true")
    a = ap.parse_args()

    data = open(a.src, "rb").read()
    if len(data) != ENGINE_SIZE:
        sys.exit("unexpected size %d (want %d)" % (len(data), ENGINE_SIZE))
    h = hashlib.sha256(data).hexdigest()
    print("sha256: %s" % h)
    print("        %s" % ("== pinned stock 260128 AppUpgrade (fast path: "
                         "reuse the patch table)" if h == ENGINE_STOCK_SHA256
                         else "DIFFERENT from the pinned 260128 binary -> "
                              "full re-derivation, see docs/10"))

    if a.scan:
        print("ELF magics at: %s" %
              ", ".join(hex(o) for o in find_elf_magics(data)))
        return 0

    if data[a.base:a.base + 4] != b"\x7fELF":
        sys.exit("no ELF magic at --base %#x; rerun with --scan to locate "
                 "the embedded engine" % a.base)
    print("engine ELF confirmed at %#x" % a.base)

    for name, off, expect in ENGINE_PATCHES:
        cur = data[off:off + len(expect)]
        print("%-14s file %#010x  %s%s" %
              (name, off, cur.hex(),
               "  (stock)" if cur == expect else
               ("  (stubbed)" if cur[:3] == b"\x31\xc0\xc3" else "  (???)")))

    if a.out:
        # Carve to the end of the host file; the engine occupies the tail.
        open(a.out, "wb").write(data[a.base:])
        print("carved %d bytes -> %s (import into Ghidra as x86-64 ELF; "
              "engine vaddr 0 = file offset %#x of the host)" %
              (len(data) - a.base, a.out, a.base))
    return 0


if __name__ == "__main__":
    sys.exit(main())
