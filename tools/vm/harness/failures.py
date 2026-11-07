# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Post-mortem bundle for a failed or timed-out guest run.

Collection happens while QEMU is still alive, so the guest shell can answer
short, read-only commands. Every section is best-effort: a dead or wedged
shell records the collection error and the rest of the bundle still lands in
the run directory. Nothing here writes to the guest.
"""
from __future__ import annotations

import re
from pathlib import Path

MAX_SECTION_BYTES = 256 * 1024

SECTIONS = (
    ("dmesg", "dmesg 2>&1 | tail -160"),
    ("processes", "ps -ef 2>&1"),
    ("mounts", "cat /proc/mounts 2>&1"),
    ("storage", "df -h 2>&1; echo; ls -la /update /rw_data /log_data 2>&1 | head -120"),
    ("weston", "for f in /tmp/weston.log /tmp/weston-screenshooter.log; do "
               "[ -f \"$f\" ] || continue; echo \"=== $f ===\"; tail -200 \"$f\" 2>&1; done"),
    ("qml", "for f in /tmp/*.log /tmp/*.out /tmp/dm.stdout; do "
            "[ -f \"$f\" ] || continue; echo \"=== $f ===\"; tail -120 \"$f\" 2>&1; done"),
    ("updater", "for f in /tmp/appupgrade.out /log_data/AppUpgrade/upgrade_log; do "
                "[ -f \"$f\" ] || continue; echo \"=== $f ===\"; tail -200 \"$f\" 2>&1; done"),
    ("tools", "ls -la /media/tools /media/usb 2>&1 | head -120"),
)

FAILURE_WORDS = re.compile(
    r"(?i)(error|fail|fatal|abort|assert|segfault|timeout|timed out|"
    r"not found|no such file|denied|blocked|panic)"
)


def _tail(path: Path, limit: int = 160) -> str:
    try:
        text = path.read_text(errors="replace")
    except OSError as exc:
        return f"<unavailable: {exc}>\n"
    lines = text.splitlines()
    return "\n".join(lines[-limit:]) + ("\n" if lines else "")


def _clip(text: str) -> str:
    if len(text.encode()) <= MAX_SECTION_BYTES:
        return text
    return text.encode()[:MAX_SECTION_BYTES].decode(errors="replace") + "\n<clipped>\n"


def collect_failure_bundle(ser, run_dir: Path, result: dict, timeout: float = 20) -> dict:
    """Collect a bundle; return metadata and store it in `result`."""
    from launch import execute_guest  # local import avoids a module cycle

    run_dir = Path(run_dir)
    bundle = run_dir / "failure"
    bundle.mkdir(exist_ok=True)
    sections: dict[str, dict] = {}

    for name, command in SECTIONS:
        path = bundle / f"{name}.log"
        try:
            _, output, rc, seconds = execute_guest(ser, command, timeout)
            text = _clip(output + ("\n" if output and not output.endswith("\n") else ""))
            path.write_text(text)
            sections[name] = {"path": str(path), "rc": rc, "seconds": seconds,
                              "bytes": len(text.encode())}
        except Exception as exc:  # noqa: BLE001
            text = f"collection failed: {type(exc).__name__}: {exc}\n"
            path.write_text(text)
            sections[name] = {"path": str(path), "error": str(exc), "bytes": len(text.encode())}

    serial_tail = bundle / "serial-tail.log"
    serial_tail.write_text(_tail(run_dir / "serial.log", 240))
    guest_output = bundle / "guest-output-tail.log"
    guest_output.write_text(_tail(run_dir / "guest-output.log", 160))

    excerpts: list[str] = []
    for name in ("dmesg", "weston", "qml", "updater"):
        path = bundle / f"{name}.log"
        if not path.is_file():
            continue
        for line in path.read_text(errors="replace").splitlines():
            if FAILURE_WORDS.search(line):
                excerpts.append(f"{name}: {line.strip()}")
                if len(excerpts) >= 60:
                    break
        if len(excerpts) >= 60:
            break

    summary = bundle / "summary.md"
    summary.write_text(
        "# Failure bundle\n\n"
        f"- run: `{result.get('run_dir')}`\n"
        f"- verdict: **{result.get('verdict')}** rc={result.get('rc')} "
        f"guest_rc={result.get('guest_rc')}\n"
        f"- scenario: `{(result.get('scenario') or {}).get('name', 'custom command')}`\n"
        f"- profile: `{(result.get('profile') or {}).get('name', '?')}`\n"
        f"- error: `{result.get('error')}`\n"
        f"- boot: {result.get('boot_seconds')} s; readiness: "
        f"{result.get('readiness_seconds')} s\n\n"
        "## Files\n\n"
        + "\n".join(f"- `{name}.log` - {meta}" for name, meta in sections.items())
        + "\n"
        + "\n## Failure-word excerpts\n\n"
        + ("\n".join(f"- {line}" for line in excerpts) if excerpts else "- none")
        + "\n\n## Serial tail\n\n```\n"
        + _tail(run_dir / "serial.log", 160)
        + "```\n"
    )
    metadata = {
        "path": str(bundle),
        "summary": str(summary),
        "sections": sections,
        "excerpts": len(excerpts),
    }
    result["failure_bundle"] = metadata
    return metadata
