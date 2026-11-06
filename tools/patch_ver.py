#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Patch the mango-rootfs row (crc32 + stored size) in IONIQ5_EU.ver.

Usage:
  patch_ver.py STOCK.ver CONTAINER OUT.ver [NEW_VERSION]

NEW_VERSION, when given, also replaces the version field of the header line
(used by variant B, e.g. NE_E.EUR.S5W_L.001.001.260129).
"""
import os
import sys
import zlib


def crc32_file(path):
    c = 0
    with open(path, 'rb') as f:
        while True:
            b = f.read(1 << 22)
            if not b:
                break
            c = zlib.crc32(b, c)
    return c & 0xffffffff


def signed32(v):
    return v - (1 << 32) if v >= (1 << 31) else v


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(__doc__)
    src, img, out = sys.argv[1:4]
    newver = sys.argv[4] if len(sys.argv) == 5 else None

    raw = open(src, 'rb').read()
    trailing_nl = raw.endswith(b'\n')
    lines = raw.decode('utf-8').splitlines()

    crc = crc32_file(img)
    size = os.path.getsize(img)
    assert size > 0x4024, 'container too small'

    if newver:
        fields = lines[0].split('|')
        assert fields[0] == '+' and len(fields) >= 7, lines[0]
        fields[2] = newver
        lines[0] = '|'.join(fields)

    found = 0
    for i, line in enumerate(lines):
        f = line.split('|')
        if len(f) >= 6 and f[0].endswith('HU\\images') \
                and f[1] == 'mango-rootfs.tar.gz':
            f[3] = str(signed32(crc))
            f[4] = str(size)
            lines[i] = '|'.join(f)
            found += 1
    assert found == 1, 'expected exactly one mango-rootfs row, found %d' % found

    text = '\n'.join(lines) + ('\n' if trailing_nl else '')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)

    # re-parse and verify
    rows = [l.split('|') for l in open(out, encoding='utf-8').read().splitlines()
            if l.split('|')[1:2] == ['mango-rootfs.tar.gz']]
    assert len(rows) == 1
    row = rows[0]
    assert int(row[3]) == signed32(crc), (row[3], signed32(crc))
    assert int(row[4]) == size, (row[4], size)
    print('patched %s' % out)
    print('  header : %s' % lines[0])
    print('  row    : %s' % '|'.join(row))
    print('  crc    : %d (0x%08x)  size: %d' % (crc, crc, size))


if __name__ == '__main__':
    main()
