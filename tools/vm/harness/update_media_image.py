# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Build a small synthetic update stick for preflight tests.

The image contains only lab bytes with the real filenames and package-map
grammar. It is not an OEM package and contains no credentials.
"""
from __future__ import annotations

import hashlib
import subprocess
import tempfile
import zlib
from pathlib import Path

VERSION = 'NE_E.EUR.S5W_L.001.001.260128'
PAYLOAD = Path('HU/images/mango-rootfs.tar.gz')
BLOB = Path('.lge.upgrade.xml')
VER = Path('IONIQ5_EU.ver')
MANIFEST = Path('MANIFEST.sha256')

#: Row ids as the OEM package uses them (docs/02): the rootfs and the wrapper are
#: the two rows SCOURGE rewrites, and the signed manifest has its own. The fixture
#: carried 341142 (the *wrapper* id) for the rootfs and a hardcoded 609028602 —
#: crc32 of the stock AppUpgrade — so the row described nothing that was in the
#: image. Derive it instead; a pinned CRC is what went stale in the real package.
ROW_ROOTFS = 341174
ROW_XML = 341175


def _digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _signed_crc32(data: bytes) -> int:
    """The `.ver` convention: 32-bit zlib CRC written as a signed integer."""
    value = zlib.crc32(data) & 0xFFFFFFFF
    return value - (1 << 32) if value >= 1 << 31 else value


def build_update_image(output: Path, size_mb: int = 32,
                       payload_size: int = 65536) -> dict:
    if not 16 <= size_mb <= 256:
        raise ValueError('size_mb must be between 16 and 256')
    if not 1024 <= payload_size <= 8 * 1024 * 1024:
        raise ValueError('payload_size must be between 1 KiB and 8 MiB')
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()
    payload = (bytes(range(256)) * (payload_size // 256 + 1))[:payload_size]
    blob = b'\x0f\x09\x21\x8f' + bytes(range(64))
    payload_crc, blob_crc = _signed_crc32(payload), _signed_crc32(blob)
    ver = (
        f'+|21393|{VERSION}|HM|IONIQ5_EU|1489|1\n'
        f'IONIQ5_EU\\HU\\images|mango-rootfs.tar.gz|{ROW_ROOTFS}|{payload_crc}|'
        f'{len(payload)}|1\n'
        f'IONIQ5_EU|.lge.upgrade.xml|{ROW_XML}|{blob_crc}|{len(blob)}|1\n'
    ).encode()
    manifest = (f'{_digest(ver)}  {VER.as_posix()}\n'
                f'{_digest(payload)}  {PAYLOAD.as_posix()}\n').encode()
    with tempfile.TemporaryDirectory(prefix='vm-update-media-') as tmp:
        root = Path(tmp)
        for relative, data in ((PAYLOAD, payload), (BLOB, blob), (VER, ver),
                               (MANIFEST, manifest)):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        completed = subprocess.run(
            ['mke2fs', '-q', '-t', 'ext4', '-b', '4096', '-I', '256',
             '-O', '^metadata_csum,^64bit,^metadata_csum_seed,^orphan_file',
             '-d', str(root), str(output), f'{size_mb}M'],
            capture_output=True, text=True)
        if completed.returncode != 0:
            raise RuntimeError(
                f'mke2fs failed: {completed.stderr.strip() or completed.stdout.strip()}')
    return {'output': str(output), 'size_mb': size_mb,
            'payload_bytes': len(payload), 'payload_sha256': _digest(payload),
            'version': VERSION, 'manifest_sha256': _digest(manifest),
            'rootfs_row_id': ROW_ROOTFS, 'rootfs_row_crc': payload_crc,
            'xml_row_id': ROW_XML, 'xml_row_crc': blob_crc}
