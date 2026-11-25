#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Independent build and transport tests for the C3 MFi adapter.

Everything here uses first-party mock .so files and mock Unix servers.  No
OEM library is loaded or executed on the host, and outputs stay in
/tmp/opencode.  The target clean build is verified separately by the build
script against the owner's rootfs; this test exercises its hash/symbol gates
with mocks.
"""

from __future__ import annotations

import errno
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from unittest import mock

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "carplay"
TEST_C = ROOT / "tests" / "test_carplay_oem_auth.c"
BUILD_SCRIPT = ROOT / "scripts" / "build_carplay_native.py"
PY_TEST = Path(__file__).resolve()

MOCK_SHA_PLACEHOLDER = "0" * 64
MAGIC = 0x53434131
VERSION = 1
OP_CERT = 1
OP_SIGN = 2
HDR_REQ = ">IHHIII"
HDR_RESP = ">IHHIIII"

DANGEROUS_ENV = (
    "LD_PRELOAD", "LD_AUDIT", "LD_DEBUG", "LD_PROFILE", "LD_LIBRARY_PATH",
    "LD_USE_LOAD_BIAS", "LD_ORIGIN_PATH", "LD_ASSUME_KERNEL",
    "LD_TRACE_LOADED_OBJECTS", "LD_WARN", "LD_VERBOSE", "LD_BIND_NOT",
    "LD_PREFER_MAP_32BIT_EXEC", "LD_HWCAP_MASK", "LD_SHOW_AUXV",
    "LD_DYNAMIC_WEAK",
)


def helper_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = dict(os.environ)
    for key in DANGEROUS_ENV:
        env.pop(key, None)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    if extra:
        env.update(extra)
    return env


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=False, **kwargs)


def checked(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    result = run(command, **kwargs)
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
    return result


def load_build_module():
    spec = importlib.util.spec_from_file_location("build_carplay_native",
                                                   BUILD_SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


MOCK_HEADER = r"""
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static int env_int(const char *name, int fallback) {
    const char *v = getenv(name);
    return v != NULL ? atoi(v) : fallback;
}

__attribute__((constructor)) static void marker(void) {
    const char *path = getenv("CP_MOCK_MARKER");
    if (path != NULL) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { (void)write(fd, "loaded", 6); (void)close(fd); }
    }
}

int MFiPlatform_Initialize(void) {
    const char *count = getenv("CP_MOCK_INIT_COUNT");
    const char *cwd_path = getenv("CP_MOCK_CWD");
    if (count != NULL) {
        int fd = open(count, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) { (void)write(fd, "I", 1); (void)close(fd); }
    }
    if (cwd_path != NULL) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            int fd = open(cwd_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd >= 0) {
                (void)write(fd, cwd, strlen(cwd));
                (void)close(fd);
            }
        }
    }
    return env_int("CP_MOCK_INIT_RC", 0);
}

