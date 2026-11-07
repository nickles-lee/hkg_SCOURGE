# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Safe image outputs and explicit disposable QEMU overlays."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import stat
import subprocess


def validate_output(path: Path, protected: tuple[Path, ...] = ()) -> Path:
    """Only new local regular files are allowed, including for --force callers."""
    if path.is_symlink():
        raise ValueError(f"refusing symlink output: {path}")
    resolved = path.resolve()
    if resolved == Path('/mnt/shares') or Path('/mnt/shares') in resolved.parents:
        raise ValueError(f"outputs must stay off the shared backing-image tree: {path}")
    if any(resolved == item.resolve() for item in protected):
        raise ValueError(f"refusing protected input/output alias: {path}")
    if path.exists():
        raise ValueError(f"output already exists; choose a new path: {path}")
    if not resolved.parent.is_dir():
        raise ValueError(f"output directory does not exist: {resolved.parent}")
    return resolved


def reserve_outputs(paths: list[Path], protected: tuple[Path, ...] = ()) -> None:
    resolved = [validate_output(path, protected) for path in paths]
    if len(set(resolved)) != len(resolved):
        raise ValueError("output paths alias one another")
    created = []
    try:
        for path in resolved:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o644)
            os.close(fd)
            created.append(path)
    except BaseException:
        for path in created:
            path.unlink()
        raise


def drive_path(path: Path) -> str:
    value = str(path.resolve())
    if any(char in value for char in ',\r\n\x00'):
        raise ValueError(f"unsupported QEMU drive path: {path!s}")
    return value


def create_overlays(run_dir: Path, inputs: dict[str, Path]) -> dict[str, dict]:
    overlays = {}
    for name, source in inputs.items():
        source = source.resolve(strict=True)
        if not stat.S_ISREG(source.stat().st_mode):
            raise ValueError(f"backing image is not a regular file: {source}")
        drive_path(source)
        output = validate_output(run_dir / f"{name}.qcow2", tuple(inputs.values()))
        try:
            subprocess.run(['qemu-img', 'create', '-q', '-f', 'qcow2', '-F', 'raw',
                            '-b', str(source), str(output)], check=True)
        except BaseException:
            if not output.is_symlink():
                output.unlink(missing_ok=True)
            raise
        overlays[name] = {'path': str(output), 'backing': str(source), 'format': 'qcow2'}
    return overlays


def cleanup_overlays(run_dir: Path, overlays: dict[str, dict]) -> None:
    """Only unlink the exact overlay files owned by this run, never their backings."""
    for name, item in overlays.items():
        path = Path(item['path'])
        if path != run_dir / f"{name}.qcow2" or path.is_symlink():
            raise ValueError(f"refusing unexpected overlay cleanup: {path}")
        path.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['reserve'])
    parser.add_argument('paths', type=Path, nargs='+')
    args = parser.parse_args()
    vm = Path(__file__).resolve().parent.parent
    protected = tuple(vm / name for name in ('rootfs.img', 'bzImage', 'vmlinux',
                                             'tools.img', 'update-k.img'))
    try:
        reserve_outputs(args.paths, protected)
    except (ValueError, OSError) as exc:
        parser.exit(1, f"image safety: {exc}\n")


if __name__ == '__main__':
    main()
