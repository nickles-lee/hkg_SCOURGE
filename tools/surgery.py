#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""5W POC bundle: streaming tar-entry surgery + faithful gzip re-pack.

Replaces the payload of selected regular-file entries inside the decrypted
mango-rootfs tar.gz and writes a new single-member gzip. Everything else
(entry order, headers, modes, mtimes, padding, gzip header fields) is kept
byte-for-byte identical.

Usage:
  surgery.py SRC.gz DEST.gz --replace ./etc/update_start_5w.sh=FILE \
                            --replace ./etc/update_start_5w.sh.ori=FILE ...
  surgery.py --scan SRC.gz
"""
import argparse
import os
import struct
import sys
import zlib

BLOCK = 512


def parse_num(field):
    """Parse a tar numeric field (octal ASCII or GNU base-256)."""
    if field and (field[0] & 0x80):
        n = field[0] & 0x7f
        for b in field[1:]:
            n = (n << 8) | b
        return n
    raw = field.split(b'\x00', 1)[0].strip(b' \x00')
    if not raw:
        return 0
    try:
        return int(raw, 8)
    except ValueError:
        return 0


def tar_name(hdr):
    name = hdr[0:100].split(b'\x00', 1)[0]
    prefix = b''
    if hdr[257:262] == b'ustar':
        prefix = hdr[345:500].split(b'\x00', 1)[0]
    full = prefix + b'/' + name if prefix else name
    return full.decode('utf-8', 'surrogateescape')


def norm(name):
    while name.startswith('./'):
        name = name[2:]
    return name


def set_size_checksum(hdr, size):
    h = bytearray(hdr)
    h[124:136] = ('%011o' % size).encode('ascii') + b'\x00'
    h[148:156] = b' ' * 8
    chk = sum(h)
    h[148:156] = ('%06o' % chk).encode('ascii') + b'\x00 '
    return bytes(h)


def parse_pax(data):
    rec = {}
    i = 0
    while i < len(data):
        sp = data.find(b' ', i)
        if sp < 0:
            break
        try:
            ln = int(data[i:sp])
        except ValueError:
            break
        if ln <= 0:
            break
        entry = data[sp + 1:i + ln]
        if entry.endswith(b'\n'):
            entry = entry[:-1]
        k, _, v = entry.partition(b'=')
        rec[k.decode('utf-8', 'replace')] = v
        i += ln
    return rec


class GzTarStream:
    """Block reader over the first gzip member of a file."""

    def __init__(self, path):
        self.f = open(path, 'rb')
        head = self.f.read(64)
        if head[:3] != b'\x1f\x8b\x08':
            raise SystemExit('not a deflate gzip member: %s' % path)
        flg = head[3]
        off = 10
        if flg & 4:
            xlen = struct.unpack('<H', head[off:off + 2])[0]
            off += 2 + xlen
        if flg & 8:
            off = head.index(b'\x00', off) + 1
        if flg & 16:
            off = head.index(b'\x00', off) + 1
        if flg & 2:
            off += 2
        self.header = head[:off]
        self.f.seek(off)
        self.d = zlib.decompressobj(-15)
        self.buf = b''
        self.src_eof = False

    def _fill(self, want):
        while len(self.buf) < want and not self.src_eof:
            chunk = self.f.read(1 << 20)
            if not chunk:
                self.src_eof = True
                try:
                    self.buf += self.d.flush()
                except zlib.error:
                    pass
                break
            self.buf += self.d.decompress(chunk)
            if self.d.eof:
                self.src_eof = True
                if self.d.unused_data:
                    sys.stderr.write('WARN: %d trailing byte(s) after gzip '
                                     'member ignored\n'
                                     % len(self.d.unused_data))

    def read(self, n):
        self._fill(n)
        out = self.buf[:n]
        self.buf = self.buf[n:]
        return out


TARGET_NAMES = ('etc/update_start_5w.sh',
                'etc/update_start_5w.sh.ori',
                'etc/version.txt')


def scan(rd):
    n = 0
    total = 0
    types = {}
    while True:
        hdr = rd.read(BLOCK)
        if len(hdr) == 0:
            print('EOF after %d entries (no zero-block marker)' % n)
            break
        if len(hdr) < BLOCK:
            print('WARN: short trailing block (%d bytes)' % len(hdr))
            break
        if hdr == b'\x00' * BLOCK:
            extra = len(hdr)
            while True:
                chunk = rd.read(1 << 22)
                if not chunk:
                    break
                extra += len(chunk)
            print('end-of-archive marker at entry %d; %d extra zero-pad bytes'
                  % (n, extra))
            break
        name = tar_name(hdr)
        typ = hdr[156:157]
        size = parse_num(hdr[124:136])
        n += 1
        total += size
        types[typ] = types.get(typ, 0) + 1
        nm = norm(name)
        if nm in TARGET_NAMES or typ in (b'x', b'g'):
            print('entry %d: name=%r type=%r size=%d mode=%s mtime=%s'
                  % (n, name, typ, size,
                     hdr[100:108].rstrip(b'\x00').decode('ascii', 'replace'),
                     hdr[136:148].rstrip(b'\x00').decode('ascii', 'replace')))
        if typ in (b'x', b'g'):
            data = rd.read((size + BLOCK - 1) // BLOCK * BLOCK)[:size]
            rec = parse_pax(data)
            if rec:
                print('   pax: %s' % {k: v[:60] for k, v in rec.items()})
        else:
            skip = (size + BLOCK - 1) // BLOCK * BLOCK
            while skip > 0:
                chunk = rd.read(min(skip, 1 << 20))
                if not chunk:
                    break
                skip -= len(chunk)
    print('entries: %d' % n)
    print('sum of file sizes: %d' % total)
    print('typeflags: %s' % {k.decode('latin1'): v for k, v in types.items()})


def build(rd, dst_path, repl, level=6):
    dst = open(dst_path, 'wb')
    dst.write(rd.header)
    comp = zlib.compressobj(level, zlib.DEFLATED, -15)
    st = {'crc': 0, 'size': 0, 'entries': 0, 'replaced': []}

    def emit(b):
        if not b:
            return
        st['crc'] = zlib.crc32(b, st['crc'])
        st['size'] += len(b)
        out = comp.compress(b)
        if out:
            dst.write(out)

    pax_pending = None
    while True:
        hdr = rd.read(BLOCK)
        if len(hdr) == 0:
            sys.stderr.write('WARN: EOF without tar end marker\n')
            break
        if len(hdr) < BLOCK:
            sys.stderr.write('WARN: short block (%d bytes)\n' % len(hdr))
            emit(hdr)
            break
        if hdr == b'\x00' * BLOCK:
            emit(hdr)
            while True:
                chunk = rd.read(1 << 22)
                if not chunk:
                    break
                emit(chunk)
            break
        name = tar_name(hdr)
        typ = hdr[156:157]
        size = parse_num(hdr[124:136])
        eff_name = norm(name)
        eff_size = size
        if pax_pending is not None:
            if 'path' in pax_pending:
                eff_name = norm(pax_pending['path'].decode(
                    'utf-8', 'surrogateescape'))
            if 'size' in pax_pending:
                eff_size = int(pax_pending['size'])
            pax_pending = None
        stored = (eff_size + BLOCK - 1) // BLOCK * BLOCK

        if typ in (b'x', b'g'):
            data = rd.read(stored)[:eff_size]
            pax_pending = parse_pax(data)
            emit(hdr)
            emit(data)
            emit(b'\x00' * (stored - eff_size))
            st['entries'] += 1
            continue

        if eff_name in repl:
            if typ not in (b'0', b'\x00'):
                raise SystemExit('refusing to replace non-regular entry %r '
                                 '(typeflag %r)' % (eff_name, typ))
            newdata = repl[eff_name]
            emit(set_size_checksum(hdr, len(newdata)))
            emit(newdata)
            pad = (-len(newdata)) % BLOCK
            if pad:
                emit(b'\x00' * pad)
            # consume the original payload blocks
            remaining = stored
            while remaining > 0:
                chunk = rd.read(min(remaining, 1 << 20))
                if not chunk:
                    break
                remaining -= len(chunk)
            st['replaced'].append((name, size, len(newdata)))
            st['entries'] += 1
            continue

        emit(hdr)
        remaining = stored
        while remaining > 0:
            chunk = rd.read(min(remaining, 1 << 20))
            if not chunk:
                break
            remaining -= len(chunk)
            emit(chunk)
        st['entries'] += 1

    dst.write(comp.flush())
    dst.write(struct.pack('<I', st['crc'] & 0xffffffff))
    dst.write(struct.pack('<I', st['size'] & 0xffffffff))
    dst.close()
    print('entries: %d' % st['entries'])
    print('replaced: %s' % st['replaced'])
    print('uncompressed tar size: %d  crc32=%08x'
          % (st['size'], st['crc'] & 0xffffffff))
    print('wrote %s (%d bytes)' % (dst_path, os.path.getsize(dst_path)))
    return st


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst', nargs='?')
    ap.add_argument('--replace', action='append', default=[],
                    help='NAME=FILE payload replacement (NAME as in tar)')
    ap.add_argument('--scan', action='store_true')
    a = ap.parse_args()
    if not a.scan and not a.dst:
        ap.error('DEST is required unless --scan is used')

    repl = {}
    for spec in a.replace:
        name, path = spec.split('=', 1)
        with open(path, 'rb') as f:
            repl[norm(name)] = f.read()
        print('replacement: %s <- %s (%d bytes)'
              % (norm(name), path, len(repl[norm(name)])))

    rd = GzTarStream(a.src)
    if a.scan:
        scan(rd)
        return

    st = build(rd, a.dst, repl)

    done = {}
    for name, _old, _new in st['replaced']:
        key = norm(name)
        done[key] = done.get(key, 0) + 1
    missing = set(repl) - set(done)
    if missing:
        raise SystemExit('ERROR: replacement target(s) not found: %s'
                         % ', '.join(sorted(missing)))
    dupes = {k: v for k, v in done.items() if v != 1}
    if dupes:
        raise SystemExit('ERROR: target(s) replaced more than once: %s' % dupes)


if __name__ == '__main__':
    main()
