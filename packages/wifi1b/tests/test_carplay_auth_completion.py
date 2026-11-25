#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Strict and sanitizer tests of deferred session authentication, no OEM calls."""
from pathlib import Path
import os
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent / "src/carplay"
CASES = {
    "completion": (["test_carplay_auth_completion.c"],
                   ["cp_bootstrap.c", "cp_wire.c"], []),
    "broker": (["test_carplay_auth_broker.c"],
               ["cp_auth_broker.c", "cp_oem_auth_async.c", "cp_session.c",
                "cp_link.c", "cp_bootstrap.c", "cp_wire.c"],
               ["-DCP_OEM_AUTH_ASYNC_TEST"]),
}

with tempfile.TemporaryDirectory(prefix="cp-completion-", dir="/tmp/opencode") as tmp:
    for sanitized in (False, True):
        flags = (["-O1", "-g", "-fsanitize=address,undefined",
                  "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
                 if sanitized else ["-O2"])
        for name, (tests, sources, defines) in CASES.items():
            binary = Path(tmp) / (name + ("-sanitized" if sanitized else "-strict"))
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                *flags, *defines, "-I", str(SOURCE), "-I", str(HERE),
                *[str(HERE / path) for path in tests],
                *[str(SOURCE / path) for path in sources], "-o", str(binary)
            ], check=True, timeout=120)
            env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
            subprocess.run([str(binary)], check=True, timeout=120, env=env)
print("PASS: deferred auth completion and nonblocking helper/session broker (strict + ASan/UBSan)")
