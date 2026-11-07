# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Cross-process lock for heavy VM/regression runs."""
from __future__ import annotations

import fcntl
import time
from contextlib import contextmanager
from pathlib import Path


class LockTimeout(RuntimeError):
    pass


@contextmanager
def run_lock(vm_dir: Path, timeout: float = 600.0):
    """Exclusive lock at runs/.vm-lab.lock; serializes competing suites."""
    path = Path(vm_dir) / "runs" / ".vm-lab.lock"
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = open(path, "w")
    deadline = time.monotonic() + timeout
    try:
        while True:
            try:
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() > deadline:
                    raise LockTimeout(
                        f"another VM suite holds {path}; wait or use --no-lock")
                time.sleep(0.25)
        yield
    finally:
        try:
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        finally:
            handle.close()
