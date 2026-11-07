#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Patch the vendor vmlinux SDHCI PCI ID table offline.

The vendor 4.1.27 kernel does not know QEMU's sdhci-pci ID (1b36:0007).
launch.py normally patches the table entry in guest RAM through the gdbstub.
This script applies the identical 20-byte change to a copy of vmlinux so the
patched kernel can be booted with -kernel vmlinux.patched and no gdbstub.

Usage: ./patch_kernel.py [--check]
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path
from harness.images import validate_output

VM = Path(__file__).resolve().parent
SRC = VM / "vmlinux"
OUT = VM / "vmlinux.patched"

TABLE_ENTRY = 0xFFFFFFFF8164D100
EXPECTED = bytes.fromhex(
    "22100000ffffffffffffffffffffffff0005080000ffff0040d16481ffffffff")
COMPAT = struct.pack("<6IQ", 0x1B36, 7, 0xFFFFFFFF, 0xFFFFFFFF,
                     0x080500, 0xFFFF00, 0)


def vaddr_to_offset(blob: bytes, vaddr: int) -> int:
    if blob[:4] != b"\x7fELF" or blob[4] != 2:
        raise SystemExit(f"{SRC}: not an ELF64 file")
    phoff = struct.unpack_from("<Q", blob, 0x20)[0]
    phentsize = struct.unpack_from("<H", blob, 0x36)[0]
    phnum = struct.unpack_from("<H", blob, 0x38)[0]
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type = struct.unpack_from("<I", blob, off)[0]
        if p_type != 1:  # PT_LOAD
            continue
        p_offset, p_vaddr, _p_paddr, p_filesz = struct.unpack_from(
            "<QQQQ", blob, off + 8)
        if p_vaddr <= vaddr < p_vaddr + p_filesz:
            return p_offset + (vaddr - p_vaddr)
    raise SystemExit(f"{SRC}: vaddr {vaddr:#x} is not inside a LOAD segment")


def add_pvh_note(blob: bytearray) -> bytearray:
    """Add a Xen PVH note so QEMU can boot the uncompressed ELF kernel."""
    phoff = struct.unpack_from("<Q", blob, 0x20)[0]
    phentsize = struct.unpack_from("<H", blob, 0x36)[0]
    phnum = struct.unpack_from("<H", blob, 0x38)[0]
    # XEN_ELFNOTE_PHYS32_ENTRY: namesz=4, descsz=4, type=18.
    entry = 0x1000000
    note = struct.pack("<III", 4, 4, 18) + b"Xen\x00" + struct.pack("<I", entry)
    note_off = len(blob)
    blob += note
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type = struct.unpack_from("<I", blob, off)[0]
        if p_type == 4:  # PT_NOTE
            struct.pack_into("<IIQQQQQQ", blob, off,
                             4, 0, note_off, 0xFFFFFFFF81C00000,
                             0, len(note), len(note), 4)
            return blob
    raise SystemExit("no PT_NOTE segment found for PVH note")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="only verify an existing vmlinux.patched")
    args = ap.parse_args()
    target = OUT if args.check else SRC
    blob = target.read_bytes()
    off = vaddr_to_offset(blob, TABLE_ENTRY)
    current = blob[off:off + 32]
    if args.check:
        if current != COMPAT:
            print(f"FAIL {target}: table is {current.hex()}")
            return 1
        print(f"OK {target}: SDHCI table entry already patched at {off:#x}")
        return 0
    validate_output(OUT, (SRC, VM / 'bzImage', VM / 'rootfs.img'))
    if current != EXPECTED:
        print(f"FAIL {SRC}: table bytes differ at {off:#x}\n"
              f"expected {EXPECTED.hex()}\nactual   {current.hex()}")
        return 1
    patched = bytearray(blob)
    patched[off:off + 32] = COMPAT
    patched = add_pvh_note(patched)
    OUT.write_bytes(patched)
    check = OUT.read_bytes()[off:off + 32]
    if check != COMPAT:
        print("FAIL: readback mismatch")
        return 1
    print(f"wrote {OUT} ({len(patched)} bytes), patched offset {off:#x}")
    print(f"  {EXPECTED.hex()} -> {COMPAT.hex()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
