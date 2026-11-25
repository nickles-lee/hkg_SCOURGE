#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Offline checks for the passive OEM receiver probe.

No network, OEM execution, real keychain access or OEM binaries are used.
All build products and fixtures stay under /tmp/opencode.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src/carplay"
TEST = ROOT / "tests/test_carplay_oem_receiver.c"
MAIN = SRC / "cp_receiver_probe_main.c"
CORE = SRC / "cp_oem_receiver.c"
HEADER = SRC / "cp_oem_receiver.h"
CC = os.environ.get("CC", "cc")
COMMON = [
    CC,
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
    "-Wshadow",
    "-Wstrict-prototypes",
    "-Wmissing-prototypes",
    "-I",
    str(SRC),
]


def invoke(argv: list[str], *, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(argv, text=True, capture_output=True, env=env, timeout=60)
    except subprocess.TimeoutExpired as exc:
        raise SystemExit(f"command timed out after 60s: {' '.join(argv)}") from exc


def run(argv: list[str], *, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    result = invoke(argv, env=env)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(f"command failed ({result.returncode}): {' '.join(argv)}")
    return result


def expect_failure(argv: list[str], code: int, needle: str) -> None:
    result = invoke(argv)
    if result.returncode != code:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        raise SystemExit(
            f"expected exit {code}, got {result.returncode}: {' '.join(argv)}"
        )
    combined = result.stdout + result.stderr
    if needle not in combined:
        raise SystemExit(f"missing {needle!r} in output of {' '.join(argv)}")


def main() -> int:
    # A FIFO-opening regression must fail the suite, not leave CI hung.
    for action in (lambda: run(["fixture-hang"]),
                   lambda: expect_failure(["fixture-hang"], 1, "unused")):
        with patch.object(subprocess, "run", side_effect=subprocess.TimeoutExpired("fixture-hang", 60)) as call:
            try:
                action()
            except SystemExit as exc:
                assert "timed out" in str(exc)
            else:
                raise AssertionError("timeout must fail the test gate")
            assert call.call_args.kwargs["timeout"] == 60
    base = Path("/tmp/opencode")
    base.mkdir(mode=0o700, parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cp-oem-receiver-", dir=base) as tmp:
        tmp_path = Path(tmp)
        normal = tmp_path / "test-normal"
        sanitizer = tmp_path / "test-sanitizer"
        probe_bin = tmp_path / "cp_receiver_probe"

        run(COMMON + ["-O2", str(CORE), str(TEST), "-o", str(normal)])
        run([str(normal)])
        run(
            COMMON
            + [
                "-O1",
                "-g",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-fno-pie",
                "-no-pie",
                str(CORE),
                str(TEST),
                "-o",
                str(sanitizer),
            ]
        )
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=1"
        run([str(sanitizer)], env=env)

        run(COMMON + ["-O2", str(CORE), str(MAIN), "-o", str(probe_bin), "-ldl"])
        expect_failure([str(probe_bin)], 2, "probe disabled")
        expect_failure([str(probe_bin), "--identity"], 3, "identity adapter unavailable")
        expect_failure(
            [str(probe_bin), "--probe", "--dry-run", "--root", str(tmp_path / "missing")],
            4,
            "pin check failed",
        )

    core_text = CORE.read_text()
    header_text = HEADER.read_text()
    main_text = MAIN.read_text()
    forbidden_core = [
        "AirPlayCopyHomeKitPairingIdentity",
        "PairingSession",
        "_carplay-ctrl",
        "ctrl-int",
        "DNSServiceBrowse",
        "dlopen",
        "socket(",
        "connect(",
    ]
    for token in forbidden_core:
        if token in core_text or token in header_text:
            raise SystemExit(f"forbidden token in core/header: {token}")
    for token in ["--probe", "DNSServiceBrowse", "DNSServiceResolve", "dlopen"]:
        if token not in main_text:
            raise SystemExit(f"missing expected main token: {token}")
    for token in [
        "AirPlayCopyHomeKitPairingIdentity",
        "PairingSession",
        "PairingSessionCopyIdentity",
        "PairingSessionSetKeychainInfo",
        "_carplay-ctrl",
        "ctrl-int",
    ]:
        if token in main_text:
            raise SystemExit(f"forbidden identity/discovery token in main: {token}")
    if "snapshot->pi" in main_text or "snapshot.pi" in main_text:
        raise SystemExit("main must not print the pi field directly")
    pins = [
        "8f0096bcc0734d4c9766c15bac54c42f302ed7057182c2573b5ac543913960e3",
        "ef9e69e3b3388d4a7a1d185827c710af25b8ee99d406c60c664a9609b00aada0",
        "9104e2c16ee27e647fac090ee7f3aa4cc686b8caaea1dc24763af2950c0ebfae",
        "e3603e6b0744cf07724cc5cc79ea01ea3d9207d7eaf37c544c7ce26536ecd7a2",
        "69690210d1aa4dec33c77b0e80f1d3368f8a70bcb507c1b158b6245dc29eb2f3",
    ]
    for digest in pins:
        if digest not in core_text:
            raise SystemExit(f"missing target pin: {digest}")
    if "6c6849fb0e3619c170ad3b9679f6c60d9ccbfb88e55e8af4210ddf51dfb52592" not in core_text:
        raise SystemExit("missing libdns_sd pin")
    if 'CP_OEM_RECEIVER_SOURCE_VERSION "320.17.6"' not in header_text:
        raise SystemExit("source version fact missing")
    if "CP_OEM_RECEIVER_PROJECTION_UNAVAILABLE" not in header_text:
        raise SystemExit("projection-unavailable contract missing")
    if "PROJECTION_ACTIVE" in header_text or "PROJECTION_ACTIVE" in core_text:
        raise SystemExit("forbidden projection-active state present")

    print("all passive OEM receiver probe checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
