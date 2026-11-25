#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Loader-environment and missing-library tests for the MFi auth helper.

The helper is built from first-party source on the host with CP_OEM_AUTH_TEST.
No OEM library, chip, certificate or credential is used.  These checks pin the
unit's explicit /app/lib search path and prove that an absent library fails
closed instead of being reported as a provider/busy/success state.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
AUTH_UNIT = ROOT / "artifacts/systemd/scourge-carplay-auth.service"
OEM_TEST = HERE / "test_carplay_oem_auth.py"


def load_oem_test():
    spec = importlib.util.spec_from_file_location("test_carplay_oem_auth",
                                                   OEM_TEST)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    unit = AUTH_UNIT.read_text(encoding="utf-8")
    assert "Environment=LD_LIBRARY_PATH=/app/lib" in unit
    assert "LD_LIBRARY_PATH=/" not in unit.replace(
        "LD_LIBRARY_PATH=/app/lib", "")
    assert "LD_PRELOAD" not in unit and "LD_AUDIT" not in unit

    oem = load_oem_test()
    with tempfile.TemporaryDirectory(prefix="carplay-auth-loader-",
                                     dir="/tmp/opencode") as tmp_name:
        tmp = Path(tmp_name)
        helper = oem.build_host_helper(tmp)
        socket_dir = tmp / "run"
        socket_dir.mkdir(mode=0o700)

        # The unit's explicit target directory is accepted by the loader
        # sanitizer; a missing library is still a hard LIBRARY failure (4).
        cases = (
            ("missing-absolute", str(tmp / "absent-libCoreUtils.so"),
             {"LD_LIBRARY_PATH": "/app/lib"}),
            ("missing-app-lib", "/app/lib/absent-libCoreUtils.so", {}),
            ("empty-path", "", {}),
        )
        for name, library, extra in cases:
            socket_path = socket_dir / (name + ".sock")
            env = oem.helper_env(extra)
            result = subprocess.run(
                [str(helper), "--serve", "--experimental", "--library",
                 library, "--expected-sha256", "0" * 64, "--socket",
                 str(socket_path), "--once"],
                text=True, capture_output=True, timeout=30, env=env)
            assert result.returncode == 4, (name, result)
            assert not socket_path.exists(), name

        # An unsafe inherited path is still a policy refusal (3) and is never
        # silently ignored.
        result = subprocess.run(
            [str(helper), "--serve", "--experimental", "--library",
             str(tmp / "absent.so"), "--expected-sha256", "0" * 64,
             "--socket", str(socket_dir / "unsafe.sock"), "--once"],
            text=True, capture_output=True, timeout=30,
            env=oem.helper_env({"LD_LIBRARY_PATH": "/tmp/opencode"}))
        assert result.returncode == 3, result

    print("PASS: MFi helper loader path and missing-library failure are fail-closed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
