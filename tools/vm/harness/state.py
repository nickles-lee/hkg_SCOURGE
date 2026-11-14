# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Persistent qcow2 overlays for fast reboot/install iteration."""
from __future__ import annotations

import fcntl
import json
import subprocess
from pathlib import Path

from .images import validate_output


class StateBusy(RuntimeError):
    pass


def _qemu_img_info(path: Path) -> dict:
    out = subprocess.run(
        ["qemu-img", "info", "--output=json", str(path)],
        capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


def create_state_overlays(state_dir: Path, inputs: dict[str, Path],
                          reset: bool = False,
                          mode: str | None = None) -> tuple[dict[str, dict], object]:
    """Create/reuse persistent overlays. The returned handle holds the lock.

    When `mode` is given (vendor/compat), a reused state must have been created
    in the same mode; switching modes requires `--reset`.
    """
    state_dir = Path(state_dir).resolve()
    state_dir.mkdir(parents=True, exist_ok=True)
    lock_path = state_dir / "state.lock"
    lock = open(lock_path, "w")
    try:
        fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exc:
        lock.close()
        raise StateBusy(f"state is already in use: {state_dir}") from exc

    metadata_path = state_dir / "state.json"
    previous_mode = None
    if metadata_path.is_file():
        try:
            previous = json.loads(metadata_path.read_text())
            if isinstance(previous, dict):
                previous_mode = previous.get("mode")
        except (OSError, json.JSONDecodeError):
            previous_mode = None
    if mode is not None and not reset and previous_mode not in (None, mode):
        lock.close()
        raise ValueError(
            f"state mode mismatch: {previous_mode!r} != {mode!r}; use --reset")
    overlays: dict[str, dict] = {}
    try:
        for name, source in inputs.items():
            source = Path(source).resolve(strict=True)
            destination = state_dir / f"{name}.qcow2"
            if reset or not destination.is_file():
                destination.unlink(missing_ok=True)
                validate_output(destination)
                subprocess.run(
                    ["qemu-img", "create", "-f", "qcow2", "-F", "raw",
                     "-b", str(source), str(destination)],
                    check=True, capture_output=True)
            info = _qemu_img_info(destination)
            backing = Path(info.get("backing-filename", ""))
            if not backing or backing.resolve() != source:
                raise RuntimeError(
                    f"state overlay backing mismatch for {name}: {backing} != {source}")
            overlays[name] = {
                "path": str(destination),
                "backing": str(source),
                "persistent": True,
                "state_dir": str(state_dir),
            }
        if mode is not None:
            metadata_path.write_text(json.dumps(
                {"mode": mode, "overlays": sorted(inputs)}, indent=2) + "\n")
        return overlays, lock
    except BaseException:
        lock.close()
        raise