int MFiPlatform_CopyCertificate(void **out, size_t *out_len) {
    int rc = env_int("CP_MOCK_CERT_RC", 0);
    size_t n = (size_t)env_int("CP_MOCK_CERT_LEN", 64);
    unsigned char *p;
    size_t i;
    if (rc != 0) return rc;
    if (env_int("CP_MOCK_CERT_NULL", 0)) { *out = NULL; *out_len = 0; return 0; }
    p = malloc(n == 0 ? 1 : n);
    if (p == NULL) return -12;
    for (i = 0; i < n; i++) p[i] = (unsigned char)(0xA0u + (i & 0x1Fu));
    *out = p; *out_len = n; return 0;
}
"""

MOCK_SIGNATURE = r"""
int MFiPlatform_CreateSignature(const void *digest, size_t digest_len,
                                void **out, size_t *out_len) {
    int rc = env_int("CP_MOCK_SIGN_RC", 0);
    size_t n = (size_t)env_int("CP_MOCK_SIGN_LEN", 32);
    unsigned char *p;
    size_t i;
    (void)digest;
    if (digest_len != 20 && digest_len != 32) return -22;
    if (rc != 0) return rc;
    if (env_int("CP_MOCK_SIGN_NULL", 0)) { *out = NULL; *out_len = 0; return 0; }
    p = malloc(n == 0 ? 1 : n);
    if (p == NULL) return -12;
    for (i = 0; i < n; i++) p[i] = (unsigned char)(0x5Au ^ (i & 0x0Fu));
    *out = p; *out_len = n; return 0;
}
"""


def build_mock_so(tmp: Path, name: str = "mock_mfi.so",
                  with_signature: bool = True) -> Path:
    source = tmp / (name + ".c")
    source.write_text(MOCK_HEADER + (MOCK_SIGNATURE if with_signature else ""),
                      encoding="utf-8")
    output = tmp / name
    checked(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-pedantic", "-fPIC", "-shared", str(source), "-o", str(output)],
            text=True, capture_output=True, timeout=120)
    return output


def build_host_helper(tmp: Path) -> Path:
    output = tmp / "cp_native_auth_host"
    checked(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-pedantic", "-DCP_OEM_AUTH_TEST", "-I", str(SRC),
             str(SRC / "cp_native_auth.c"), "-o", str(output), "-ldl"],
            text=True, capture_output=True, timeout=120)
    return output


def build_host_start_helper(tmp: Path) -> Path:
    object_file = tmp / "cp_native_auth_start.o"
    start_file = tmp / "cp_native_start_host.o"
    output = tmp / "cp_native_auth_host_start"
    checked(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-pedantic", "-DCP_OEM_AUTH_TEST", "-fno-pie",
             "-fstack-protector-strong", "-march=x86-64",
             "-mtune=generic", "-fcf-protection=none", "-I", str(SRC),
             "-c", str(SRC / "cp_native_auth.c"), "-o", str(object_file)],
            text=True, capture_output=True, timeout=120)
    checked(["gcc", "-c", "-fno-pie", str(SRC / "cp_native_start.S"),
             "-o", str(start_file)], text=True, capture_output=True,
            timeout=120)
    interpreter = "/lib64/ld-linux-x86-64.so.2"
    if not Path(interpreter).exists():
        interpreter = "/lib/ld-linux-x86-64.so.2"
    checked(["gcc", "-nostdlib", "-no-pie", "-Wl,-e,_start",
             f"-Wl,--dynamic-linker={interpreter}", "-Wl,-z,relro,-z,now",
             "-Wl,-z,noexecstack", "-o", str(output), str(start_file),
             str(object_file), "-lc", "-ldl"], text=True,
            capture_output=True, timeout=120)
    return output


def build_test_binary(tmp: Path) -> Path:
    output = tmp / "test_carplay_oem_auth"
    checked(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", "-pedantic", "-DCP_OEM_AUTH_TEST",
             "-pthread", "-I", str(SRC), str(TEST_C),
             str(SRC / "cp_oem_auth.c"), str(SRC / "cp_wire.c"),
             str(SRC / "cp_bootstrap.c"), "-o", str(output)],
            text=True, capture_output=True, timeout=120)
    return output


def strict_compile_sources(tmp: Path) -> None:
    common = ["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
              "-pedantic", "-I", str(SRC), "-c"]
    checked([*common, str(SRC / "cp_oem_auth.c"), "-o",
             str(tmp / "cp_oem_auth.o")], text=True,
            capture_output=True, timeout=120)
    checked([*common, "-DCP_OEM_AUTH_TEST", str(SRC / "cp_oem_auth.c"),
             "-o", str(tmp / "cp_oem_auth_test.o")], text=True,
            capture_output=True, timeout=120)
    checked([*common, str(SRC / "cp_native_auth.c"), "-o",
             str(tmp / "cp_native_auth.o")], text=True,
            capture_output=True, timeout=120)
    checked([*common, "-DCP_OEM_AUTH_TEST", str(SRC / "cp_native_auth.c"),
             "-o", str(tmp / "cp_native_auth_test.o")], text=True,
            capture_output=True, timeout=120)
    checked(["gcc", "-c", "-fno-pie", str(SRC / "cp_native_start.S"),
             "-o", str(tmp / "cp_native_start.o")], text=True,
            capture_output=True, timeout=120)


def sha256_file(path: Path) -> str:
    import hashlib
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def wait_socket(path: Path, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.02)
    raise AssertionError(f"socket did not appear: {path}")


def start_helper(helper: Path, library: Path, library_sha: str,
                 socket_path: Path, marker: Path | None,
                 env_extra: dict[str, str] | None = None,
                 once: bool = True) -> subprocess.Popen:
    env = helper_env(env_extra)
    if marker is not None:
        env["CP_MOCK_MARKER"] = str(marker)
    command = [str(helper), "--serve", "--experimental", "--library",
               str(library), "--expected-sha256", library_sha, "--socket",
               str(socket_path), "--timeout-ms", "1000"]
    if once:
        command.append("--once")
    return subprocess.Popen(command, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def helper_exit(process: subprocess.Popen, timeout: float = 5.0) -> tuple[int, str, str]:
    out, err = process.communicate(timeout=timeout)
    return process.returncode, out, err


def rpc_client(binary: Path, socket_path: Path, mode: str,
               timeout_ms: int | None = None,
               cap: int | None = None) -> subprocess.CompletedProcess:
    command = [str(binary), "--rpc-client", str(socket_path), mode]
    if timeout_ms is not None or cap is not None:
        command.append(str(timeout_ms if timeout_ms is not None else 1000))
    if cap is not None:
        command.append(str(cap))
    return run(command, text=True, capture_output=True, timeout=15)


def parse_client_output(output: str) -> dict[str, str]:
    values = {}
    for line in output.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
    return values


def test_build_gates(build_module, tmp: Path) -> None:
    mock = build_mock_so(tmp)
    mock_sha = sha256_file(mock)
    symbols = build_module.elf_symbols(mock)
    expected = {}
    for name in ("MFiPlatform_Initialize", "MFiPlatform_CopyCertificate",
                 "MFiPlatform_CreateSignature"):
        assert name in symbols, name
        expected[name] = int(symbols[name]["value"])
    core = build_module.verify_coreutils(mock, mock_sha, expected)
    assert core["sha256"] == mock_sha
    try:
        build_module.verify_coreutils(mock, MOCK_SHA_PLACEHOLDER, expected)
    except build_module.BuildError:
        pass
    else:
        raise AssertionError("hash refusal did not trigger")

    bad = build_mock_so(tmp, "mock_missing_sign.so", with_signature=False)
    bad_sha = sha256_file(bad)
    bad_symbols = build_module.elf_symbols(bad)
    bad_expected = {
        "MFiPlatform_Initialize":
            int(bad_symbols["MFiPlatform_Initialize"]["value"]),
        "MFiPlatform_CopyCertificate":
            int(bad_symbols["MFiPlatform_CopyCertificate"]["value"]),
        "MFiPlatform_CreateSignature": expected["MFiPlatform_CreateSignature"],
    }
    try:
        build_module.verify_coreutils(bad, bad_sha, bad_expected)
    except build_module.BuildError:
        pass
    else:
        raise AssertionError("missing-symbol refusal did not trigger")

    wrong_address = dict(expected)
    wrong_address["MFiPlatform_CreateSignature"] += 1
    try:
        build_module.verify_coreutils(mock, mock_sha, wrong_address)
    except build_module.BuildError:
        pass
    else:
        raise AssertionError("wrong-address refusal did not trigger")

    pinned = build_module.parse_expected_symbols()
    assert pinned["MFiPlatform_Initialize"] == 0x8129F
    assert pinned["MFiPlatform_CopyCertificate"] == 0x80D6C
    assert pinned["MFiPlatform_CreateSignature"] == 0x8068A

    safe_out = tmp / "out-safe"
    build_module.ensure_output_dir(safe_out)
    assert safe_out.is_dir()
    build_module.check_output_targets(safe_out)

    victim = tmp / "victim.bin"
    victim.write_bytes(b"do not overwrite")
    target = safe_out / "carplay-native-auth"
    target.symlink_to(victim)
    try:
        build_module.check_output_targets(safe_out)
    except build_module.BuildError:
        pass
    else:
        raise AssertionError("symlink output alias was accepted")
    target.unlink()

    os.link(victim, target)
    try:
        build_module.check_output_targets(safe_out)
    except build_module.BuildError:
        pass
    else:
        raise AssertionError("hardlink output alias was accepted")
    target.unlink()

    target.write_bytes(b"existing")
    try:
        build_module.check_output_targets(safe_out)
    except build_module.BuildError:
        pass
    else:
        raise AssertionError("existing regular output was accepted")
    assert target.read_bytes() == b"existing"
    target.unlink()

    source = tmp / "publish-source"
    source.write_bytes(b"new build")
    target.write_bytes(b"unknown existing output")
    try:
        build_module.publish_no_clobber(source, target)
    except build_module.BuildError:
        pass
    else:
        raise AssertionError("publish_no_clobber overwrote an existing file")
    assert target.read_bytes() == b"unknown existing output"
    target.unlink()

    victim = tmp / "publish-victim"
    victim.write_bytes(b"victim")
    target.symlink_to(victim)
    try:
        build_module.publish_no_clobber(source, target)
    except build_module.BuildError:
        pass
    else:
        raise AssertionError("publish_no_clobber followed a symlink target")
    assert victim.read_bytes() == b"victim"
    assert target.is_symlink()
    target.unlink()

    real_out = tmp / "out-real"
    real_out.mkdir(mode=0o700)
    linked_out = tmp / "out-linked"
    linked_out.symlink_to(real_out)
    try:
        build_module.ensure_output_dir(linked_out)
    except build_module.BuildError:
        pass
    else:
        raise AssertionError("symlink output directory was accepted")

    writable_out = tmp / "out-writable"
    writable_out.mkdir(mode=0o700)
    os.chmod(writable_out, 0o777)
    try:
        build_module.ensure_output_dir(writable_out)
    except build_module.BuildError:
        pass
    else:
        raise AssertionError("world-writable output directory was accepted")

    try:
        build_module.ensure_output_dir(
            ROOT / "src" / "carplay" / "must-not-be-written")
    except build_module.BuildError:
        pass
    else:
        raise AssertionError("unignored output path was accepted")

    wrapper = tmp / "cc-wrapper"
    wrapper.write_text("#!/bin/sh\nexec gcc -DCP_OEM_AUTH_TEST \"$@\"\n",
                       encoding="utf-8")
    os.chmod(wrapper, 0o755)
    for compiler in (str(wrapper), "gcc -DCP_OEM_AUTH_TEST"):
        try:
            build_module.verify_compiler(compiler)
        except build_module.BuildError:
            pass
        else:
            raise AssertionError("test-define compiler wrapper was accepted")
    assert build_module.verify_compiler("gcc") == "gcc"


def test_helper_cli(helper: Path, mock: Path, mock_sha: str,
                    tmp: Path) -> None:
    no_args = run([str(helper)], text=True, capture_output=True, timeout=10)
    assert no_args.returncode == 2, no_args

    no_optin_socket = tmp / "no-optin" / "auth.sock"
    marker = tmp / "no-optin.marker"
    no_optin = run([str(helper), "--serve", "--library", str(mock),
                    "--expected-sha256", mock_sha, "--socket",
                    str(no_optin_socket)], text=True, capture_output=True,
                   timeout=10, env=helper_env({"CP_MOCK_MARKER": str(marker)}))
    assert no_optin.returncode == 3, no_optin
    assert not no_optin_socket.exists()
    assert not marker.exists()

    bad_hash_socket = tmp / "bad-hash" / "auth.sock"
    marker = tmp / "bad-hash.marker"
    bad_hash = run([str(helper), "--serve", "--experimental", "--library",
                    str(mock), "--expected-sha256", MOCK_SHA_PLACEHOLDER,
                    "--socket", str(bad_hash_socket), "--once"], text=True,
                   capture_output=True, timeout=10,
                   env=helper_env({"CP_MOCK_MARKER": str(marker)}))
    assert bad_hash.returncode == 5, bad_hash
    assert not bad_hash_socket.exists()
    assert not marker.exists()

    missing = build_mock_so(tmp, "missing_sign.so", with_signature=False)
    missing_sha = sha256_file(missing)
    symbol_socket = tmp / "symbol" / "auth.sock"
    marker = tmp / "symbol.marker"
    symbol = run([str(helper), "--serve", "--experimental", "--library",
                  str(missing), "--expected-sha256", missing_sha,
                  "--socket", str(symbol_socket), "--once"], text=True,
                 capture_output=True, timeout=10,
                 env=helper_env({"CP_MOCK_MARKER": str(marker)}))
    assert symbol.returncode == 6, symbol
    assert not symbol_socket.exists()
    assert marker.exists()  # dlopen happened, dlsym refused

    bad_dir = tmp / "bad-helper-dir"
    bad_dir.mkdir(mode=0o755)
    os.chmod(bad_dir, 0o755)
    bad_dir_socket = bad_dir / "auth.sock"
    marker = tmp / "bad-dir.marker"
    bad_dir_result = run([str(helper), "--serve", "--experimental",
                          "--library", str(mock), "--expected-sha256",
                          mock_sha, "--socket", str(bad_dir_socket),
                          "--once"], text=True, capture_output=True,
                         timeout=10,
                         env=helper_env({"CP_MOCK_MARKER": str(marker)}))
    assert bad_dir_result.returncode == 3, bad_dir_result
    assert not bad_dir_socket.exists()
    assert not marker.exists()  # path validation precedes dlopen

    existing_dir = tmp / "existing-socket"
    existing_dir.mkdir(mode=0o700)
    existing_path = existing_dir / "auth.sock"
    existing = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    existing.bind(str(existing_path))
    os.chmod(existing_path, 0o600)
    existing.listen(1)
    marker = tmp / "existing.marker"
    existing_result = run([str(helper), "--serve", "--experimental",
                           "--library", str(mock), "--expected-sha256",
                           mock_sha, "--socket", str(existing_path),
                           "--once"], text=True, capture_output=True,
                          timeout=10,
                          env=helper_env({"CP_MOCK_MARKER": str(marker)}))
    assert existing_result.returncode == 3, existing_result
    assert existing_path.exists()
    existing.close()
    existing_path.unlink()
    existing_dir.rmdir()

    for name, extra in (
            ("ld-preload", {"LD_PRELOAD": "/tmp/not-a-real-preload.so"}),
            ("ld-relative", {"LD_LIBRARY_PATH": "relative"}),
            ("ld-other", {"LD_LIBRARY_PATH": "/tmp/opencode"})):
        env_socket = tmp / ("env-" + name) / "auth.sock"
        marker = tmp / ("env-" + name + ".marker")
        env_extra = dict(extra)
        env_extra["CP_MOCK_MARKER"] = str(marker)
        result = run([str(helper), "--serve", "--experimental", "--library",
                      str(mock), "--expected-sha256", mock_sha, "--socket",
                      str(env_socket), "--once"], text=True,
                     capture_output=True, timeout=10,
                     env=helper_env(env_extra))
        assert result.returncode == 3, (name, result)
        assert not env_socket.exists()
        assert not marker.exists()


def test_helper_rpc(helper: Path, mock: Path, mock_sha: str,
                    client: Path, tmp: Path) -> None:
    run_dir = tmp / "rpc-run"
    socket_path = run_dir / "auth.sock"
    marker = tmp / "rpc.marker"
    cwd_file = tmp / "rpc.cwd"
    process = start_helper(helper, mock, mock_sha, socket_path, marker,
                           env_extra={"CP_MOCK_CWD": str(cwd_file)})
    wait_socket(socket_path)
    assert (run_dir.stat().st_mode & 0o777) == 0o700
    assert (socket_path.stat().st_mode & 0o777) == 0o600
    assert run_dir.stat().st_uid == os.geteuid()
    assert socket_path.stat().st_uid == os.geteuid()
    result = rpc_client(client, socket_path, "both")
    assert result.returncode == 0, (result.stdout, result.stderr)
    values = parse_client_output(result.stdout)
    assert values.get("cert_len") == "64"
    assert values.get("sig_len") == "32"
    assert values["cert_hex"].startswith("a0a1a2a3")
    assert values["sig_hex"].startswith("5a5b5859")
    rc, out, err = helper_exit(process)
    assert rc == 0, (rc, out, err)
    assert not socket_path.exists()
    assert not run_dir.exists()
    assert cwd_file.read_text(encoding="utf-8") == "/"

    # The same path can be restarted after clean shutdown.
    process = start_helper(helper, mock, mock_sha, socket_path,
                           tmp / "rpc-restart.marker")
    wait_socket(socket_path)
    result = rpc_client(client, socket_path, "cert")
    assert result.returncode == 0, (result.stdout, result.stderr)
    rc, out, err = helper_exit(process)
    assert rc == 0, (rc, out, err)
    assert not socket_path.exists()
    assert not run_dir.exists()

    # Extra files are not deleted by the helper's cleanup.
    extra_dir = tmp / "rpc-extra"
    extra_dir.mkdir(mode=0o700)
    extra_file = extra_dir / "keep.txt"
    extra_file.write_text("keep", encoding="utf-8")
    socket_path = extra_dir / "auth.sock"
    process = start_helper(helper, mock, mock_sha, socket_path,
                           tmp / "rpc-extra.marker")
    wait_socket(socket_path)
    result = rpc_client(client, socket_path, "cert")
    assert result.returncode == 0, (result.stdout, result.stderr)
    rc, out, err = helper_exit(process)
    assert rc == 0, (rc, out, err)
    assert not socket_path.exists()
    assert extra_file.read_text(encoding="utf-8") == "keep"
    assert extra_dir.is_dir()


def test_helper_failures(helper: Path, mock: Path, mock_sha: str,
                         client: Path, tmp: Path) -> None:
    def one(env_extra: dict[str, str], mode: str,
            expected: int) -> subprocess.CompletedProcess:
        socket_path = tmp / ("fail-" + mode + "-" + str(expected)) / "auth.sock"
        process = start_helper(helper, mock, mock_sha, socket_path,
                               None, env_extra=env_extra)
        wait_socket(socket_path)
        result = rpc_client(client, socket_path, mode)
        rc, out, err = helper_exit(process)
        assert rc == 0, (rc, out, err)
        assert result.returncode == expected, (result.stdout, result.stderr)
        return result

    result = one({"CP_MOCK_CERT_RC": "8"}, "cert", 8)
    assert parse_client_output(result.stdout).get("cert_error") == "8"
    result = one({"CP_MOCK_SIGN_RC": "10"}, "sign", 10)
    assert parse_client_output(result.stdout).get("sig_error") == "10"
    result = one({"CP_MOCK_CERT_NULL": "1"}, "cert", 8)
    assert parse_client_output(result.stdout).get("cert_error") == "8"


def parse_helper_response(data: bytes) -> tuple[int, bytes]:
    assert len(data) >= 24, data
    magic, version, op, request_id, status, payload_len, reserved = \
        struct.unpack(HDR_RESP, data[:24])
    del op, request_id
    assert magic == MAGIC and version == VERSION and reserved == 0
    assert len(data) == 24 + payload_len
    return status, data[24:]


def helper_request(connection: socket.socket, op: int, request_id: int,
                   payload: bytes = b"") -> tuple[int, bytes]:
    connection.send(struct.pack(HDR_REQ, MAGIC, VERSION, op, request_id,
                                len(payload), 0) + payload)
    return parse_helper_response(connection.recv(8192))


def test_helper_init_latch(helper: Path, mock: Path, mock_sha: str,
                           tmp: Path) -> None:
    run_dir = tmp / "init-latch"
    socket_path = run_dir / "auth.sock"
    count_file = tmp / "init-latch.count"
    process = start_helper(
        helper, mock, mock_sha, socket_path, None,
        env_extra={"CP_MOCK_INIT_RC": "7",
                   "CP_MOCK_INIT_COUNT": str(count_file)},
        once=False)
    wait_socket(socket_path)

    first = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    first.settimeout(3.0)
    first.connect(str(socket_path))
    status, payload = helper_request(first, OP_CERT, 1)
    assert (status, payload) == (7, b"")
    status, payload = helper_request(first, OP_SIGN, 2, bytes(20))
    assert (status, payload) == (7, b"")
    first.close()

    second = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    second.settimeout(3.0)
    second.connect(str(socket_path))
    status, payload = helper_request(second, OP_SIGN, 3, bytes(20))
    assert (status, payload) == (7, b"")
    status, payload = helper_request(second, OP_CERT, 4)
    assert (status, payload) == (7, b"")
    second.close()

    assert count_file.read_bytes() == b"I"
    process.terminate()
    try:
        out, err = process.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        out, err = process.communicate(timeout=5)
        raise AssertionError(f"helper did not stop: {out}\n{err}")
    assert not socket_path.exists()
    assert not run_dir.exists()


def test_helper_malformed(helper: Path, mock: Path, mock_sha: str,
                          tmp: Path) -> None:
    def launch(tag: str) -> tuple[subprocess.Popen, Path, Path]:
        run_dir = tmp / ("malformed-" + tag)
        socket_path = run_dir / "auth.sock"
        process = start_helper(helper, mock, mock_sha, socket_path, None)
        wait_socket(socket_path)
        return process, run_dir, socket_path

    process, run_dir, socket_path = launch("magic")
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    sock.connect(str(socket_path))
    sock.send(struct.pack(HDR_REQ, 0x00000000, VERSION, OP_CERT, 1, 0, 0))
    assert sock.recv(1024) == b""
    sock.close()
    rc, out, err = helper_exit(process)
    assert rc == 0, (rc, out, err)
    assert not socket_path.exists()
    assert not run_dir.exists()

    process, run_dir, socket_path = launch("short-sign")
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    sock.connect(str(socket_path))
    sock.send(struct.pack(HDR_REQ, MAGIC, VERSION, OP_SIGN, 2, 19, 0) +
              bytes(19))
    assert sock.recv(1024) == b""
    sock.close()
    rc, out, err = helper_exit(process)
    assert rc == 0, (rc, out, err)

    process, run_dir, socket_path = launch("oversize")
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    sock.connect(str(socket_path))
    sock.send(bytes(9000))
    assert sock.recv(1024) == b""
    sock.close()
    rc, out, err = helper_exit(process)
    assert rc == 0, (rc, out, err)


class OneShotServer(threading.Thread):
    def __init__(self, directory: Path, handler, dir_mode: int = 0o700,
                 sock_mode: int = 0o600):
        super().__init__(daemon=True)
        self.directory = directory
        self.handler = handler
        self.dir_mode = dir_mode
        self.sock_mode = sock_mode
        self.path = directory / "auth.sock"
        self.error: BaseException | None = None
        self.directory.mkdir(mode=dir_mode, parents=True, exist_ok=True)
        os.chmod(self.directory, dir_mode)
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.server.bind(str(self.path))
        os.chmod(self.path, sock_mode)
        self.server.listen(1)

    def run(self) -> None:
        try:
            connection, _ = self.server.accept()
            try:
                self.handler(connection)
            finally:
                connection.close()
        except BaseException as error:  # pragma: no cover - reported below
            self.error = error
        finally:
            self.server.close()

    def close(self) -> None:
        try:
            self.server.close()
        except OSError:
            pass
        try:
            self.path.unlink()
        except OSError:
            pass


def server_response_for(request: bytes, *, magic: int = MAGIC,
                        version: int = VERSION, status: int = 0,
                        payload: bytes = b"", request_id: int | None = None,
                        payload_len: int | None = None,
                        op: int | None = None) -> bytes:
    req_magic, req_version, req_op, req_id, req_len, req_reserved = \
        struct.unpack(HDR_REQ, request[:20])
    del req_magic, req_version, req_reserved
    if request_id is None:
        request_id = req_id
    if op is None:
        op = req_op
    if payload_len is None:
        payload_len = len(payload)
    header = struct.pack(HDR_RESP, magic, version, op, request_id, status,
                         payload_len, 0)
    return header + payload


def test_malicious_servers(client: Path, tmp: Path) -> None:
    def case(name: str, handler, expected: int, timeout_ms: int = 500) -> None:
        directory = tmp / ("server-" + name)
        server = OneShotServer(directory, handler)
        server.start()
        result = rpc_client(client, server.path, "cert", timeout_ms)
        server.join(timeout=5)
        server.close()
        assert server.error is None, (name, server.error)
        assert result.returncode == expected, (
            name, result.returncode, result.stdout, result.stderr)

    def valid(connection: socket.socket) -> None:
        request = connection.recv(8192)
        connection.send(server_response_for(request, payload=b"cert"))

    case("valid", valid, 0)

    def wrong_magic(connection: socket.socket) -> None:
        request = connection.recv(8192)
        connection.send(server_response_for(request, magic=0x12345678))

    case("wrong-magic", wrong_magic, 16)

    def wrong_version(connection: socket.socket) -> None:
        request = connection.recv(8192)
        connection.send(server_response_for(request, version=2))

    case("wrong-version", wrong_version, 16)

    def wrong_id(connection: socket.socket) -> None:
        request = connection.recv(8192)
        connection.send(server_response_for(request, request_id=999))

    case("wrong-id", wrong_id, 16)

    def wrong_op(connection: socket.socket) -> None:
        request = connection.recv(8192)
        connection.send(server_response_for(request, op=OP_SIGN))

    case("wrong-op", wrong_op, 16)

    def status_cert(connection: socket.socket) -> None:
        request = connection.recv(8192)
        connection.send(server_response_for(request, status=8))

    case("status-cert", status_cert, 8)

    def status_internal(connection: socket.socket) -> None:
        request = connection.recv(8192)
        connection.send(server_response_for(request, status=17))

    case("status-internal", status_internal, 17)

    def status_out_of_range(connection: socket.socket) -> None:
        request = connection.recv(8192)
        connection.send(server_response_for(request, status=99))

    case("status-range", status_out_of_range, 16)

    def truncated_payload(connection: socket.socket) -> None:
        request = connection.recv(8192)
        connection.send(server_response_for(request, payload_len=4))

    case("truncated", truncated_payload, 16)

    def oversize_payload(connection: socket.socket) -> None:
        request = connection.recv(8192)
        connection.send(server_response_for(request, payload_len=4097))

    case("oversize", oversize_payload, 16)

    def close_early(connection: socket.socket) -> None:
        _ = connection.recv(8192)

    case("close", close_early, 13)

    def timeout(connection: socket.socket) -> None:
        _ = connection.recv(8192)
        time.sleep(0.5)

    case("timeout", timeout, 14, timeout_ms=100)

    # Path policy failures happen before connect; no server is needed.
    bad_dir = tmp / "bad-dir"
    bad_dir.mkdir(mode=0o755)
    os.chmod(bad_dir, 0o755)
    server = OneShotServer(bad_dir / "unused", lambda _: None,
                           dir_mode=0o755)
    server.start()
    result = rpc_client(client, server.path, "cert", 200)
    assert result.returncode == 15, (result.stdout, result.stderr)
    server.close()
    server.join(timeout=2)

    bad_socket_dir = tmp / "bad-sock"
    server = OneShotServer(bad_socket_dir, lambda _: None,
                           sock_mode=0o644)
    server.start()
    result = rpc_client(client, server.path, "cert", 200)
    assert result.returncode == 15, (result.stdout, result.stderr)
    server.close()
    server.join(timeout=2)


def test_client_total_deadline(client: Path, tmp: Path) -> None:
    def slow(connection: socket.socket) -> None:
        _ = connection.recv(8192)
        time.sleep(0.5)

    server = OneShotServer(tmp / "total-deadline", slow)
    server.start()
    started = time.monotonic()
    result = rpc_client(client, server.path, "cert", 100)
    elapsed = time.monotonic() - started
    server.join(timeout=5)
    server.close()
    assert result.returncode == 14, (result.stdout, result.stderr)
    assert elapsed < 0.35, elapsed
    assert elapsed >= 0.05, elapsed


def test_client_error_order_and_cap(client: Path, tmp: Path) -> None:
    def case(name: str, handler, expected: int, mode: str = "both",
             cap: int | None = None) -> dict[str, str]:
        server = OneShotServer(tmp / name, handler)
        server.start()
        result = rpc_client(client, server.path, mode, None, cap)
        server.join(timeout=5)
        server.close()
        if server.error is not None:
            raise AssertionError(f"{name}: server error {server.error!r}")
        assert result.returncode == expected, (
            name, result.returncode, result.stdout, result.stderr)
        return parse_client_output(result.stdout)

    def cert_then_sign(cert_status: int, sign_status: int):
        def handler(connection: socket.socket) -> None:
            request = connection.recv(8192)
            cert_payload = b"c" * 64 if cert_status == 0 else b""
            connection.send(server_response_for(request, status=cert_status,
                                                payload=cert_payload))
            second = connection.recv(8192)
            if sign_status == 0:
                connection.send(server_response_for(second,
                                                    payload=b"s" * 32))
            else:
                connection.send(server_response_for(second,
                                                    status=sign_status))
        return handler

    fields = case("both-certfail-signok", cert_then_sign(8, 0), 8)
    assert fields["cert_error"] == "8"
    assert fields["sig_len"] == "32"
    assert "cert_len" not in fields

    fields = case("both-bothfail", cert_then_sign(8, 10), 8)
    assert fields["cert_error"] == "8"
    assert fields["sig_error"] == "10"

    fields = case("both-signfail", cert_then_sign(0, 10), 10)
    assert fields["cert_len"] == "64"
    assert fields["sig_error"] == "10"

    def cert_size(size: int):
        def handler(connection: socket.socket) -> None:
            request = connection.recv(8192)
            connection.send(server_response_for(request,
                                                payload=b"x" * size))
        return handler

    fields = case("cap-2048", cert_size(2048), 0, mode="cert", cap=2048)
    assert fields["cert_len"] == "2048"

    for size in (2049, 4096):
        fields = case(f"cap-{size}", cert_size(size), 11, mode="cert",
                      cap=2048)
        assert fields["cert_error"] == "11"
        assert "cert_len" not in fields
        assert "cert_hex" not in fields


def test_build_rollback_and_cleanup(build_module, tmp: Path) -> None:
    def fake_compile(rootfs, work, cc):
        del rootfs, cc
        (work / "cp_native_auth.o").write_bytes(b"object")
        (work / "cp_native_start.o").write_bytes(b"start")
        (work / "carplay-native-auth").write_bytes(b"helper")
        return {
            "path": str(work / "carplay-native-auth"),
            "compile_flags": [],
            "link_flags": [],
        }

    def fake_verify(path):
        data = path.read_bytes()
        return {
            "path": str(path),
            "sha256": hashlib.sha256(data).hexdigest(),
            "size": len(data),
        }

    def fake_toolchain():
        return mock.patch.multiple(
            build_module,
            compile_helper=fake_compile,
            verify_helper=fake_verify,
            verify_compiler=lambda cc: cc,
        )

    def build(out: Path) -> dict[str, object]:
        result: dict[str, object] = {}
        build_module.build_and_publish(
            Path("/unused-first-party-fixture"), out, "gcc", result)
        return result

    real_publish = build_module.publish_no_clobber
    real_link = build_module.os.link
    real_unlink = build_module.os.unlink
    real_open = build_module.os.open
    real_fstat = build_module.os.fstat
    real_close = build_module.os.close

    # Second publication fails with a real ENOSPC error: the first output must
    # be rolled back and the private work directory removed.
    out = tmp / "fault-enospc"
    link_calls = []

    def link_enospc(source, target, *args, **kwargs):
        link_calls.append(target)
        if len(link_calls) == 2:
            raise OSError(errno.ENOSPC, "injected ENOSPC", str(target))
        return real_link(source, target, *args, **kwargs)

    with fake_toolchain(), mock.patch.object(
            build_module.os, "link", link_enospc):
        try:
            build(out)
        except build_module.BuildError as error:
            message = str(error)
        else:
            raise AssertionError("second publish ENOSPC was accepted")
    assert "cannot publish" in message
    assert "ENOSPC" in message or "No space left" in message
    assert "rollback incomplete" not in message
    assert not (out / "carplay-native-auth").exists()
    assert not (out / "build-metadata.json").exists()
    assert not list(out.glob(".work-*"))

    # Rollback cannot unlink the owned helper: report the remaining path and
    # retain the private work instead of silently claiming a clean refusal.
    out = tmp / "fault-rollback-eacces"
    link_calls = []

    def link_second_failure(source, target, *args, **kwargs):
        link_calls.append(target)
        if len(link_calls) == 2:
            raise OSError(errno.ENOSPC, "injected ENOSPC", str(target))
        return real_link(source, target, *args, **kwargs)

    def deny_published_unlink(path, *args, **kwargs):
        if Path(path) == out / "carplay-native-auth":
            raise PermissionError(
                errno.EACCES, "injected rollback denial", str(path))
        return real_unlink(path, *args, **kwargs)

    with fake_toolchain(), mock.patch.object(
            build_module.os, "link", link_second_failure), \
            mock.patch.object(
                build_module.os, "unlink", deny_published_unlink):
        try:
            build(out)
        except build_module.BuildError as error:
            message = str(error)
        else:
            raise AssertionError("rollback permission denial was hidden")
    assert "cannot publish" in message
    assert "ENOSPC" in message or "No space left" in message
    assert "rollback incomplete" in message
    assert "cannot remove owned published output" in message
    assert str(out / "carplay-native-auth") in message
    assert (out / "carplay-native-auth").exists()
    assert not (out / "build-metadata.json").exists()
    work_dirs = list(out.glob(".work-*"))
    assert len(work_dirs) == 1
    assert "private work retained because rollback incomplete" in message

    # A foreign replacement of the published helper must be preserved and
    # reported, never unlinked as if it were ours.
    out = tmp / "fault-foreign-final"
    calls = []

    def publish_foreign_final(source, target, published=None, **kwargs):
        calls.append(target)
        if len(calls) == 1:
            result = real_publish(source, target, published, **kwargs)
            target.unlink()
            target.write_bytes(b"foreign final replacement")
            return result
        raise build_module.BuildError("injected second publication failure")

    with fake_toolchain(), mock.patch.object(
            build_module, "publish_no_clobber", publish_foreign_final):
        try:
            build(out)
        except build_module.BuildError as error:
            message = str(error)
        else:
            raise AssertionError("foreign final replacement was removed")
    assert "rollback incomplete" in message
    assert "changed published output preserved" in message
    assert (out / "carplay-native-auth").read_bytes() == \
        b"foreign final replacement"
    assert not (out / "build-metadata.json").exists()
    assert len(list(out.glob(".work-*"))) == 1

    # A foreign replacement after both publications must be detected by the
    # identity/readback checks and preserved, not adopted as our output.
    out = tmp / "fault-foreign-after-publish"
    calls = []

    def publish_foreign_after_second(source, target, published=None, **kwargs):
        calls.append(target)
        result = real_publish(source, target, published, **kwargs)
        if len(calls) == 2:
            helper = target.parent / "carplay-native-auth"
            helper.unlink()
            helper.write_bytes(b"foreign after publish")
        return result

    with fake_toolchain(), mock.patch.object(
            build_module, "publish_no_clobber", publish_foreign_after_second):
        try:
            build(out)
        except build_module.BuildError as error:
            message = str(error)
        else:
            raise AssertionError("post-publish foreign replacement was adopted")
    assert "published output changed after creation" in message
    assert "rollback incomplete" in message
    assert "changed published output preserved" in message
    assert (out / "carplay-native-auth").read_bytes() == \
        b"foreign after publish"
    assert not (out / "build-metadata.json").exists()
    assert len(list(out.glob(".work-*"))) == 1

    # A foreign replacement inside private work must be preserved and the
    # directory retained with a reported path.
    out = tmp / "fault-private-replacement"
    calls = []

    def publish_foreign_private(source, target, published=None, **kwargs):
        calls.append(target)
        if len(calls) == 1:
            return real_publish(source, target, published, **kwargs)
        source.unlink()
        source.write_bytes(b"foreign private replacement")
        raise build_module.BuildError("injected second publication failure")

    with fake_toolchain(), mock.patch.object(
            build_module, "publish_no_clobber", publish_foreign_private):
        try:
            build(out)
        except build_module.BuildError as error:
            message = str(error)
        else:
            raise AssertionError("foreign private replacement was removed")
    assert "private cleanup incomplete" in message
    assert "changed private entry preserved" in message
    assert not (out / "carplay-native-auth").exists()
    work_dirs = list(out.glob(".work-*"))
    assert len(work_dirs) == 1
    assert (work_dirs[0] / "build-metadata.json").read_bytes() == \
        b"foreign private replacement"

    # Cleanup permission denial on an owned private entry must be reported;
    # the owned published helper is still rolled back.
    out = tmp / "fault-private-eacces"
    calls = []

    def deny_private_unlink(path, *args, **kwargs):
        if str(path) == "cp_native_auth.o":
            raise PermissionError(
                errno.EACCES, "injected private denial", str(path))
        return real_unlink(path, *args, **kwargs)

    def publish_second_failure_again(source, target, published=None, **kwargs):
        calls.append(target)
        if len(calls) == 2:
            raise build_module.BuildError(
                "injected second publication failure")
        return real_publish(source, target, published, **kwargs)

    with fake_toolchain(), mock.patch.object(
            build_module, "publish_no_clobber",
            publish_second_failure_again), mock.patch.object(
                build_module.os, "unlink", deny_private_unlink):
        try:
            build(out)
        except build_module.BuildError as error:
            message = str(error)
        else:
            raise AssertionError("private cleanup denial was hidden")
    assert "private cleanup incomplete" in message
    assert "cannot remove owned private entry" in message
    assert "cp_native_auth.o" in message
    assert not (out / "carplay-native-auth").exists()
    work_dirs = list(out.glob(".work-*"))
    assert len(work_dirs) == 1
    assert (work_dirs[0] / "cp_native_auth.o").exists()

    # Creation/open failure must retain and report the work directory.
    out = tmp / "fault-work-open"

    def deny_directory_open(path, flags, *args, **kwargs):
        if flags & os.O_DIRECTORY and Path(path).name.startswith(".work-"):
            raise OSError(errno.EMFILE, "injected work open failure")
        return real_open(path, flags, *args, **kwargs)

    with fake_toolchain(), mock.patch.object(
            build_module.os, "open", deny_directory_open):
        try:
            build(out)
        except build_module.BuildError as error:
            message = str(error)
        else:
            raise AssertionError("private work open failure was hidden")
    assert "private work retained" in message
    assert len(list(out.glob(".work-*"))) == 1

    # fstat failure after opening the work directory must close the descriptor
    # and report the retained directory.
    out = tmp / "fault-work-fstat"
    opened = []
    closed = []

    def tracking_open(path, flags, *args, **kwargs):
        fd = real_open(path, flags, *args, **kwargs)
        if flags & os.O_DIRECTORY and Path(path).name.startswith(".work-"):
            opened.append(fd)
        return fd

    def failing_fstat(fd):
        if fd in opened:
            raise OSError(errno.EIO, "injected work fstat failure")
        return real_fstat(fd)

    def tracking_close(fd):
        closed.append(fd)
        return real_close(fd)

    with fake_toolchain(), mock.patch.object(
            build_module.os, "open", tracking_open), \
            mock.patch.object(build_module.os, "fstat", failing_fstat), \
            mock.patch.object(build_module.os, "close", tracking_close):
        try:
            build(out)
        except build_module.BuildError as error:
            message = str(error)
        else:
            raise AssertionError("private work fstat failure was hidden")
    assert "private work retained" in message
    assert opened
    assert opened[0] in closed

    # Replacing the private work directory itself must preserve the foreign
    # directory and report the owned directory's actual moved path.
    out = tmp / "fault-private-dir-replacement"
    calls = []

    def publish_replace_work_dir(source, target, published=None, **kwargs):
        calls.append(target)
        result = real_publish(source, target, published, **kwargs)
        if len(calls) == 2:
            work = source.parent
            moved = work.parent / (work.name + ".moved")
            work.rename(moved)
            work.mkdir(mode=0o700)
            (work / "foreign-dir-entry").write_bytes(b"foreign directory")
        return result

    with fake_toolchain(), mock.patch.object(
            build_module, "publish_no_clobber", publish_replace_work_dir):
        try:
            build(out)
        except build_module.BuildError as error:
            message = str(error)
        else:
            raise AssertionError("private work directory replacement was hidden")
    assert "output published:" in message
    assert "private cleanup incomplete" in message
    assert "changed private work directory preserved" in message
    work_paths = list(out.glob(".work-*"))
    foreign = [path for path in work_paths
               if not path.name.endswith(".moved")]
    moved = [path for path in work_paths if path.name.endswith(".moved")]
    assert len(foreign) == 1 and len(moved) == 1
    assert (foreign[0] / "foreign-dir-entry").read_bytes() == \
        b"foreign directory"
    assert str(moved[0]) in message
    assert (out / "carplay-native-auth").exists()
    assert (out / "build-metadata.json").exists()

    # Success path: an unknown private entry is preserved and reported in the
    # result instead of being removed by a blind recursive cleanup.
    out = tmp / "fault-private-unknown"
    calls = []

    def publish_add_foreign(source, target, published=None, **kwargs):
        calls.append(target)
        result = real_publish(source, target, published, **kwargs)
        if len(calls) == 2:
            (source.parent / "foreign-note").write_bytes(b"foreign private")
        return result

    with fake_toolchain(), mock.patch.object(
            build_module, "publish_no_clobber", publish_add_foreign):
        try:
            build(out)
        except build_module.BuildError as error:
            message = str(error)
        else:
            raise AssertionError(
                "published output was reported as a clean build")
    assert "output published:" in message
    assert "private cleanup incomplete" in message
    assert "unknown private entry preserved" in message
    work_dirs = list(out.glob(".work-*"))
    assert len(work_dirs) == 1
    assert (work_dirs[0] / "foreign-note").read_bytes() == b"foreign private"
    assert (out / "carplay-native-auth").exists()
    assert (out / "build-metadata.json").exists()


def static_checks() -> None:
    required = [
        SRC / "cp_oem_auth.h",
        SRC / "cp_oem_auth.c",
        SRC / "cp_native_auth.c",
        SRC / "cp_native_start.S",
        TEST_C,
        PY_TEST,
        BUILD_SCRIPT,
    ]
    spdx = "SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors"
    license_line = "SPDX-License-Identifier: AGPL-3.0-only"
    for path in required:
        assert path.is_file(), path
        head = path.read_text(encoding="utf-8")[:500]
        assert spdx in head and license_line in head, path
    for name in ("cp_oem_auth.c", "cp_native_auth.c"):
        text = (SRC / name).read_text(encoding="utf-8")
        for token in ("printf(", "fprintf(", "syslog(", "perror(",
                      "#include <stdio.h>", "system(", "popen("):
            assert token not in text, (name, token)
    client = (SRC / "cp_oem_auth.c").read_text(encoding="utf-8")
    assert "dlopen(" not in client and "dlsym(" not in client
    header = (SRC / "cp_oem_auth.h").read_text(encoding="utf-8")
    assert header.count("cp_oem_auth_open_mock") == 1
    assert header.index("#ifdef CP_OEM_AUTH_TEST") < \
        header.index("cp_oem_auth_open_mock")
    helper = (SRC / "cp_native_auth.c").read_text(encoding="utf-8")
    assert "#ifdef CP_OEM_AUTH_TEST" in helper
    assert "--expected-sha256" in helper and "--once" in helper
    assert "dlopen(path" not in helper
    assert "dlopen(proc_path" in helper
    assert "sanitize_loader_env" in helper and "LD_PRELOAD" in helper
    assert "init_failed" in helper and "init_error" in helper
    assert "not a global claim" in helper
    assert "does not bound OEM/chip" in helper
    header_text = header
    assert "BUILD-ONLY" in header_text
    assert "NOT CAR-VALIDATED" in header_text
    assert "backend_error" in header_text
    assert "force_destroy_lock_failure" in header_text
    script = BUILD_SCRIPT.read_text(encoding="utf-8")
    assert "TARGET:" in script
    assert "GLIBC" in script and "2.25" in script
    assert "shutil" not in script
    assert "ignore_errors" not in script
    for token in ("check_output_targets", "verify_compiler",
                  "verify_no_test_overrides", "activation",
                  "publish_no_clobber", "create_private_work",
                  "cleanup_published", "cleanup_private_work",
                  "verify_published_identity", "private_work_readlink",
                  "rollback incomplete", "private work retained",
                  "unknown private entry preserved"):
        assert token in script, token
    forbidden_home = "/" + "home/"
    assert forbidden_home not in "".join(
        path.read_text(encoding="utf-8") for path in required)


def main() -> int:
    static_checks()
    build_module = load_build_module()
    output_root = Path("/tmp/opencode")
    output_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="carplay-c3-",
                                     dir=str(output_root)) as tmp_name:
        tmp = Path(tmp_name)
        strict_compile_sources(tmp)
        start_helper = build_host_start_helper(tmp)
        start_result = run([str(start_helper)], text=True,
                           capture_output=True, timeout=10)
        assert start_result.returncode == 2, start_result
        client = build_test_binary(tmp)
        unit = checked([str(client)], text=True, capture_output=True,
                       timeout=120)
        assert "checks passed" in unit.stdout, unit.stdout
        assert "FAIL" not in unit.stdout + unit.stderr

        test_build_gates(build_module, tmp)
        test_build_rollback_and_cleanup(build_module, tmp)

        mock = build_mock_so(tmp)
        mock_sha = sha256_file(mock)
        helper = build_host_helper(tmp)
        try:
            build_module.verify_no_test_overrides(helper)
        except build_module.BuildError:
            pass
        else:
            raise AssertionError("test-mode host helper passed production gate")
        test_helper_cli(helper, mock, mock_sha, tmp)
        test_helper_rpc(helper, mock, mock_sha, client, tmp)
        test_helper_failures(helper, mock, mock_sha, client, tmp)
        test_helper_init_latch(helper, mock, mock_sha, tmp)
        test_helper_malformed(helper, mock, mock_sha, tmp)
        test_malicious_servers(client, tmp)
        test_client_total_deadline(client, tmp)
        test_client_error_order_and_cap(client, tmp)

        # Host helper is not a target helper: the build gate must refuse it.
        try:
            build_module.verify_helper(helper)
        except build_module.BuildError:
            pass
        else:
            raise AssertionError("host helper unexpectedly passed target gate")

    print("carplay c3 oem auth: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
