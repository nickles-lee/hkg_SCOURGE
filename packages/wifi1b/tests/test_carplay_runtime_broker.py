#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Strict and sanitizer tests of the unactivated runtime broker.

Each scenario gets a fresh private dbus-daemon with no service directories and
no activation.  The C fixture starts a first-party mock OEM service, a private
helper socket and synthetic phone link frames.  No system bus, OEM binary,
MFi chip, hardware or host network is used.

The normal libdbus scenarios disable ASan leak detection because libdbus and
the in-process mock service retain process-global caches.  The separate
``leakinit`` scenario never opens libdbus and runs init/destroy with leak
detection enabled.  The ``failchild`` meta-test deliberately leaves a mock
child holding the captured pipes; the runner must kill its own fixture process
group rather than leak or hang.
"""
from __future__ import annotations

import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/carplay"
TEST = ROOT / "tests/test_carplay_runtime_broker.c"
SOURCES = [
    "cp_runtime_broker.c",
    "cp_spp_dbus.c",
    "cp_spp_socket.c",
    "cp_auth_broker.c",
    "cp_oem_auth_async.c",
    "cp_session.c",
    "cp_link.c",
    "cp_bootstrap.c",
    "cp_wire.c",
]
SCENARIOS = (
    "happy",
    "timeout",
    "no_connect",
    "cancel",
    "usb",
    "consent",
    "owner",
    "peer",
    "stale",
    "authfail",
    "backpressure",
    "delayed",
    "drain_boundary",
    "ap",
    "ap_pre",
    "epoch",
    "epoch_pending_feed",
    "epoch_pending_rx",
    "feedfail_hook",
    "feedfail_socket",
    "overflow",
    "attach_early",
    "reconnect_same",
)


def pkg_config(*args: str) -> list[str]:
    result = subprocess.run(["pkg-config", *args], capture_output=True,
                            text=True, check=True)
    return result.stdout.split()


def require_tools() -> tuple[list[str], list[str]]:
    for tool in ("cc", "pkg-config", "dbus-daemon"):
        if shutil.which(tool) is None:
            raise SystemExit(f"SKIP: missing {tool}")
    for package in ("dbus-1", "libxml-2.0"):
        if subprocess.run(["pkg-config", "--exists", package]).returncode != 0:
            raise SystemExit(f"SKIP: {package} development files unavailable")
    return (pkg_config("--cflags", "dbus-1", "libxml-2.0"),
            pkg_config("--libs", "dbus-1", "libxml-2.0"))


def compile_binary(out: Path, cflags: list[str], libs: list[str],
                   sanitize: bool) -> None:
    command = [
        "cc", "-std=c11", "-O1" if sanitize else "-O2", "-g",
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
        "-DCP_OEM_AUTH_ASYNC_TEST", "-DCP_SPP_SOCKET_TESTING",
        "-DCP_SPP_DBUS_HAVE_LIBXML2=1",
        "-I", str(SOURCE), "-I", str(ROOT / "tests"),
        "-I", str(ROOT.parent / "common"), *cflags,
    ]
    if sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += [str(TEST)]
    command += [str(SOURCE / source) for source in SOURCES]
    command += [*libs, "-o", str(out)]
    subprocess.run(command, check=True, timeout=240)


def start_bus(directory: Path) -> tuple[subprocess.Popen, str]:
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
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=True)
    assert process.stdout is not None
    ready, _, _ = select.select([process.stdout], [], [], 10.0)
    if not ready:
        process.kill()
        raise AssertionError("private dbus-daemon did not start")
    address = process.stdout.readline().strip()
    if not address.startswith("unix:path="):
        process.kill()
        raise AssertionError(f"unexpected private address {address!r}")
    if not socket.exists():
        process.kill()
        raise AssertionError("private bus socket missing")
    return process, address


def stop_bus(bus: subprocess.Popen) -> None:
    bus.terminate()
    try:
        bus.wait(timeout=10)
    except subprocess.TimeoutExpired:
        bus.kill()
        bus.wait(timeout=10)


def kill_fixture_group(process: subprocess.Popen) -> None:
    """Kill only the fixture process group created with start_new_session."""
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def run_scenario(binary: Path, scenario: str, sanitize: bool,
                 leak_check: bool = False) -> None:
    with tempfile.TemporaryDirectory(prefix=f"wifi1b-runtime-{scenario}-",
                                     dir="/tmp/opencode") as directory:
        bus, address = start_bus(Path(directory))
        env = dict(os.environ)
        env["DBUS_SESSION_BUS_ADDRESS"] = "autolaunch:do-not-use"
        env["DBUS_SYSTEM_BUS_ADDRESS"] = "autolaunch:do-not-use"
        if sanitize:
            # libdbus and the mock service retain process-global caches; the
            # leakinit scenario below enables leak detection for the runtime's
            # own init/destroy path without opening libdbus.
            env["ASAN_OPTIONS"] = (
                "detect_leaks=1:abort_on_error=1" if leak_check
                else "detect_leaks=0:abort_on_error=1")
            env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        try:
            process = subprocess.Popen(
                [str(binary), address, scenario], env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, start_new_session=True)
            try:
                stdout, stderr = process.communicate(timeout=180)
            except subprocess.TimeoutExpired:
                kill_fixture_group(process)
                stdout, stderr = process.communicate(timeout=10)
                raise AssertionError(
                    f"scenario {scenario} timed out\n{stdout}\n{stderr}")
            finally:
                # A failed C assertion must not leave a fixture mock holding
                # the captured output pipes or the private bus connection.
                kill_fixture_group(process)
            result = subprocess.CompletedProcess(
                process.args, process.returncode, stdout, stderr)
        finally:
            stop_bus(bus)
        if result.returncode != 0:
            raise AssertionError(
                f"scenario {scenario} failed rc={result.returncode}\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        assert "PASS:" in result.stdout, (scenario, result.stdout)
        assert "autolaunch" not in result.stderr, result.stderr


def pid_alive(pid: int) -> bool:
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().split()
    except (FileNotFoundError, ProcessLookupError):
        return False
    return len(fields) > 2 and fields[2] != "Z"


def run_failure_cleanup(binary: Path) -> None:
    """Prove a failed fixture does not leave its mock child or pipes behind."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-runtime-fail-",
                                     dir="/tmp/opencode") as directory:
        bus, address = start_bus(Path(directory))
        pidfile = Path(directory) / "child.pid"
        env = dict(os.environ)
        env["DBUS_SESSION_BUS_ADDRESS"] = "autolaunch:do-not-use"
        env["DBUS_SYSTEM_BUS_ADDRESS"] = "autolaunch:do-not-use"
        env["CP_RUNTIME_DEBUG"] = "1"
        env["CP_RUNTIME_CHILD_PID_FILE"] = str(pidfile)
        try:
            process = subprocess.Popen(
                [str(binary), address, "failchild"], env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, start_new_session=True)
            timed_out = False
            try:
                stdout, stderr = process.communicate(timeout=2)
            except subprocess.TimeoutExpired:
                timed_out = True
                kill_fixture_group(process)
                stdout, stderr = process.communicate(timeout=10)
            finally:
                # Always target only the fixture's own PGID, never a host
                # process or a broad name/pattern match.
                kill_fixture_group(process)
        finally:
            stop_bus(bus)
        assert timed_out, (
            "failchild did not hold the captured pipes; test did not exercise "
            "the leak-hang path")
        assert process.returncode == 7, (process.returncode, stdout, stderr)
        child = int(pidfile.read_text().strip())
        for _ in range(20):
            if not pid_alive(child):
                break
            time.sleep(0.1)
        assert not pid_alive(child), f"fixture mock child {child} survived"


def main() -> int:
    cflags, libs = require_tools()
    with tempfile.TemporaryDirectory(prefix="wifi1b-runtime-build-",
                                     dir="/tmp/opencode") as directory:
        build = Path(directory)
        binary = build / "test_runtime_broker"
        compile_binary(binary, cflags, libs, sanitize=False)
        for scenario in SCENARIOS:
            run_scenario(binary, scenario, sanitize=False)
            print(f"PASS: runtime broker scenario {scenario}")
        run_failure_cleanup(binary)
        print("PASS: runtime broker failed-fixture process-group cleanup")

        asan = build / "test_runtime_broker_asan"
        compile_binary(asan, cflags, libs, sanitize=True)
        for scenario in SCENARIOS:
            run_scenario(asan, scenario, sanitize=True)
            print(f"PASS: runtime broker ASan/UBSan scenario {scenario}")
        run_scenario(asan, "leakinit", sanitize=True, leak_check=True)
        print("PASS: runtime broker ASan leak check (init/destroy)")

    print("PASS: runtime broker private-bus, hook RX, helper auth and lifecycle")
    return 0


if __name__ == "__main__":
    sys.exit(main())
