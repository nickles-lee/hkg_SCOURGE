# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Run directory discovery, extraction and safe pruning."""
from __future__ import annotations

import json
import shutil
from pathlib import Path

from .result_schema import schema_state, validate_result


def _result(path: Path) -> dict:
    try:
        data = json.loads((path / "result.json").read_text())
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def collect_runs(vm_dir: Path) -> list[dict]:
    runs = []
    root = Path(vm_dir) / "runs"
    try:
        entries = [entry for entry in root.iterdir() if entry.is_dir()]
    except OSError:
        return runs
    for path in entries:
        result_file = path / "result.json"
        result = _result(path) if result_file.is_file() else None
        data = result or {}
        problems = (validate_result(result) if result_file.is_file()
                    else ["missing result.json"])
        scenario = data.get("scenario") or {}
        runs.append({
            "run": path.name,
            "path": str(path),
            "mtime": path.stat().st_mtime if path.exists() else 0,
            "label": data.get("label") or "",
            "mode": data.get("mode") or "vendor",
            "scenario": scenario.get("name") if isinstance(scenario, dict) else None,
            "verdict": data.get("verdict") or "UNKNOWN",
            "rc": data.get("rc"),
            "known_failure": bool(data.get("known_failure")),
            "keep_overlay": bool(data.get("keep_overlay")),
            "overlays_retained": bool(data.get("overlays_retained")),
            "screenshot": (data.get("screenshot") or {}),
            "schema_state": schema_state(result, problems),
            "schema_ok": not problems,
            "schema_problem_count": len(problems),
            "schema_problems": problems[:8],
            "result": str(result_file) if result_file.is_file() else None,
        })
    runs.sort(key=lambda item: item["mtime"], reverse=True)
    return runs


def filter_runs(runs: list[dict], label: str | None = None,
                scenario: str | None = None, verdict: str | None = None) -> list[dict]:
    selected = runs
    if label:
        selected = [item for item in selected if label in item["label"]]
    if scenario:
        selected = [item for item in selected if item["scenario"] == scenario]
    if verdict:
        selected = [item for item in selected if item["verdict"] == verdict]
    return selected


def extract(vm_dir: Path, run: str, member: str, destination: Path) -> dict:
    root = (Path(vm_dir) / "runs").resolve()
    run_dir = (root / run).resolve()
    if root not in run_dir.parents:
        raise ValueError(f"run escapes runs directory: {run}")
    source = (run_dir / member).resolve()
    if source != run_dir and run_dir not in source.parents:
        raise ValueError(f"member escapes run directory: {member}")
    if not source.is_file():
        raise ValueError(f"not a regular file: {member}")
    destination = Path(destination)
    if destination.is_dir():
        destination = destination / source.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    return {"run": run, "member": member, "source": str(source),
            "destination": str(destination), "size": destination.stat().st_size}


def prune(vm_dir: Path, keep: int = 20, apply: bool = False,
          label: str | None = None, scenario: str | None = None,
          allow_no_result: bool = False) -> dict:
    if keep < 0:
        raise ValueError("keep must be >= 0")
    runs = filter_runs(collect_runs(vm_dir), label=label, scenario=scenario)
    candidates: list[dict] = []
    protected: list[dict] = []
    for index, item in enumerate(runs):
        if index < keep:
            protected.append({**item, "reason": "keep-newest"})
        elif item["run"] == "daemon":
            # A live daemon owns overlays/sockets here; never prune it.
            protected.append({**item, "reason": "daemon"})
        elif item["result"] is None and not allow_no_result:
            # No result.json: not a completed managed run (daemon dirs,
            # regression bundles, interrupted runs). Explicit opt-in only.
            protected.append({**item, "reason": "no-result"})
        elif item["keep_overlay"] or item["overlays_retained"]:
            protected.append({**item, "reason": "retained-overlays"})
        else:
            candidates.append(item)
    freed = 0
    for item in candidates:
        path = Path(item["path"])
        if path.is_symlink():
            protected.append({**item, "reason": "symlink"})
            continue
        try:
            freed += sum(file.stat().st_size for file in path.rglob("*")
                         if file.is_file() and not file.is_symlink())
        except OSError:
            pass
    deleted = []
    if apply:
        for item in candidates:
            path = Path(item["path"])
            root = (Path(vm_dir) / "runs").resolve()
            if path.is_symlink() or root not in path.resolve().parents:
                protected.append({**item, "reason": "unsafe-path"})
                continue
            shutil.rmtree(path)
            deleted.append(item["run"])
    return {
        "keep": keep,
        "apply": apply,
        "allow_no_result": allow_no_result,
        "candidates": [item["run"] for item in candidates],
        "deleted": deleted,
        "protected": protected,
        "estimated_bytes": freed,
    }
