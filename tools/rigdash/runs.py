# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Run registry: start a step, stream its output, keep the tail.

Commands are plain argv lists executed from the repo root. Nothing is passed
through a shell, and the only value ever fed to a child's stdin is a sudo
password (never echoed, never logged).
"""
from __future__ import annotations

import itertools
import subprocess
import threading
import time
from dataclasses import dataclass, field

MAX_LINES = 5000


@dataclass
class Run:
    id: str
    step_id: str
    argv: list[str] = field(default_factory=list)
    state: str = "running"            # running | ok | failed | error
    lines: list[str] = field(default_factory=list)
    started: float = field(default_factory=time.time)
    finished: float | None = None
    exit_code: int | None = None
    plan: list[tuple[list[str], str | None]] = field(default_factory=list)

    def as_dict(self, limit: int = 400) -> dict:
        return {
            "id": self.id,
            "step_id": self.step_id,
            "argv": self.argv,
            "state": self.state,
            "exit_code": self.exit_code,
            "started": self.started,
            "finished": self.finished,
            "lines": self.lines[-limit:],
            "line_count": len(self.lines),
        }


class Registry:
    def __init__(self) -> None:
        self._runs: dict[str, Run] = {}
        self._order: list[str] = []
        self._lock = threading.Lock()
        self._ids = itertools.count(1)

    def start(self, step_id: str, plan: list[tuple[list[str], str | None]], cwd: str,
              error: str | None = None) -> Run:
        """Run the plan in a thread; `error` short-circuits to a failed run."""
        with self._lock:
            run = Run(id=f"run-{next(self._ids)}", step_id=step_id,
                      argv=plan[0][0] if plan else [], plan=plan)
            self._runs[run.id] = run
            self._order.append(run.id)
            if error:
                run.lines.append(error)
                run.state = "error"
                run.finished = time.time()
                return run
        threading.Thread(target=self._worker, args=(run, cwd), daemon=True).start()
        return run

    def _worker(self, run: Run, cwd: str) -> None:
        try:
            for argv, stdin_text in run.plan:
                run.lines.append("$ " + " ".join(argv))
                proc = subprocess.Popen(
                    argv, cwd=cwd, text=True, bufsize=1,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    stdin=subprocess.PIPE if stdin_text else subprocess.DEVNULL,
                )
                if stdin_text:
                    try:
                        proc.stdin.write(stdin_text)
                    finally:
                        proc.stdin.close()
                assert proc.stdout is not None
                for line in proc.stdout:
                    with self._lock:
                        if len(run.lines) < MAX_LINES:
                            run.lines.append(line.rstrip("\n"))
                code = proc.wait()
                if code != 0:
                    run.exit_code = code
                    run.state = "failed"
                    run.finished = time.time()
                    return
            run.exit_code = 0
            run.state = "ok"
        except OSError as exc:
            run.state = "error"
            run.lines.append(f"{type(exc).__name__}: {exc}")
        finally:
            run.finished = time.time()

    def get(self, run_id: str) -> Run | None:
        return self._runs.get(run_id)

    def recent(self, limit: int = 10) -> list[dict]:
        with self._lock:
            ids = self._order[-limit:]
        return [self._runs[i].as_dict(limit=20) for i in reversed(ids)]
