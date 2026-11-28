#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Host test for the aa_shim process self-gate (docs/hudnav/AA_PROTOCOL.md).

The shim is preloaded launcher-wide, so it must be inert outside AppProjection.
This runs tests/aa_gate_probe (basename != AppProjection) with LD_PRELOAD set and
checks whether anything reaches a local unix socket:

  default env            -> no connection, no bytes
  APPID=111              -> no connection, no bytes
  APPID=110              -> forwarded (AppID fallback)
  HUDNAV_FORCE=1         -> forwarded (host-test override)
"""
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SHIM = ROOT / "out" / "aa_shim.so"
PROBE = ROOT / "out" / "aa_gate_probe"

fails = 0
checks = 0


def check(cond, what):
    global fails, checks
    checks += 1
    if not cond:
        print(f"FAIL: {what}", file=sys.stderr)
        fails += 1


def run_probe(env_extra, sock_path, expect_bytes):
    """Run the probe; return True if a connection arrived (and data if expected)."""
    try:
        os.unlink(sock_path)
    except FileNotFoundError:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(sock_path)
    srv.listen(1)
    srv.settimeout(1.0)

    env = dict(os.environ)
    env["LD_PRELOAD"] = str(SHIM)
    env["HUDNAV_SOCK"] = sock_path
    env.pop("APPID", None)
    env.pop("HUDNAV_FORCE", None)
    env.update(env_extra)

    p = subprocess.run([str(PROBE)], env=env, capture_output=True, text=True)
    check(p.returncode == 0, f"probe exit {p.returncode}: {p.stderr.strip()}")

    data = b""
    connected = False
    try:
        conn, _ = srv.accept()
        connected = True
        conn.settimeout(0.5)
        try:
            data = conn.recv(4096)
        except socket.timeout:
            data = b""
        conn.close()
    except socket.timeout:
        connected = False
    finally:
        srv.close()

    if expect_bytes:
        check(connected, "gate open: expected a connection")
        check(b"STATE s=1" in data, f"gate open: expected STATE line, got {data!r}")
    else:
        check(not connected, f"gate closed: unexpected connection/data {data!r}")
    return connected, data


def main():
    if not SHIM.is_file() or not PROBE.is_file():
        print("build the prototype first (sh scripts/build.sh)", file=sys.stderr)
        return 2
    with tempfile.TemporaryDirectory() as td:
        sock = os.path.join(td, "hudnav.sock")
        run_probe({}, sock, expect_bytes=False)
        run_probe({"APPID": "111"}, sock, expect_bytes=False)
        run_probe({"APPID": "110"}, sock, expect_bytes=True)
        run_probe({"HUDNAV_FORCE": "1"}, sock, expect_bytes=True)
        time.sleep(0.05)

    print(f"aa self-gate: {checks} checks, {fails} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
