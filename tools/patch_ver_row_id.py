#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Patch one .ver row identified by its numeric ID.

Usage:
  patch_ver_row_id.py SRC.ver OUT.ver ROW_ID IMAGE [EXPECTED_NAME]

The CRC32 is computed over the file bytes (the encrypted container for TE2
files, the plain file for VR fixed/md5sum).  The size field is the file size.
This patcher has no 0x4024 minimum and does not match by filename, so it works
for 200-byte md5sum files and tiny VR fixed gzips.
"""
import os
import sys
import zlib


def crc32_file(path):
    c = 0
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''):
            c = zlib.crc32(b, c)
    return c & 0xffffffff


def signed32(v):
    return v - (1 << 32) if v >= (1 << 31) else v


def patch_row(src, out, row_id, image, expected_name=None):
    if os.path.exists(out):
        raise SystemExit('refusing to overwrite %s' % out)
    raw = open(src, encoding='utf-8', errors='surrogateescape').read()
    trailing = raw.endswith('\n')
    lines = raw.splitlines()
    crc = crc32_file(image)
    size = os.path.getsize(image)
    sid = str(row_id)
    found = 0
    for i, line in enumerate(lines):
        f = line.split('|')
        if len(f) >= 6 and f[2] == sid:
            if expected_name is not None and f[1] != expected_name:
                raise SystemExit('row %s name %r != expected %r'
                                 % (sid, f[1], expected_name))
            f[3] = str(signed32(crc))
            f[4] = str(size)
            lines[i] = '|'.join(f)
            found += 1
    if found != 1:
        raise SystemExit('expected one row ID %s, found %d' % (sid, found))
    with open(out, 'w', encoding='utf-8', errors='surrogateescape',
              newline='\n') as f:
        f.write('\n'.join(lines) + ('\n' if trailing else ''))
    row = [l for l in open(out, encoding='utf-8', errors='surrogateescape')
           .read().splitlines()
           if len(l.split('|')) >= 6 and l.split('|')[2] == sid][0]
    f = row.split('|')
    assert int(f[3]) == signed32(crc)
    assert int(f[4]) == size
    print('patched row %s: crc %s (0x%08x) size %d name %s'
          % (sid, f[3], crc, size, f[1]))
    return row


def main():
    if len(sys.argv) not in (5, 6):
        sys.exit(__doc__)
    src, out, rid, image = sys.argv[1:5]
    expected = sys.argv[5] if len(sys.argv) == 6 else None
    patch_row(src, out, rid, image, expected)


if __name__ == '__main__':
    main()
