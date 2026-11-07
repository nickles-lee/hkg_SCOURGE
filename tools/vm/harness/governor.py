# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Cross-process lock for heavy VM/regression runs."""
from __future__ import annotations

import fcntl
import json
import math
import os
import time
from contextlib import contextmanager
from pathlib import Path


class LockTimeout(RuntimeError):
    pass


@contextmanager
def run_lock(vm_dir: Path, timeout: float = 600.0):
    """Exclusive lock at runs/.vm-lab.lock; serializes competing boots.

    Waits up to `timeout` seconds (the queue), then raises LockTimeout.
    Yields evidence (path, waited_seconds, pid) for result.json.
    """
    if not math.isfinite(timeout) or timeout < 0:
        raise ValueError("lock timeout must be finite and non-negative")
    path = Path(vm_dir) / "runs" / ".vm-lab.lock"
    path.parent.mkdir(parents=True, exist_ok=True)
    # Never truncate on open: a waiter would erase the current holder's
    # evidence while it is the most useful. Truncate only after acquiring.
    handle = os.fdopen(os.open(path, os.O_RDWR | os.O_CREAT, 0o600), "r+")
    started = time.monotonic()
    try:
        while True:
            try:
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError:
                if time.monotonic() - started > timeout:
                    raise LockTimeout(
                        f"another VM boot holds {path}; wait, stop the daemon, "
                        "or use --no-lock (unsafe)")
                time.sleep(0.25)
        info = {"held": True, "path": str(path), "pid": os.getpid(),
                "waited_seconds": round(time.monotonic() - started, 3)}
        handle.seek(0)
        handle.truncate()
        handle.write(json.dumps(info) + "\n")
        handle.flush()
        try:
            yield info
        finally:
            # Record release before unlocking, so a waiter cannot interleave
            # its holder record with our truncation.
            handle.seek(0)
            handle.truncate()
            handle.write(json.dumps({"held": False, "pid": os.getpid(),
                                     "released": True}) + "\n")
            handle.flush()
    finally:
        try:
            fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        finally:
            handle.close()
