#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Offline tests for the C3 experimental vbtd-63 SPP RX hook and builder.

No OEM binary is executed.  The synthetic ELF tests prove the hash, ELF
offset-mapping, span, alias, and readback gates without committing OEM bytes.
The optional real-firmware test only reads an owner-supplied rootfs and writes
a patched copy under /tmp/opencode; it never executes the OEM binary.
"""
from __future__ import annotations

import hashlib
import errno
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from unittest.mock import patch

# The tests import the builder from the source tree; do not leave bytecode
# side effects in the repository.
sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[3]
PKG = ROOT / "packages" / "wifi1b"
SRC = PKG / "src" / "carplay"
SCRIPTS = PKG / "scripts"
TESTS = PKG / "tests"
BUILD_ROOT = Path("/tmp/opencode")

BUILDER_PATH = SCRIPTS / "build_carplay_spp_patch.py"
HOOK_C = SRC / "cp_spp_rx_hook.c"
HOOK_H = SRC / "cp_spp_rx.h"
HOOK_TEST_C = TESTS / "test_carplay_spp_hook.c"
LOCKS = ROOT / "config" / "firmware_locks.env"

FUNC_VADDR = 0x491C90
FUNC_LEN = 0x355
MAGIC = b"W1BSPP01"
DEFAULT_PATH = b"/run/wifi1b/carplay/rx.sock"
UUID_CAFF = b"00000000-DECA-FADE-DECA-DEAFDECACAFF"
UUID_CAFE = b"00000000-DECA-FADE-DECA-DEAFDECACAFE"
UUID_CARPLAY = b"EC884348-CD41-40A2-9727-575D50BF1FD3"
FUNC_STOCK_SHA = "ce4499ebe7786606b94c58809283f2306aacd77914c38f620807e396d19ca5c7"
VBTD63_SHA = "c56b68fdd35b16b95688ab54a48ffb9f18034d925c057e86f8b9fe304f8b26b9"


def load_builder():
    spec = importlib.util.spec_from_file_location("cp_spp_patch_builder",
                                                  BUILDER_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


BUILDER = load_builder()


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def test_source_policy() -> None:
    for path in (HOOK_C, HOOK_H, BUILDER_PATH, Path(__file__), HOOK_TEST_C):
        body = text(path)
        assert "SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors" in body, path
        assert "SPDX-License-Identifier: AGPL-3.0-only" in body, path

    header = text(HOOK_H)
    for needle in (
        "CP_SPP_RX_MAGIC",
        "CP_SPP_RX_HEADER_SIZE",
        "CP_SPP_RX_OFF_DATALEN",
        "CP_SPP_RX_OFF_PEER",
        "CP_SPP_RX_OFF_UUID_KIND",
        "CP_SPP_RX_MAX_PAYLOAD",
        "CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF",
        "CP_SPP_RX_UUID_KIND_IAP2_PHONE_CAFE",
        "CP_SPP_RX_UUID_KIND_CARPLAY",
    ):
        assert needle in header, needle

    # The published header must not leak the private OEM event ABI or hook
    # entry point; the future decoder includes only wire constants.
    for forbidden in (
        "0x491c90",
        "T_VBT_EVENT_BASE",
        "cp_spp_rx_hook(",
        "VBTSppReceivedDataCfm",
        "payload pointer",
    ):
        assert forbidden not in header, forbidden

    hook = text(HOOK_C)
    for needle in (
        "CP_SPP_RX_SYS_SOCKET 41",
        "CP_SPP_RX_SYS_SENDMSG 46",
        "CP_SPP_RX_SYS_CLOSE 3",
        "CP_SPP_RX_SOCK_DGRAM",
        "CP_SPP_RX_SOCK_NONBLOCK",
        "CP_SPP_RX_SOCK_CLOEXEC",
        "CP_SPP_RX_MSG_DONTWAIT",
        "CP_SPP_RX_MSG_NOSIGNAL",
        "CP_SPP_RX_EVENT_OFF_PEER_PTR 0x08u",
        "CP_SPP_RX_EVENT_OFF_UUID_PTR 0x30u",
        "CP_SPP_RX_EVENT_OFF_PAYLOAD 0x50u",
    ):
        assert needle in hook, needle

    for forbidden in (
        "mkfifo",
        "malloc(",
        "calloc(",
        "realloc(",
        "free(",
        "printf(",
        "syslog(",
        "pthread_",
        "dlopen(",
        "LD_PRELOAD",
    ):
        assert forbidden not in hook, forbidden
    assert "signal(SIGPIPE" not in hook
    assert "raise(SIGPIPE" not in hook

    builder = text(BUILDER_PATH)
    for needle in (
        "VBTD63_STOCK_SHA256",
        "VBTD63_SPP_RX_STOCK_SHA256",
        "VBTD63_SPP_RX_VADDR",
        "VBTD63_SPP_RX_OFFSET",
        "VBTD63_SPP_RX_LEN",
        "--orphan-handling=error",
        "EXPERIMENTAL_BUILD_ONLY_NOT_CAR_VALIDATED",
        "oem_binary_executed",
        "os.link",
        "_map_vaddr",
    ):
        assert needle in builder, needle
    assert "shell=True" not in builder
    assert "os.system(" not in builder
    assert "--force" not in builder
    assert 'add_argument("--locks"' not in builder
    # The input is read, never written in place.
    assert "input_path.write" not in builder
    assert "input_path.open(\"wb\")" not in builder


def test_config_pins() -> None:
    locks = BUILDER.load_locks(LOCKS)
    assert locks["VBTD63_SIZE"] == "14149896"
    assert locks["VBTD63_STOCK_SHA256"].lower() == VBTD63_SHA
    assert locks["VBTD63_SPP_RX_VADDR"].lower() == "0x491c90"
    assert locks["VBTD63_SPP_RX_OFFSET"].lower() == "0x491c90"
    assert locks["VBTD63_SPP_RX_LEN"].lower() == "0x355"
    assert locks["VBTD63_SPP_RX_STOCK_SHA256"].lower() == FUNC_STOCK_SHA
    # Existing pins must remain untouched.
    assert locks["APPLAUNCHER_STOCK_SHA256"] == (
        "c8d6d56bc254ad0731e81f40bd83aa9b3be6b49bcbd8efb291157a06c4a84268"
    )


def test_blob() -> None:
    result = BUILDER.build_blob()
    blob = result["blob"]
    assert isinstance(blob, bytes)
    assert 0 < len(blob) <= FUNC_LEN
    assert result["blob_size"] == len(blob)
    assert result["blob_sha256"] == sha(blob)
    assert blob.startswith(b"\x48\x85\xff")  # test rdi,rdi: event null check
    assert blob.endswith(b"\x00")
    for needle in (DEFAULT_PATH, UUID_CAFF, UUID_CAFE, UUID_CARPLAY):
        assert needle in blob
    assert result["text_sections"]
    assert result["rodata_sections"]


def test_cpp_string_abi(tmp: Path) -> None:
    compiler = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++")
    if compiler is None:
        print("SKIP: no host C++ compiler for std::string ABI probe")
        return
    source = tmp / "abi_probe.cc"
    binary = tmp / "abi_probe"
    source.write_text(
        """#include <cstddef>
