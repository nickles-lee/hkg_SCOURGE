#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Fetch/extract pinned guest-only Mesa dependencies; never install or execute them."""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import urllib.request

from harness.images import validate_output

BASE = 'https://old-releases.ubuntu.com/ubuntu/pool/main/'
PACKAGES = [('libx/libxcb', f'{name}_1.11.1-1ubuntu1_amd64.deb') for name in
            ('libxcb1', 'libxcb-dri2-0', 'libxcb-dri3-0', 'libxcb-present0', 'libxcb-xfixes0', 'libxcb-sync1')]
PACKAGES += [('libx/libx11', 'libx11-xcb1_1.6.3-1ubuntu3_amd64.deb'),
             ('libx/libxshmfence', 'libxshmfence1_1.2-1_amd64.deb'),
             ('libx/libxau', 'libxau6_1.0.8-1_amd64.deb'),
             ('libx/libxdmcp', 'libxdmcp6_1.1.2-1.1_amd64.deb'),
             ('libe/libedit', 'libedit2_3.1-20150325-1ubuntu2_amd64.deb'),
             ('n/ncurses', 'libtinfo5_6.0+20160625-1ubuntu1_amd64.deb'),
             ('libb/libbsd', 'libbsd0_0.8.3-1_amd64.deb')]

# Pins reproduce our observed fetch, not an independently authenticated Ubuntu
# release. A cache entry must match too; its filename is not an integrity check.
SHA256 = {
    'libxcb1': '74feef1455379608484266ad3dd9d6d2419fed7e9fba34625c04012401fc9069',
    'libxcb-dri2-0': '06a23591ce1a7c50b4439b050f4c4789f827798bd4026f193bdccf8c796b6273',
    'libxcb-dri3-0': 'e6a6d18e2ac82ab051a4c96c59f14bd2aa666e546e1eea0488c175284746cf56',
    'libxcb-present0': 'abf3e72f1233959b3850444f2b6a9adf78249509ad87edb4794aa92953e74248',
    'libxcb-xfixes0': '269a14977923052ee94f448a22c311764b69138d39b6b42f89ec2e6092ba758e',
    'libxcb-sync1': '6e556fdd6221ec403f0eeb2d3b733ef0ea6de11e8d7a58bc3ecd1f4f31b90feb',
    'libx11-xcb1': '3476b02f22956a81d4a427e3d8900bdaee5ff4542377584893f33cd85afbf9d8',
    'libxshmfence1': '862c3555c9dede712187f495aa00e7e1491b17b22402c87ed4ebd2675c9e227a',
    'libxau6': '881544ee71d85e89ebf224e45acc94930f6a309da2fb33dbf850bd1434e70597',
    'libxdmcp6': '4219784f39ab5b10d4d83789b4822b56c6926055bcad655fae5312a9d1885c2b',
    'libedit2': '37f94bdfdd82537058765b327d461b1cfd50a666cb41f99d6a4da1a85252e0e3',
    'libtinfo5': '5c30efa64b3d18dcb34779b0d4f8c33e9fca3eea66dd3e7de492aa772d81d751',
    'libbsd0': '0b99e51ebd627cca37cdb1d9a46bb5ccd78f839130fb1639549e79c8f1267614',
}


def verify_package(filename: str, content: bytes) -> str:
    digest = hashlib.sha256(content).hexdigest()
    if digest != SHA256[filename.split('_', 1)[0]]:
        raise ValueError(f'package digest mismatch: {filename}')
    return digest


def extract_libraries(deb: Path, output: Path) -> list[str]:
    members = subprocess.run(['ar', 't', str(deb)], check=True, capture_output=True, text=True, timeout=30).stdout.splitlines()
    data = [name for name in members if re.fullmatch(r'data\.tar(?:\.(?:xz|gz|bz2))?', name)]
    if len(data) != 1:
        raise ValueError(f'unsupported data archive: {deb}')
    payload = subprocess.run(['ar', 'p', str(deb), data[0]], check=True, capture_output=True, timeout=30).stdout
    names = []
    links = []
    selected_bytes = 0
    with tarfile.open(fileobj=io.BytesIO(payload), mode='r:*') as archive:
        for item in archive:
            path = PurePosixPath(item.name.removeprefix('./'))
            if path.parent.as_posix() not in ('usr/lib/x86_64-linux-gnu', 'lib/x86_64-linux-gnu'):
                continue
            name = path.name
            if not re.fullmatch(r'lib[A-Za-z0-9_.+-]+\.so(?:\.[0-9.]+)?', name):
                continue
            if name in names:
                raise ValueError(f'duplicate library member: {name}')
            selected_bytes += item.size
            if len(names) >= 512 or selected_bytes > 64 * 1024 ** 2:
                raise ValueError('runtime library extraction limit exceeded')
            target = output / name
            if item.issym():
                if '/' in item.linkname or not re.fullmatch(r'lib[A-Za-z0-9_.+-]+\.so(?:\.[0-9.]+)?', item.linkname):
                    raise ValueError(f'unsafe library link: {name}')
                links.append((target, item.linkname))
            elif item.isfile() and item.size <= 16 * 1024 ** 2:
                stream = archive.extractfile(item)
                if stream is None:
                    raise ValueError(f'unreadable library: {name}')
                content = stream.read()
                if target.exists() or target.is_symlink():
                    raise ValueError(f'library collision: {target}')
                with target.open('xb') as file:
                    file.write(content)
            else:
                raise ValueError(f'unsupported library member: {name}')
            names.append(name)
    for target, link in links:
        if not (output / link).is_file():
            raise ValueError(f'unresolved library link: {target.name} -> {link}')
        target.symlink_to(link)
    if not names:
        raise ValueError(f'no runtime libraries extracted: {deb}')
    return names


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='new local bundle directory')
    parser.add_argument('--cache', type=Path, help='reuse an existing package cache without modifying it')
    args = parser.parse_args()
    output = validate_output(args.output)
    output.mkdir()
    packages = output / 'packages'
    libraries = output / 'payload' / 'runtime'
    packages.mkdir()
    libraries.mkdir(parents=True)
    manifest = {'purpose': 'guest-only software EGL dependency fixtures',
                'trust': 'HTTPS official archive; hashes record fetched bytes, release signatures not independently verified',
                'packages': []}
    for directory, filename in PACKAGES:
        url = BASE + directory + '/' + filename
        cached = args.cache / filename if args.cache else None
        if cached is not None and cached.is_file():
            if cached.stat().st_size > 8 * 1024 ** 2:
                raise ValueError(f'oversized cached package: {cached}')
            content = cached.read_bytes()
        else:
            with urllib.request.urlopen(url, timeout=30) as response:
                content = response.read(8 * 1024 ** 2 + 1)
        if len(content) > 8 * 1024 ** 2 or not content.startswith(b'!<arch>\n'):
            raise ValueError(f'invalid or oversized package: {url}')
        digest = verify_package(filename, content)
        deb = packages / filename
        deb.write_bytes(content)
        names = extract_libraries(deb, libraries)
        manifest['packages'].append({'url': url, 'sha256': digest, 'libraries': names})
        print(f'{filename}: {len(content)} bytes; extracted {len(names)} library entries', flush=True)
    (output / 'payload' / 'GRAPHICS_RUNTIME').write_text('guest-only software EGL dependencies v1\n')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
