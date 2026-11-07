#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Prepare a guest-only legacy gdbserver alongside the small EGL payload."""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import urllib.request

from harness.images import validate_output

URL = ('https://old-releases.ubuntu.com/ubuntu/pool/main/g/gdb/'
       'gdbserver_7.11.90.20161005-0ubuntu2_amd64.deb')
SHA256 = 'df01abbcab8d663d967169fb80a5cfe2bc3e436e3781aee144fe9d8d3f25a503'
ELF_SHA256 = '4bcd28d0fa5a923eea17733b5ed57cedb356cbc97a003f5f76e5b8351f255722'


def validate_graphics(source: Path):
    if {p.name for p in source.iterdir()} != {'runtime', 'GRAPHICS_RUNTIME'}:
        raise ValueError('expected only the small prepared graphics payload')
    if (source / 'runtime').is_symlink() or not (source / 'runtime').is_dir():
        raise ValueError('runtime must be a real directory')
    marker = source / 'GRAPHICS_RUNTIME'
    if marker.is_symlink() or not marker.is_file() or marker.stat().st_size > 512:
        raise ValueError('invalid graphics payload marker')
    total = 0
    entries = list((source / 'runtime').iterdir())
    if len(entries) > 512:
        raise ValueError('too many runtime entries')
    for path in entries:
        if path.is_symlink():
            target = str(path.readlink())
            if '/' in target or target in ('.', '..') or not (path.parent / target).is_file() or (path.parent / target).is_symlink():
                raise ValueError('runtime links must target a sibling regular file')
        elif not path.is_file() or path.stat().st_size > 16 * 1024 ** 2:
            raise ValueError('unexpected runtime entry')
        else:
            total += path.stat().st_size
    if total > 64 * 1024 ** 2:
        raise ValueError('graphics payload is too large')


def symbol_links(rootfs: Path, inventory: Path, output: Path) -> int:
    """Local SONAME aliases for extracted trees that omit original symlinks."""
    with inventory.open() as file:
        rows = list(csv.DictReader(file))
    output.mkdir()
    count = 0
    for row in sorted(rows, key=lambda r: (not r['path'].startswith('lib/'), not r['path'].startswith('usr/lib/'))):
        path = PurePosixPath(row['path'])
        name = row['soname']
        if (row['root'] != 'rootfs' or not name or '/' in name or name in ('.', '..') or
                path.is_absolute() or '..' in path.parts):
            continue
        link = output / name
        if link.is_symlink():
            continue
        link.symlink_to(rootfs / row['path'])
        count += 1
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--graphics', type=Path, required=True, help='prepared small graphics payload directory')
    parser.add_argument('--cache', type=Path, help='previously fetched package, read-only')
    parser.add_argument('--rootfs', type=Path, help='optional extracted tree for local symbol aliases; no binary execution')
    parser.add_argument('--inventory', type=Path, help='matching binaries.csv required with --rootfs')
    args = parser.parse_args()
    output = validate_output(args.output)
    if bool(args.rootfs) != bool(args.inventory):
        parser.error('--rootfs and --inventory must be supplied together')
    validate_graphics(args.graphics)
    if args.cache:
        if args.cache.stat().st_size > 1024 * 1024:
            raise ValueError('cached package is too large')
        content = args.cache.read_bytes()
    else:
        with urllib.request.urlopen(URL, timeout=30) as response:
            content = response.read(1024 * 1024 + 1)
    if len(content) > 1024 * 1024 or hashlib.sha256(content).hexdigest() != SHA256:
        raise ValueError('gdbserver package digest/size mismatch')
    output.mkdir()
    package = output / 'gdbserver.deb'
    package.write_bytes(content)
    members = subprocess.check_output(['ar', 't', str(package)], text=True, timeout=30).splitlines()
    data = [m for m in members if m in ('data.tar.gz', 'data.tar.xz')]
    if len(data) != 1:
        raise ValueError('expected one supported Debian data archive')
    raw = subprocess.check_output(['ar', 'p', str(package), data[0]], timeout=30)
    with tarfile.open(fileobj=io.BytesIO(raw)) as archive:
        selected = [m for m in archive if m.name.removeprefix('./') == 'usr/bin/gdbserver']
        if len(selected) != 1 or not selected[0].isfile() or selected[0].size > 1024 * 1024:
            raise ValueError('expected one small regular gdbserver ELF')
        binary = archive.extractfile(selected[0]).read()
    if hashlib.sha256(binary).hexdigest() != ELF_SHA256:
        raise ValueError('gdbserver ELF digest mismatch')
    # Only this already-prepared small runtime is copied, never a VM image or
    # firmware archive. Preserve library links rather than dereferencing them.
    shutil.copytree(args.graphics, output / 'payload', symlinks=True)
    executable = output / 'payload' / 'gdbserver'
    with executable.open('xb') as file:
        file.write(binary)
    executable.chmod(0o755)
    manifest = {'url': URL, 'package_sha256': SHA256, 'gdbserver_sha256': ELF_SHA256,
                'trust': 'Observed HTTPS bytes pinned; release signatures not independently authenticated',
                'purpose': 'Guest-only debugger; target execution remains inside QEMU'}
    if args.rootfs:
        manifest['symbol_links'] = symbol_links(args.rootfs.resolve(), args.inventory, output / 'symbols')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