#include <cstdio>
#include <string>
int main() {
    std::string sso(10, 'x');
    std::string heap(36, 'y');
    if (sizeof(std::string) != 32u) return 1;
    if (*reinterpret_cast<const void* const*>(&sso) != sso.data()) return 2;
    if (*reinterpret_cast<const std::size_t*>(
            reinterpret_cast<const char*>(&sso) + 8u) != sso.size()) return 3;
    if (*reinterpret_cast<const void* const*>(&heap) != heap.data()) return 4;
    if (*reinterpret_cast<const std::size_t*>(
            reinterpret_cast<const char*>(&heap) + 8u) != heap.size()) return 5;
    std::puts("PASS: host libstdc++ std::string layout 32/0/8");
    return 0;
}
""",
        encoding="utf-8",
    )
    subprocess.run([compiler, "-std=c++17", "-O2", "-Wall", "-Wextra",
                    "-Werror", "-o", str(binary), str(source)],
                   check=True, timeout=120)
    subprocess.run([str(binary)], check=True, timeout=30)


def test_c_hook(tmp: Path) -> None:
    compiler = os.environ.get("CC", "cc")
    binary = tmp / "test_carplay_spp_hook"
    sock = tmp / "rx.sock"
    cmd = [
        compiler,
        "-std=c11",
        "-O2",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(SRC),
        "-DCP_SPP_RX_SOCKET_PATH=" + '"' + str(sock) + '"',
        str(HOOK_C),
        str(HOOK_TEST_C),
        "-o",
        str(binary),
    ]
    subprocess.run(cmd, check=True, timeout=120)
    subprocess.run([str(binary)], check=True, timeout=120)
    print("PASS: cp_spp_rx_hook fake-event, backpressure, SIGPIPE and FD tests")


def test_c_hook_sanitizer(tmp: Path) -> None:
    compiler = os.environ.get("CC", "cc")
    probe = tmp / "sanprobe.c"
    probe.write_text("int main(void) { return 0; }\n", encoding="utf-8")
    supported = subprocess.run(
        [compiler, "-std=c11", "-fsanitize=address,undefined",
         "-o", str(tmp / "sanprobe"), str(probe)],
        capture_output=True, text=True, timeout=60,
    ).returncode == 0
    if not supported:
        print("SKIP: compiler sanitizer runtime unavailable")
        return
    binary = tmp / "test_carplay_spp_hook_san"
    sock = tmp / "rx-san.sock"
    cmd = [
        compiler,
        "-std=c11",
        "-O1",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-I",
        str(SRC),
        "-DCP_SPP_RX_SOCKET_PATH=" + '"' + str(sock) + '"',
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
        str(HOOK_C),
        str(HOOK_TEST_C),
        "-o",
        str(binary),
    ]
    subprocess.run(cmd, check=True, timeout=120)
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1:strict_string_checks=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    subprocess.run([str(binary)], check=True, timeout=120, env=env)
    print("PASS: cp_spp_rx_hook ASan/UBSan fake-event tests")


def make_synthetic_elf(path: Path, *, file_size: int = 0x2000,
                       segment_offset: int = 0,
                       segment_vaddr: int = 0,
                       segment_flags: int = 5,
                       function_offset: int = 0x1000,
                       function_len: int = FUNC_LEN,
                       seed: int = 1) -> None:
    data = bytearray(file_size)
    ident = bytearray(16)
    ident[:4] = b"\x7fELF"
    ident[4] = 2
    ident[5] = 1
    ident[6] = 1
    struct.pack_into(
        "<16sHHIQQQIHHHHHH", data, 0, bytes(ident), 2, 62, 1, 0, 64, 0, 0,
        64, 56, 1, 0, 0, 0,
    )
    struct.pack_into(
        "<IIQQQQQQ", data, 64, 1, segment_flags, segment_offset, segment_vaddr,
        segment_vaddr, file_size - segment_offset, file_size - segment_offset,
        0x1000,
    )
    for index in range(function_len):
        if function_offset + index >= len(data):
            break
        data[function_offset + index] = (index * 17 + seed) & 0xFF
    path.write_bytes(data)


def write_locks(path: Path, input_path: Path, *, vaddr: int,
                offset: int, length: int = FUNC_LEN,
                whole_sha: str | None = None,
                function_sha: str | None = None) -> None:
    data = input_path.read_bytes()
    if whole_sha is None:
        whole_sha = sha(data)
    preimage = data[offset:offset + length]
    if function_sha is None:
        function_sha = sha(preimage)
    path.write_text(
        "SCOURGE_FIRMWARE_ID=\"TEST\"\n"
        f"VBTD63_SIZE={len(data)}\n"
        f"VBTD63_STOCK_SHA256=\"{whole_sha}\"\n"
        f"VBTD63_SPP_RX_VADDR={vaddr:#x}\n"
        f"VBTD63_SPP_RX_OFFSET={offset:#x}\n"
        f"VBTD63_SPP_RX_LEN={length:#x}\n"
        f"VBTD63_SPP_RX_STOCK_SHA256=\"{function_sha}\"\n",
        encoding="ascii",
    )


def expect_refusal(function, *args, **kwargs) -> str:
    try:
        function(*args, **kwargs)
    except BUILDER.BuildError as exc:
        return str(exc)
    raise AssertionError("builder accepted an input that should have been refused")


def test_synthetic_builder() -> None:
    with tempfile.TemporaryDirectory(prefix="cp-spp-synthetic-",
                                     dir=str(BUILD_ROOT)) as raw:
        tmp = Path(raw)
        input_path = tmp / "vbtd-63.synthetic"
        locks = tmp / "locks.env"
        out = tmp / "out"
        make_synthetic_elf(input_path)
        write_locks(locks, input_path, vaddr=0x1000, offset=0x1000)

        result = BUILDER.build_patch(input_path, out, locks,
                                     allow_test_geometry=True)
        patched = out / "vbtd-63.spp-rx.patched"
        manifest = out / "vbtd-63.spp-rx.manifest.json"
        assert patched.is_file() and manifest.is_file()
        blob = BUILDER.build_blob()["blob"]
        original = input_path.read_bytes()
        written = patched.read_bytes()
        assert len(written) == len(original)
        assert written[:0x1000] == original[:0x1000]
        assert written[0x1000:0x1000 + len(blob)] == blob
        assert written[0x1000 + len(blob):0x1000 + FUNC_LEN] == (
            b"\x90" * (FUNC_LEN - len(blob))
        )
        assert written[0x1000 + FUNC_LEN:] == original[0x1000 + FUNC_LEN:]
        report = result["manifest"]
        assert report["input_sha256"] == sha(original)
        assert report["output_sha256"] == sha(written)
        assert report["blob_sha256"] == sha(blob)
        assert report["output_sha256"] != report["input_sha256"]
        assert report["status"] == "EXPERIMENTAL_BUILD_ONLY_NOT_CAR_VALIDATED"
        assert report["car_validated"] is False
        assert report["installer_activated"] is False
        assert report["oem_binary_executed"] is False
        assert report["test_fixture"] is True and report["target"] == "synthetic-test-elf"
        for tool in ("compiler", "linker"):
            assert len(report["toolchain"][tool]["sha256"]) == 64
            assert report["toolchain"][tool]["version"]
        assert "-fPIC" in report["compile_flags"]
        assert "--build-id=none" in report["link_flags"]
        assert report["builder_sha256"] == sha(BUILDER_PATH.read_bytes())
        assert len(report["linker_script_sha256"]) == 64

        # The production CLI/API cannot replace canonical pins with synthetic
        # locks, even when the synthetic ELF has the production function VADDR.
        bypass = tmp / "bypass.elf"
        bypass_locks = tmp / "bypass.env"
        make_synthetic_elf(bypass, segment_vaddr=FUNC_VADDR)
        write_locks(bypass_locks, bypass, vaddr=FUNC_VADDR, offset=0x1000)
        error = expect_refusal(BUILDER.build_patch, bypass, tmp / "bypass-api", bypass_locks)
        assert "canonical" in error
        cli = subprocess.run([sys.executable, str(BUILDER_PATH), "--input", str(bypass),
                              "--out", str(tmp / "bypass-cli"), "--locks", str(bypass_locks)],
                             capture_output=True, text=True, timeout=30)
        assert cli.returncode == 2 and "unrecognized arguments" in cli.stderr
        assert not (tmp / "bypass-cli").exists()
        cli = subprocess.run([sys.executable, str(BUILDER_PATH), "--input", str(bypass),
                              "--out", str(tmp / "canonical-refusal")],
                             capture_output=True, text=True, timeout=30)
        assert cli.returncode == 2 and "REFUSED:" in cli.stderr
        assert not (tmp / "canonical-refusal").exists()

        # Existing regular outputs are not silently replaced.
        expect_refusal(BUILDER.build_patch, input_path, out, locks,
                       allow_test_geometry=True)
        assert patched.read_bytes() == written
        for filename in ("vbtd-63.spp-rx.patched", "vbtd-63.spp-rx.manifest.json"):
            existing = tmp / ("existing-" + filename)
            existing.mkdir()
            sentinel = existing / filename
            sentinel.write_bytes(b"preserve-existing-output\n")
            expect_refusal(BUILDER.build_patch, input_path, existing, locks,
                           allow_test_geometry=True)
            assert sentinel.read_bytes() == b"preserve-existing-output\n"

        dangling = tmp / "dangling-output-parent"
        dangling.symlink_to(tmp / "missing-parent", target_is_directory=True)
        expect_refusal(BUILDER.build_patch, input_path, dangling / "out", locks,
                       allow_test_geometry=True)
        assert not (tmp / "missing-parent").exists()

        # Reject parent components before lexical normalization can erase a
        # symlink from a path such as link/../input. Neither direction writes.
        parent_target = tmp / "target" / "dir"
        parent_target.mkdir(parents=True)
        (parent_target.parent / input_path.name).write_bytes(original)
        parent_link = tmp / "parent-link"
        parent_link.symlink_to(parent_target, target_is_directory=True)
        assert "parent-directory" in expect_refusal(
            BUILDER.build_patch, parent_link / ".." / input_path.name,
            tmp / "parent-input-out", locks, allow_test_geometry=True)
        assert "parent-directory" in expect_refusal(
            BUILDER.build_patch, input_path, parent_link / ".." / "out",
            locks, allow_test_geometry=True)
        assert not (tmp / "target" / "out").exists()

        # A failed publish must retract only files this invocation created.
        original_link = os.link
        original_unlink = Path.unlink
        for mode in ("second-link", "post-link-unlink", "foreign-replacement",
                     "persistent-temp-denial", "foreign-temp-replacement"):
            fault_out = tmp / ("fault-" + mode)
            fault_output = fault_out / "vbtd-63.spp-rx.patched"
            fault_manifest = fault_out / "vbtd-63.spp-rx.manifest.json"
            calls = [0]
            fired = [False]
            temp_output = [None]
            before_fds = len(os.listdir("/proc/self/fd"))

            def failing_link(source, destination):
                calls[0] += 1
                if calls[0] == 1:
                    temp_output[0] = Path(source)
                if calls[0] == 2 and mode != "post-link-unlink":
                    fired[0] = True
                    if mode == "foreign-replacement":
                        fault_output.unlink()
                        fault_output.write_bytes(b"preserve-foreign-replacement\n")
                    if mode == "foreign-temp-replacement":
                        original_unlink(temp_output[0])
                        temp_output[0].write_bytes(b"preserve-foreign-temp\n")
                    raise OSError(errno.ENOSPC, "injected second-link failure")
                return original_link(source, destination)

            def failing_unlink(path, *args, **kwargs):
                if (mode == "persistent-temp-denial" and path.parent == fault_out and
                        path.name.startswith(".vbtd-63.spp-rx.")):
                    raise OSError(errno.EACCES, "injected persistent temp unlink denial")
                if (mode == "post-link-unlink" and not fired[0] and
                        path.name.startswith(".vbtd-63.spp-rx.patched.") and
                        fault_output.exists()):
                    fired[0] = True
                    raise OSError(errno.EIO, "injected post-link unlink failure")
                return original_unlink(path, *args, **kwargs)

            with patch.object(BUILDER.os, "link", failing_link), \
                    patch.object(Path, "unlink", failing_unlink):
                message = expect_refusal(BUILDER.build_patch, input_path, fault_out,
                                         locks, allow_test_geometry=True)
            assert fired[0] and "cannot publish output" in message
            assert len(os.listdir("/proc/self/fd")) == before_fds
            assert not fault_manifest.exists()
            if mode == "foreign-replacement":
                assert fault_output.read_bytes() == b"preserve-foreign-replacement\n"
                assert "changed output preserved" in message
            elif mode == "foreign-temp-replacement":
                assert not fault_output.exists()
                assert temp_output[0].read_bytes() == b"preserve-foreign-temp\n"
                assert "changed private temp preserved" in message
                assert str(temp_output[0]) in message
                assert list(fault_out.iterdir()) == [temp_output[0]]
            elif mode == "persistent-temp-denial":
                assert not fault_output.exists()
                leftovers = list(fault_out.iterdir())
                assert len(leftovers) == 2
                assert "rollback incomplete" in message
                assert "cannot remove owned private temp" in message
                assert all(str(path) in message for path in leftovers)
            else:
                assert not fault_output.exists()
                assert list(fault_out.iterdir()) == []

        # Creation-time failures must be tracked even before a file wrapper
        # could be constructed. The builder now owns raw fds, never fdopen.
        original_fstat = os.fstat
        original_stat = os.stat
        original_fdopen = os.fdopen
        for mode in ("fstat-output", "fstat-manifest", "fstat-no-identity",
                     "fdopen-output", "fdopen-manifest"):
            fault_out = tmp / ("creation-" + mode)
            fired = [False]
            before_fds = len(os.listdir("/proc/self/fd"))

            def is_temp_fd(fd, manifest=False):
                try:
                    target = Path(os.readlink(f"/proc/self/fd/{fd}"))
                except OSError:
                    return False
                prefix = ".vbtd-63.spp-rx.manifest." if manifest else ".vbtd-63.spp-rx.patched."
                return target.parent == fault_out and target.name.startswith(prefix)

            def failing_fstat(fd):
                if mode.startswith("fstat") and is_temp_fd(fd, mode == "fstat-manifest"):
                    fired[0] = True
                    raise OSError(errno.EIO, "injected creation fstat failure")
                return original_fstat(fd)

            def failing_stat(path, *args, **kwargs):
                if (mode == "fstat-no-identity" and
                        str(path).startswith("/proc/self/fd/")):
                    raise OSError(errno.EACCES, "injected unavailable descriptor identity")
                return original_stat(path, *args, **kwargs)

            def failing_fdopen(fd, *args, **kwargs):
                if is_temp_fd(fd, mode == "fdopen-manifest"):
                    fired[0] = True
                    raise OSError(errno.EIO, "unexpected artifact fdopen call")
                return original_fdopen(fd, *args, **kwargs)

            with patch.object(BUILDER.os, "fstat", failing_fstat), \
                    patch.object(BUILDER.os, "stat", failing_stat), \
                    patch.object(BUILDER.os, "fdopen", failing_fdopen):
                if mode.startswith("fdopen"):
                    BUILDER.build_patch(input_path, fault_out, locks,
                                        allow_test_geometry=True)
                    assert not fired[0], "artifact writer must not transfer fd ownership"
                else:
                    message = expect_refusal(BUILDER.build_patch, input_path,
                                             fault_out, locks, allow_test_geometry=True)
                    assert fired[0] and "creation fstat failure" in message
            assert len(os.listdir("/proc/self/fd")) == before_fds
            if mode == "fstat-no-identity":
                leftovers = list(fault_out.iterdir())
                assert len(leftovers) == 1
                assert "unverified private temp preserved" in message
                assert str(leftovers[0]) in message
            elif mode.startswith("fstat"):
                assert list(fault_out.iterdir()) == []

        # A patched output is not accepted as an input.
        already_out = tmp / "already-out"
        expect_refusal(BUILDER.build_patch, patched, already_out, locks,
                       allow_test_geometry=True)

        # Whole-input hash refusal.
        corrupt = bytearray(original)
        corrupt[0x20] ^= 0x01
        bad_input = tmp / "corrupt.elf"
        bad_input.write_bytes(corrupt)
        write_locks(tmp / "whole-bad.env", bad_input, vaddr=0x1000,
                    offset=0x1000, whole_sha=sha(original))
        expect_refusal(BUILDER.build_patch, bad_input, tmp / "o1",
                       tmp / "whole-bad.env", allow_test_geometry=True)

        # Function-preimage hash refusal: whole file pin is made consistent,
        # function pin is wrong.
        corrupt2 = bytearray(original)
        corrupt2[0x1000] ^= 0x01
        bad2 = tmp / "corrupt-function.elf"
        bad2.write_bytes(corrupt2)
        write_locks(tmp / "function-bad.env", bad2, vaddr=0x1000,
                    offset=0x1000, whole_sha=sha(corrupt2),
                    function_sha=sha(original[0x1000:0x1000 + FUNC_LEN]))
        expect_refusal(BUILDER.build_patch, bad2, tmp / "o2",
                       tmp / "function-bad.env", allow_test_geometry=True)

        # ELF mapping: segment at file 0x1000, vaddr 0x2000.
        mapped = tmp / "mapped.elf"
        make_synthetic_elf(mapped, segment_offset=0x1000,
                           segment_vaddr=0x2000)
        write_locks(tmp / "mapped.env", mapped, vaddr=0x2000, offset=0x1000)
        BUILDER.build_patch(mapped, tmp / "o3", tmp / "mapped.env",
                            allow_test_geometry=True)
        write_locks(tmp / "mapped-bad.env", mapped, vaddr=0x2000, offset=0x1001)
        expect_refusal(BUILDER.build_patch, mapped, tmp / "o4",
                       tmp / "mapped-bad.env", allow_test_geometry=True)

        # Non-executable / out-of-bounds text spans.
        nonexec = tmp / "nonexec.elf"
        make_synthetic_elf(nonexec, segment_flags=4)
        write_locks(tmp / "nonexec.env", nonexec, vaddr=0x1000, offset=0x1000)
        expect_refusal(BUILDER.build_patch, nonexec, tmp / "o5",
                       tmp / "nonexec.env", allow_test_geometry=True)

        out_of_bounds = tmp / "oob.elf"
        make_synthetic_elf(out_of_bounds, file_size=0x1200)
        write_locks(tmp / "oob.env", out_of_bounds, vaddr=0x1000,
                    offset=0x1000)
        expect_refusal(BUILDER.build_patch, out_of_bounds, tmp / "o6",
                       tmp / "oob.env", allow_test_geometry=True)

        # Aliases and symlinks.
        expect_refusal(BUILDER.build_patch, input_path, input_path, locks,
                       allow_test_geometry=True)
        input_link = tmp / "input.link"
        input_link.symlink_to(input_path)
        expect_refusal(BUILDER.build_patch, input_link, tmp / "o7", locks,
                       allow_test_geometry=True)

        real_out = tmp / "real-out"
        real_out.mkdir()
        out_link = tmp / "out.link"
        out_link.symlink_to(real_out)
        expect_refusal(BUILDER.build_patch, input_path, out_link, locks,
                       allow_test_geometry=True)

        symlink_out = tmp / "symlink-out"
        symlink_out.mkdir()
        target = tmp / "target.bin"
        target.write_bytes(b"x")
        (symlink_out / "vbtd-63.spp-rx.patched").symlink_to(target)
        expect_refusal(BUILDER.build_patch, input_path, symlink_out, locks,
                       allow_test_geometry=True)

        # Output outside /tmp/opencode and not Git-ignored.
        with tempfile.TemporaryDirectory(prefix="cp-spp-unignored-",
                                         dir="/tmp") as bad_raw:
            expect_refusal(BUILDER.build_patch, input_path, Path(bad_raw), locks,
                           allow_test_geometry=True)


def test_blob_rejections() -> None:
    with tempfile.TemporaryDirectory(prefix="cp-spp-source-",
                                     dir=str(BUILD_ROOT)) as raw:
        tmp = Path(raw)
        oversize = tmp / "oversize.c"
        oversize.write_text(
            "#include \"cp_spp_rx.h\"\n"
            "static const unsigned char huge[0x400] "
            "__attribute__((used)) = {1};\n"
            "void cp_spp_rx_hook(void *event) { (void)event; (void)huge[0]; }\n",
            encoding="utf-8",
        )
        error = expect_refusal(BUILDER.build_blob, oversize)
        assert "exceeds function extent" in error, error

        writable = tmp / "writable.c"
        writable.write_text(
            "#include \"cp_spp_rx.h\"\n"
            "int global_data = 1;\n"
            "void cp_spp_rx_hook(void *event) { (void)event; global_data++; }\n",
            encoding="utf-8",
        )
        expect_refusal(BUILDER.build_blob, writable)

        undefined = tmp / "undefined.c"
        undefined.write_text(
            "#include \"cp_spp_rx.h\"\n"
            "extern void external_symbol(void);\n"
            "void cp_spp_rx_hook(void *event) { (void)event; external_symbol(); }\n",
            encoding="utf-8",
        )
        expect_refusal(BUILDER.build_blob, undefined)


def test_optional_real_firmware() -> None:
    rootfs = os.environ.get("SCOURGE_ROOTFS")
    if not rootfs:
        print("SKIP: SCOURGE_ROOTFS not set; real firmware readback not run")
        return
    input_path = Path(rootfs) / "app" / "bin" / "vbtd-63"
    if not input_path.is_file():
        print(f"SKIP: {input_path} not present")
        return
    with tempfile.TemporaryDirectory(prefix="cp-spp-real-",
                                     dir=str(BUILD_ROOT)) as raw:
        out = Path(raw) / "out"
        result = BUILDER.build_patch(input_path, out, LOCKS)
        patched = out / "vbtd-63.spp-rx.patched"
        original = input_path.read_bytes()
        written = patched.read_bytes()
        assert len(original) == len(written)
        assert written[:FUNC_VADDR] == original[:FUNC_VADDR]
        assert written[FUNC_VADDR + FUNC_LEN:] == original[FUNC_VADDR + FUNC_LEN:]
        assert result["manifest"]["input_sha256"] == VBTD63_SHA
        assert result["manifest"]["test_fixture"] is False
        assert result["manifest"]["target"] == "vbtd-63"
        assert result["manifest"]["function_preimage_sha256"] == FUNC_STOCK_SHA
        blob = BUILDER.build_blob()["blob"]
        assert written[FUNC_VADDR:FUNC_VADDR + len(blob)] == blob
        assert written[FUNC_VADDR + len(blob):FUNC_VADDR + FUNC_LEN] == (
            b"\x90" * (FUNC_LEN - len(blob))
        )
        print("PASS: real pinned vbtd-63 read-only patch readback "
              "(OEM binary not executed)")


def main() -> int:
    BUILD_ROOT.mkdir(parents=True, exist_ok=True)
    test_source_policy()
    test_config_pins()
    test_blob()
    with tempfile.TemporaryDirectory(prefix="cp-spp-abi-",
                                     dir=str(BUILD_ROOT)) as raw:
        test_cpp_string_abi(Path(raw))
    with tempfile.TemporaryDirectory(prefix="cp-spp-hook-",
                                     dir=str(BUILD_ROOT)) as raw:
        test_c_hook(Path(raw))
    with tempfile.TemporaryDirectory(prefix="cp-spp-hook-san-",
                                     dir=str(BUILD_ROOT)) as raw:
        test_c_hook_sanitizer(Path(raw))
    test_synthetic_builder()
    test_blob_rejections()
    test_optional_real_firmware()
    print("PASS: carplay SPP RX hook and builder offline tests")
    return 0


if __name__ == "__main__":
    sys.exit(main())
