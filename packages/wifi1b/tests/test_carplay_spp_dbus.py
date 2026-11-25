#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128 (isolated private-bus fixtures only)
"""Offline cp_spp_dbus tests on a real, private dbus-daemon.

The private bus uses a minimal config with no service directories and no
activation.  The mock OEM service is first-party test code compiled from this
repository; no system bus, OEM binary, MFi chip or real SPP peer is used.
"""
from __future__ import annotations

import os
from pathlib import Path
import select
import signal
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/carplay/cp_spp_dbus.c"
TEST = ROOT / "tests/test_carplay_spp_dbus.c"
SCENARIOS = (
    "basic",
    "one_arg",
    "wrong_case",
    "no_connect",
    "listen3",
    "listen_timeout",
    "bad_signature",
    "xml_dtd",
    "owner",
    "serial_owner",
    "impostor",
    "wrong_uid",
    "serial_missing",
    "bad_serial_path",
    "bad_status_signature",
    "send_error",
    "send_timeout",
    "disconnect",
    "close_inflight",
    "close_fatal",
    "attach_existing",
    "reconnect",
    "close_owned",
)


def pkg_config(*args: str) -> list[str]:
    result = subprocess.run(["pkg-config", *args], capture_output=True,
                            text=True, check=True)
    return result.stdout.split()


def require_tools() -> tuple[list[str], list[str]]:
    for tool in ("gcc", "pkg-config", "dbus-daemon"):
        if shutil.which(tool) is None:
            raise SystemExit(f"SKIP: missing {tool}")
    if subprocess.run(["pkg-config", "--exists", "dbus-1"]).returncode != 0:
        raise SystemExit("SKIP: libdbus-1 development files are unavailable")
    if subprocess.run(["pkg-config", "--exists", "libxml-2.0"]).returncode != 0:
        raise SystemExit("SKIP: libxml2 development files are unavailable")
    cflags = pkg_config("--cflags", "dbus-1", "libxml-2.0")
    libs = pkg_config("--libs", "dbus-1", "libxml-2.0")
    return cflags, libs


def compile_test(out: Path, cflags: list[str], libs: list[str],
                 sanitize: bool) -> None:
    command = [
        "gcc", "-std=c11", "-O1" if sanitize else "-O2", "-g",
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-fno-omit-frame-pointer" if sanitize else "-fno-omit-frame-pointer",
        "-DCP_SPP_DBUS_HAVE_LIBXML2=1", *cflags,
    ]
    if sanitize:
        command += ["-fsanitize=address,undefined"]
    command += [str(SOURCE), str(TEST), "-Wl,--wrap=dbus_connection_read_write",
                *libs, "-o", str(out)]
    subprocess.run(command, check=True, timeout=180)


def start_private_bus(directory: Path) -> tuple[subprocess.Popen, str]:
    config = directory / "bus.conf"
    socket = directory / "bus"
    config.write_text(
        "<busconfig>\n"
        "  <type>session</type>\n"
        f"  <listen>unix:path={socket}</listen>\n"
        "  <auth>EXTERNAL</auth>\n"
        "  <policy context=\"default\">\n"
        "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
        "    <allow eavesdrop=\"true\"/>\n"
        "    <allow own=\"*\"/>\n"
        "  </policy>\n"
        "</busconfig>\n"
    )
    process = subprocess.Popen(
        ["dbus-daemon", "--config-file", str(config), "--nofork",
         "--print-address=1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    assert process.stdout is not None
    ready, _, _ = select.select([process.stdout], [], [], 10.0)
    if not ready:
        process.kill()
        raise AssertionError("private dbus-daemon did not print an address")
    address = process.stdout.readline().strip()
    if not address.startswith("unix:path="):
        process.kill()
        raise AssertionError(f"unexpected private bus address: {address!r}")
    if not socket.exists():
        process.kill()
        raise AssertionError("private bus socket was not created")
    return process, address


def run_scenario(binary: Path, scenario: str, sanitize: bool) -> None:
    with tempfile.TemporaryDirectory(prefix=f"wifi1b-spp-{scenario}-",
                                     dir="/tmp/opencode") as directory:
        bus, address = start_private_bus(Path(directory))
        env = dict(os.environ)
        # Prove the module never falls back to the host/system bus or
        # autolaunch even when those environment values are hostile.
        env["DBUS_SESSION_BUS_ADDRESS"] = "autolaunch:do-not-use"
        env["DBUS_SYSTEM_BUS_ADDRESS"] = "autolaunch:do-not-use"
        if sanitize:
            env["ASAN_OPTIONS"] = "detect_leaks=0:abort_on_error=1"
            env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        try:
            process = subprocess.Popen(
                [str(binary), address, scenario], env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, start_new_session=True)
            try:
                stdout, stderr = process.communicate(timeout=120)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                stdout, stderr = process.communicate(timeout=10)
                raise AssertionError(f"scenario {scenario} timed out\n{stdout}\n{stderr}")
            finally:
                # A failed C assertion must not leave a fixture mock holding
                # the captured output pipes or the private bus connection.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            result = subprocess.CompletedProcess(
                process.args, process.returncode, stdout, stderr)
        finally:
            bus.terminate()
            try:
                bus.wait(timeout=10)
            except subprocess.TimeoutExpired:
                bus.kill()
                bus.wait(timeout=10)
        if result.returncode != 0:
            raise AssertionError(
                f"scenario {scenario} failed rc={result.returncode}\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        assert "PASS:" in result.stdout, (scenario, result.stdout)
        assert "autolaunch" not in result.stderr, result.stderr


def main() -> int:
    cflags, libs = require_tools()
    with tempfile.TemporaryDirectory(prefix="wifi1b-spp-dbus-build-",
                                     dir="/tmp/opencode") as directory:
        build = Path(directory)
        binary = build / "test_spp_dbus"
        compile_test(binary, cflags, libs, sanitize=False)
        for scenario in SCENARIOS:
            run_scenario(binary, scenario, sanitize=False)
            print(f"PASS: cp_spp_dbus scenario {scenario}")

        # The no-libxml2 configuration must still compile strictly and fail
        # closed at runtime instead of using a home-grown XML parser.
        noxml = build / "test_spp_dbus-noxml.o"
        subprocess.run([
            "gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", *pkg_config("--cflags", "dbus-1"),
            "-c", str(SOURCE), "-o", str(noxml),
        ], check=True, timeout=120)

        asan = build / "test_spp_dbus_asan"
        compile_test(asan, cflags, libs, sanitize=True)
        for scenario in ("basic", "wrong_case", "owner", "impostor",
                         "listen_timeout", "bad_status_signature", "send_timeout",
                         "disconnect", "close_inflight", "close_fatal",
                         "attach_existing", "reconnect", "close_owned"):
            run_scenario(asan, scenario, sanitize=True)
            print(f"PASS: cp_spp_dbus ASan/UBSan scenario {scenario}")

    print("PASS: cp_spp_dbus private-bus offline transport")
    return 0


if __name__ == "__main__":
    sys.exit(main())
