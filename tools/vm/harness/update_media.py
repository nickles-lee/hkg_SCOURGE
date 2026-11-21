# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Validate an offline update stick without executing target code.

The real media carries an encrypted ``.lge.upgrade.xml``, a pipe-delimited
``IONIQ5_EU.ver`` package map and a ``MANIFEST.sha256``.  This module checks
the package-map grammar, path confinement, manifest coverage and optional
hashes.  It does not decrypt, extract or run the updater.
"""
from __future__ import annotations

import hashlib
import re
import subprocess
from pathlib import Path, PurePosixPath

HEX = frozenset('0123456789abcdef')
VER_HEADER_FIELDS = 7
VER_ENTRY_FIELDS = 6
MAX_TEXT_BYTES = 2 * 1024 * 1024
SAFE_PATH = re.compile(r'^[A-Za-z0-9._/+@(),-]+$')


class MediaError(ValueError):
    """The media cannot be read or is not a directory/image."""


def _normalize(rel: str) -> str:
    rel = rel.replace('\\', '/')
    return rel[2:] if rel.startswith('./') else rel


def _safe_posix(rel: str) -> tuple[str | None, str | None]:
    """Return (normalized, problem) for a confinement-checked path."""
    rel = _normalize(rel.strip())
    if not rel:
        return None, 'empty path'
    if rel.startswith('/'):
        return None, f'absolute path: {rel}'
    parts = PurePosixPath(rel).parts
    if not parts or any(part in ('', '.', '..') for part in parts):
        return None, f'path traversal or empty segment: {rel}'
    if '\x00' in rel or '\n' in rel or '\r' in rel:
        return None, 'control character in path'
    return rel, None


def _safe_ver_path(path: str, product: str) -> tuple[str | None, str | None]:
    if '..' in path.split('\\') or path.startswith(('/', '\\')):
        return None, f'unsafe package path: {path}'
    parts = path.split('\\')
    if not parts or parts[0] != product:
        return None, f'package path does not start with {product}: {path}'
    rel = '/'.join(parts[1:])
    if not rel:
        return '', None
    return _safe_posix(rel)


def parse_ver(text: str) -> dict:
    """Parse IONIQ5_EU.ver; return header, entries and problems."""
    lines = text.splitlines()
    problems: list[str] = []
    warnings: list[str] = []
    header: dict = {}
    entries: list[dict] = []
    if not lines:
        return {'header': header, 'entries': entries,
                'problems': ['empty package map'], 'warnings': warnings}
    fields = lines[0].split('|')
    if len(fields) != VER_HEADER_FIELDS or fields[0] != '+':
        problems.append('package map header must be +|package-id|version|'
                        'brand|product|number|flag')
    else:
        try:
            package_id = int(fields[1])
        except ValueError:
            package_id = None
            problems.append(f'package map header id is not an integer: {fields[1]}')
        for label, value in (('package', fields[5]), ('flag', fields[6])):
            try:
                int(value)
            except ValueError:
                problems.append(f'package map header {label} is not an integer: {value}')
        header = {'package_id': package_id, 'version': fields[2],
                  'brand': fields[3], 'product': fields[4]}
        if not all(header[key] for key in ('version', 'brand', 'product')):
            problems.append('package map header has an empty version/brand/product')
    product = str(header.get('product') or '')
    seen: set[tuple[str, str]] = set()
    for lineno, line in enumerate(lines[1:], 2):
        if not line.strip():
            warnings.append(f'package map line {lineno} is blank')
            continue
        parts = line.split('|')
        if len(parts) != VER_ENTRY_FIELDS:
            problems.append(f'package map line {lineno} has {len(parts)} fields, '
                            f'expected {VER_ENTRY_FIELDS}')
            continue
        path, filename, ident, check, size, flag = parts
        for label, value in (('id', ident), ('flag', flag)):
            try:
                int(value)
            except ValueError:
                problems.append(f'package map line {lineno} {label} is not an integer')
        try:
            declared = int(size)
            if declared < 0:
                raise ValueError
        except ValueError:
            problems.append(f'package map line {lineno} size is not a non-negative integer')
            declared = None
        if not filename or '/' in filename or '\\' in filename or filename in ('.', '..'):
            problems.append(f'package map line {lineno} has an unsafe filename: {filename!r}')
        if product:
            rel, problem = _safe_ver_path(path, product)
            if problem:
                problems.append(f'package map line {lineno}: {problem}')
                rel = None
        else:
            rel = None
        key = (path, filename)
        if key in seen:
            problems.append(f'package map line {lineno} duplicates {path}|{filename}')
        seen.add(key)
        relative = (f'{rel}/{filename}' if rel else filename) \
            if rel is not None else None
        entries.append({'line': lineno, 'path': path, 'filename': filename,
                        'relative': relative, 'crc': check,
                        'declared_size': declared, 'id': ident, 'flag': flag})
    if not entries:
        problems.append('package map has no entries')
    return {'header': header, 'entries': entries,
            'problems': problems, 'warnings': warnings}


def parse_manifest(text: str) -> dict:
    """Parse sha256sum output with confinement and duplicate checks."""
    entries: list[tuple[str, str]] = []
    problems: list[str] = []
    warnings: list[str] = []
    seen: set[str] = set()
    for lineno, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split(None, 1)
        if (len(parts) != 2 or len(parts[0]) != 64
                or any(char not in HEX for char in parts[0].lower())):
            problems.append(f'MANIFEST.sha256:{lineno}: malformed sha256 entry')
            continue
        rel = parts[1].strip()
        if rel.startswith('*'):
            rel = rel[1:]
        normalized, problem = _safe_posix(rel)
        if problem:
            problems.append(f'MANIFEST.sha256:{lineno}: {problem}')
            continue
        assert normalized is not None
        if normalized in seen:
            problems.append(f'MANIFEST.sha256:{lineno}: duplicate path: {normalized}')
            continue
        seen.add(normalized)
        entries.append((parts[0].lower(), normalized))
    if not entries:
        problems.append('MANIFEST.sha256 has no entries')
    return {'entries': entries, 'problems': problems, 'warnings': warnings}


class DirMedia:
    mode = 'dir'

    def __init__(self, root: Path) -> None:
        self.root = Path(root).resolve()
        if not self.root.is_dir():
            raise MediaError(f'not a directory: {root}')

    def _path(self, rel: str) -> Path:
        candidate = (self.root / rel).resolve()
        if candidate != self.root and self.root not in candidate.parents:
            raise MediaError(f'path escapes media root: {rel}')
        return candidate

    def read_text(self, rel: str) -> str | None:
        path = self._path(rel)
        if not path.is_file():
            return None
        if path.stat().st_size > MAX_TEXT_BYTES:
            raise MediaError(f'{rel} is larger than {MAX_TEXT_BYTES} bytes')
        return path.read_text(errors='replace')

    def exists(self, rel: str) -> bool:
        return self._path(rel).is_file()

    def size(self, rel: str) -> int | None:
        path = self._path(rel)
        return path.stat().st_size if path.is_file() else None

    def sha256(self, rel: str) -> str:
        path = self._path(rel)
        digest = hashlib.sha256()
        with path.open('rb') as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b''):
                digest.update(chunk)
        return digest.hexdigest()


class ImageMedia:
    mode = 'image'

    def __init__(self, image: Path) -> None:
        self.image = Path(image).resolve()
        if not self.image.is_file():
            raise MediaError(f'not a file: {image}')
        if not subprocess.run(['debugfs', '-V'], capture_output=True).returncode == 0:
            raise MediaError('debugfs is required for image mode')

    def _debugfs(self, command: str) -> bytes:
        completed = subprocess.run(
            ['debugfs', '-R', command, str(self.image)],
            capture_output=True, check=False)
        if completed.returncode != 0:
            raise MediaError(f'debugfs failed for {command}: '
                             f'{completed.stderr.decode(errors="replace").strip()}')
        return completed.stdout

    def read_text(self, rel: str) -> str | None:
        if not SAFE_PATH.match(rel):
            raise MediaError(f'unsafe image path: {rel}')
        if not self.exists(rel):
            return None
        data = self._debugfs(f'cat /{rel}')
        if len(data) > MAX_TEXT_BYTES:
            raise MediaError(f'{rel} is larger than {MAX_TEXT_BYTES} bytes')
        return data.decode(errors='replace')

    def exists(self, rel: str) -> bool:
        if not SAFE_PATH.match(rel):
            raise MediaError(f'unsafe image path: {rel}')
        out = self._debugfs(f'stat /{rel}')
        return b'Inode:' in out

    def size(self, rel: str) -> int | None:
        if not SAFE_PATH.match(rel):
            raise MediaError(f'unsafe image path: {rel}')
        out = self._debugfs(f'stat /{rel}')
        if b'Inode:' not in out:
            return None
        match = re.search(rb'Size: (\d+)', out)
        return int(match.group(1)) if match else None

    def sha256(self, rel: str) -> str:
        if not SAFE_PATH.match(rel):
            raise MediaError(f'unsafe image path: {rel}')
        proc = subprocess.Popen(
            ['debugfs', '-R', f'cat /{rel}', str(self.image)],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        digest = hashlib.sha256()
        assert proc.stdout is not None
        try:
            for chunk in iter(lambda: proc.stdout.read(1024 * 1024), b''):
                digest.update(chunk)
        finally:
            proc.stdout.close()
        if proc.wait() != 0:
            raise MediaError(f'debugfs could not hash {rel}')
        return digest.hexdigest()


def open_media(path: Path):
    path = Path(path)
    return DirMedia(path) if path.is_dir() else ImageMedia(path)


def check_update_media(path: Path, verify_hashes: bool = False,
                       verify_files: bool = False) -> dict:
    """Check update media layout, package map and manifest; never execute it."""
    root = Path(path)
    report = {
        'ok': False, 'source': str(root), 'mode': None, 'header': {},
        'entries': 0, 'manifest_entries': 0, 'present_entries': 0,
        'hashes_verified': 0, 'missing_manifest_targets': [],
        'mismatched': [], 'problems': [], 'warnings': [],
    }
    try:
        media = open_media(root)
    except (MediaError, OSError) as exc:
        report['problems'].append(str(exc))
        return report
    report['mode'] = media.mode
    problems: list[str] = report['problems']
    warnings: list[str] = report['warnings']
    try:
        for required in ('.lge.upgrade.xml', 'IONIQ5_EU.ver', 'MANIFEST.sha256'):
            if not media.exists(required):
                problems.append(f'missing required file: {required}')
        ver_present = media.exists('IONIQ5_EU.ver')
    except (MediaError, OSError) as exc:
        problems.append(str(exc))
        return report
    if not ver_present:
        return report
    try:
        ver_text = media.read_text('IONIQ5_EU.ver')
        manifest_text = media.read_text('MANIFEST.sha256')
    except (MediaError, OSError, UnicodeError) as exc:
        problems.append(str(exc))
        return report
    ver = parse_ver(ver_text or '')
    manifest = parse_manifest(manifest_text or '')
    problems.extend(ver['problems'])
    problems.extend(manifest['problems'])
    warnings.extend(ver['warnings'])
    warnings.extend(manifest['warnings'])
    report['header'] = ver['header']
    report['entries'] = len(ver['entries'])
    report['manifest_entries'] = len(manifest['entries'])

    targets = [rel for _, rel in manifest['entries']]
    for rel in targets:
        try:
            if not media.exists(rel):
                report['missing_manifest_targets'].append(rel)
        except (MediaError, OSError) as exc:
            problems.append(str(exc))
    if 'IONIQ5_EU.ver' not in targets:
        problems.append('MANIFEST.sha256 does not cover IONIQ5_EU.ver')

    if verify_hashes:
        for expected, rel in manifest['entries']:
            if rel in report['missing_manifest_targets']:
                continue
            try:
                actual = media.sha256(rel)
            except (MediaError, OSError) as exc:
                problems.append(str(exc))
                continue
            report['hashes_verified'] += 1
            if actual != expected:
                report['mismatched'].append(rel)

    if verify_files:
        for entry in ver['entries']:
            rel = entry['relative']
            if rel is None:
                continue
            try:
                size = media.size(rel)
            except (MediaError, OSError) as exc:
                problems.append(str(exc))
                continue
            if size is None:
                continue
            report['present_entries'] += 1
            if entry['declared_size'] is not None and size != entry['declared_size']:
                problems.append(f'size mismatch: {rel} declared {entry["declared_size"]} '
                                f'actual {size}')

    report['ok'] = (not problems and not report['missing_manifest_targets']
                    and not report['mismatched'])
    return report
