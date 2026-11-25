#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Target-rootfs build test for the experimental CarPlay broker helper.

Build-only: the helper is compiled and inspected, never executed.  Skipped
when SCOURGE_ROOTFS is unset so ordinary host/VM runs do not require the
owner's firmware tree.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/build_carplay_broker.py"
ALLOWED_NEEDED = {
    "libdbus-1.so.3", "libxml2.so.2", "libpthread.so.0", "libdl.so.2",
    "libm.so.6", "libz.so.1", "libc.so.6",
}


def run(argv: list[str], timeout: int = 180) -> subprocess.CompletedProcess[str]:
    return subprocess.run(argv, text=True, capture_output=True, timeout=timeout,
                          check=False)


def main() -> int:
    rootfs = os.environ.get("SCOURGE_ROOTFS")
    if not rootfs:
        print("SKIP: SCOURGE_ROOTFS not set; target helper build not run")
        return 0
    rootfs_path = Path(rootfs)
    if not rootfs_path.is_dir():
        raise SystemExit(f"SCOURGE_ROOTFS is not a directory: {rootfs_path}")

    base = Path("/tmp/opencode")
    base.mkdir(mode=0o700, parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cp-broker-target-",
                                     dir=base) as tmp:
        out = Path(tmp) / "out"
        result = run([sys.executable, str(SCRIPT), "--rootfs", str(rootfs_path),
                      "--out", str(out)])
        if result.returncode != 0:
            sys.stdout.write(result.stdout)
            sys.stderr.write(result.stderr)
            raise SystemExit(f"target broker build failed: {result.returncode}")

        manifest = json.loads((out / "build-metadata.json").read_text())
        assert manifest["status"] == \
            "EXPERIMENTAL_BUILD_ONLY_NOT_CAR_VALIDATED", manifest["status"]
        assert manifest["car_validated"] is False
        assert manifest["installer_activated"] is False
        assert manifest["oem_binary_executed"] is False

        helper = manifest["helper"]
        assert set(helper["needed"]) == ALLOWED_NEEDED, helper["needed"]
        assert helper["compat_shims"] == [
            "fstat", "fstatat", "__isoc23_strtol", "__isoc23_strtoul"
        ], helper["compat_shims"]
        assert all(tuple(int(part) for part in version.split(".")[:2]) <=
                   (2, 25) for version in helper["versions"]), helper["versions"]
        assert "cp_target_compat.c" in manifest["sources"]
        assert "cp_usb.c" in manifest["sources"]

        binary = out / "carplay-broker"
        assert binary.is_file() and binary.stat().st_size > 0
        header = run(["readelf", "-lW", str(binary)])
        assert "Requesting program interpreter: /lib/ld-linux-x86-64.so.2" \
            in header.stdout
        dynamic = run(["readelf", "-dW", str(binary)])
        assert "RPATH" not in dynamic.stdout
        assert "RUNPATH" not in dynamic.stdout
        symbols = run(["readelf", "-sW", str(binary)]).stdout
        for name in ("fstat", "fstatat", "__isoc23_strtol",
                     "__isoc23_strtoul"):
            assert re.search(rf"\b{re.escape(name)}\b", symbols), name
        assert "GLIBC_PRIVATE" not in run(
            ["readelf", "-V", str(binary)]).stdout

    print("PASS: target broker helper builds against glibc 2.25 with "
          "compat shims and the allowed NEEDED set")
    return 0


if __name__ == "__main__":
    sys.exit(main())
