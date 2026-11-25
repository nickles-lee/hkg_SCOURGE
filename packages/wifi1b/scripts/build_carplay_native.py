#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W / IONIQ 5, x86-64, target rootfs glibc 2.25
"""Build the root-only CarPlay MFi auth helper against the target glibc.

The script reads (never writes) the owner's rootfs and the pinned
libCoreUtils.so, refuses hash/symbol/interpreter mismatches, compiles the tiny
helper with a custom _start and the rootfs libc-2.25/libdl-2.25, verifies that
every imported GLIBC symbol is <= 2.25, and writes only into an ignored output
directory.  It never executes OEM code.  Publication is no-clobber and
rollback is identity-anchored; private work is removed only after owned
cleanup, and foreign or unremovable entries are retained and reported rather
than recursively deleted.

Usage:
  python3 packages/wifi1b/scripts/build_carplay_native.py \
      --rootfs /path/to/rootfs \
      --out packages/wifi1b/out/carplay-native

  ... --check-only    verify only; print metadata JSON
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

SCRIPT = Path(__file__).resolve()
WIFI1B = SCRIPT.parents[1]
REPO = SCRIPT.parents[3]
HEADER = WIFI1B / "src" / "carplay" / "cp_oem_auth.h"
NATIVE_C = WIFI1B / "src" / "carplay" / "cp_native_auth.c"
NATIVE_START = WIFI1B / "src" / "carplay" / "cp_native_start.S"
DEFAULT_OUT = WIFI1B / "out" / "carplay-native"
TARGET_INTERPRETER = "/lib/ld-linux-x86-64.so.2"
MAX_GLIBC = (2, 25)


class BuildError(RuntimeError):
    pass


def run(command: list[str], cwd: Path | None = None) -> str:
    try:
        result = subprocess.run(
            command,
            cwd=str(cwd) if cwd is not None else None,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=120,
            check=False,
        )
    except OSError as error:
        raise BuildError(f"cannot run {' '.join(command)}: {error}") from error
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        raise BuildError(f"command failed ({result.returncode}): "
                         f"{' '.join(command)}\n{detail}")
    return result.stdout


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def header_define(name: str) -> str:
    text = HEADER.read_text(encoding="utf-8").replace("\\\n", " ")
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+(\S+)",
                      text, re.MULTILINE)
    if match is None:
        raise BuildError(f"missing {name} in {HEADER}")
    return match.group(1).strip().strip('"')


def parse_int_literal(text: str) -> int:
    cleaned = re.sub(r"(?i)(ull|llu|ul|lu|ll|u|l)$", "", text)
    return int(cleaned, 0)


def parse_expected_symbols() -> dict[str, int]:
    return {
        header_define("CP_OEM_AUTH_MFI_INITIALIZE_SYMBOL"):
            parse_int_literal(header_define(
                "CP_OEM_AUTH_MFI_INITIALIZE_ADDR")),
        header_define("CP_OEM_AUTH_MFI_CERTIFICATE_SYMBOL"):
            parse_int_literal(header_define(
                "CP_OEM_AUTH_MFI_CERTIFICATE_ADDR")),
        header_define("CP_OEM_AUTH_MFI_SIGNATURE_SYMBOL"):
            parse_int_literal(header_define(
                "CP_OEM_AUTH_MFI_SIGNATURE_ADDR")),
    }


def elf_symbols(path: Path, dynamic: bool = True) -> dict[str, dict[str, object]]:
    command = ["readelf", "--wide"]
    command.append("--dyn-syms" if dynamic else "--syms")
    command.append(str(path))
    output = run(command)
    symbols: dict[str, dict[str, object]] = {}
    pattern = re.compile(
        r"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+"
        r"(\S+)\s+(\S+)\s*$"
    )
    for line in output.splitlines():
        match = pattern.match(line)
        if match is None:
            continue
        value, size, kind, bind, visibility, ndx, name = match.groups()
        name = name.split("@", 1)[0]
        if name == "":
            continue
        symbols[name] = {
            "value": int(value, 16),
            "size": int(size, 10),
            "type": kind,
            "bind": bind,
            "visibility": visibility,
            "ndx": ndx,
        }
    return symbols


def verify_coreutils(path: Path, expected_sha: str,
                     expected_symbols: dict[str, int]) -> dict[str, object]:
    if not path.is_file() or path.is_symlink():
        raise BuildError(f"coreutils not found as a regular file: {path}")
    actual_sha = sha256_file(path)
    if actual_sha != expected_sha:
        raise BuildError(
            f"coreutils SHA-256 mismatch: expected {expected_sha}, "
            f"got {actual_sha}")
    header = run(["readelf", "-h", str(path)])
    if re.search(r"Class:\s+ELF64", header) is None:
        raise BuildError("coreutils is not ELF64")
    if re.search(r"Machine:\s+Advanced Micro Devices X86-64", header) is None:
        raise BuildError("coreutils is not x86-64")
    symbols = elf_symbols(path, dynamic=True)
    found: dict[str, int] = {}
    for name, address in expected_symbols.items():
        info = symbols.get(name)
        if info is None:
            raise BuildError(f"coreutils is missing exported symbol {name}")
        if info["type"] != "FUNC" or info["bind"] != "GLOBAL" or \
                info["visibility"] != "DEFAULT" or info["ndx"] == "UND":
            raise BuildError(f"coreutils symbol {name} is not a defined "
                             "global default function")
        if info["value"] != address:
            raise BuildError(
                f"coreutils symbol {name} address 0x{info['value']:x} "
                f"does not match pinned 0x{address:x}")
        found[name] = int(info["value"])
    return {
        "path": str(path),
        "sha256": actual_sha,
        "size": path.stat().st_size,
        "symbols": found,
    }


def elf_soname(path: Path) -> str | None:
    output = run(["readelf", "-d", str(path)])
    match = re.search(r"\(SONAME\)\s+Library soname: \[([^\]]+)\]", output)
    return match.group(1) if match else None


def verify_target_libs(rootfs: Path) -> dict[str, object]:
    expected = {
        "libc": (rootfs / "lib" / "libc-2.25.so", "libc.so.6"),
        "libdl": (rootfs / "lib" / "libdl-2.25.so", "libdl.so.2"),
        "libpthread": (rootfs / "lib" / "libpthread-2.25.so",
                       "libpthread.so.0"),
        "ld": (rootfs / "lib" / "ld-2.25.so", "ld-linux-x86-64.so.2"),
    }
    result: dict[str, object] = {}
    for name, (path, soname) in expected.items():
        if not path.is_file():
            raise BuildError(f"target {name} not found: {path}")
        actual_soname = elf_soname(path)
        if actual_soname != soname:
            raise BuildError(
                f"target {name} SONAME {actual_soname!r}, expected {soname!r}")
        result[name] = {
            "path": str(path),
            "sha256": sha256_file(path),
            "size": path.stat().st_size,
            "soname": actual_soname,
        }
    return result


def parse_glibc_version(value: str) -> tuple[int, int, int]:
    match = re.fullmatch(r"GLIBC_(\d+)\.(\d+)(?:\.(\d+))?", value)
    if match is None:
        raise BuildError(f"non-GLIBC symbol version {value}")
    return (int(match.group(1)), int(match.group(2)),
            int(match.group(3) or 0))


def verify_helper(path: Path) -> dict[str, object]:
    if not path.is_file():
        raise BuildError(f"helper not built: {path}")
    header = run(["readelf", "-h", str(path)])
    if re.search(r"Class:\s+ELF64", header) is None:
        raise BuildError("helper is not ELF64")
    if re.search(r"Machine:\s+Advanced Micro Devices X86-64", header) is None:
        raise BuildError("helper is not x86-64")
    program = run(["readelf", "-l", str(path)])
    interpreters = re.findall(
        r"\[Requesting program interpreter: ([^\]]+)\]", program)
    if interpreters != [TARGET_INTERPRETER]:
        raise BuildError(
            f"helper interpreter {interpreters!r}, expected "
            f"{TARGET_INTERPRETER!r}")
    dynamic = run(["readelf", "-d", str(path)])
    if "RPATH" in dynamic or "RUNPATH" in dynamic:
        raise BuildError("helper must not contain RPATH/RUNPATH")
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", dynamic)
    allowed_needed = {"libc.so.6", "libdl.so.2", "libpthread.so.0"}
    unexpected = [name for name in needed if name not in allowed_needed]
    if unexpected:
        raise BuildError(f"helper has unexpected NEEDED entries: {unexpected}")
    if "libc.so.6" not in needed:
        raise BuildError("helper does not depend on libc.so.6")
    symbols = run(["objdump", "-T", str(path)])
    versions = sorted(set(re.findall(r"GLIBC_[0-9.]+", symbols)))
    if "GLIBC_PRIVATE" in versions:
        raise BuildError("helper imports GLIBC_PRIVATE")
    max_version = (0, 0, 0)
    for version in versions:
        parsed = parse_glibc_version(version)
        if parsed > max_version:
            max_version = parsed
    if max_version > MAX_GLIBC:
        raise BuildError(
            f"helper imports GLIBC {max_version[0]}.{max_version[1]} "
            f"(limit {MAX_GLIBC[0]}.{MAX_GLIBC[1]})")
    for forbidden in ("OPENSSL_", "GLIBCXX_", "CXXABI_"):
        if forbidden in symbols:
            raise BuildError(f"helper imports forbidden ABI {forbidden}")
    verify_no_test_overrides(path)
    return {
        "path": str(path),
        "test_overrides_present": False,
        "sha256": sha256_file(path),
        "size": path.stat().st_size,
        "interpreter": TARGET_INTERPRETER,
        "needed": needed,
        "glibc_versions": versions,
        "max_glibc": ".".join(str(part) for part in max_version),
    }


OUTPUT_NAMES = ("carplay-native-auth", "build-metadata.json")


def repo_relative(path: Path) -> Path | None:
    try:
        return path.relative_to(REPO)
    except ValueError:
        return None


def ensure_output_dir(out: Path) -> None:
    if out.is_symlink():
        raise BuildError(f"output directory must not be a symlink: {out}")
    resolved = out.resolve()
    if repo_relative(resolved) is not None:
        result = subprocess.run(
            ["git", "-C", str(REPO), "check-ignore", "-q", str(resolved)],
            check=False,
        )
        if result.returncode != 0:
            raise BuildError(
                f"output directory is not git-ignored: {resolved}")
    else:
        try:
            resolved.relative_to(Path("/tmp/opencode").resolve())
        except ValueError:
            raise BuildError(
                "output directory must be git-ignored or under "
                f"/tmp/opencode: {resolved}") from None
    if out.exists() and not out.is_dir():
        raise BuildError(f"output path is not a directory: {out}")
    if not out.exists():
        try:
            out.mkdir(mode=0o700, parents=True)
        except OSError as error:
            raise BuildError(f"cannot create output directory {out}: {error}") from error
    st = os.lstat(out)
    if stat.S_ISLNK(st.st_mode) or not stat.S_ISDIR(st.st_mode):
        raise BuildError(f"output path is not a real directory: {out}")
    if st.st_uid != os.geteuid():
        raise BuildError(f"output directory is not owned by this user: {out}")
    if (st.st_mode & 0o022) != 0:
        raise BuildError(f"output directory is group/world writable: {out}")


def check_one_output_target(path: Path) -> None:
    try:
        st = os.lstat(path)
    except FileNotFoundError:
        return
    if stat.S_ISLNK(st.st_mode) or not stat.S_ISREG(st.st_mode) or \
            st.st_nlink != 1:
        raise BuildError(f"refusing pre-existing symlink/alias output: {path}")
    raise BuildError(f"refusing to overwrite existing output: {path}")


def check_output_targets(out: Path) -> None:
    for name in OUTPUT_NAMES:
        check_one_output_target(out / name)


def verify_compiler(cc: str) -> str:
    if not cc or any(ch in cc for ch in " \t\r\n\"'`$;&|<>()"):
        raise BuildError("compiler must be a single executable path")
    macros = run([cc, "-dM", "-E", "-x", "c", os.devnull])
    if re.search(r"^#define\s+CP_OEM_AUTH_TEST\b", macros, re.MULTILINE):
        raise BuildError("compiler wrapper injects CP_OEM_AUTH_TEST")
    return cc


def verify_no_test_overrides(path: Path) -> None:
    text = run(["strings", str(path)])
    for token in ("--expected-sha256", "--once", "CP_OEM_AUTH_TEST",
                  "CP_MOCK", "cp_oem_auth_open_mock"):
        if token in text:
            raise BuildError(
                f"production helper contains test override token {token!r}")
    dynamic = run(["readelf", "--wide", "--dyn-syms", str(path)])
    for token in ("cp_oem_auth_open_mock", "cp_oem_auth_provider_certificate",
                  "cp_oem_auth_provider_signature"):
        if token in dynamic:
            raise BuildError(
                f"production helper contains test/blocking symbol {token!r}")


PRIVATE_SOURCE_NAMES = (
    "cp_native_auth.o",
    "cp_native_start.o",
    "carplay-native-auth",
)


def close_anchors(anchors: list[int]) -> list[str]:
    errors = []
    while anchors:
        fd = anchors.pop()
        try:
            os.close(fd)
        except OSError as error:
            errors.append(f"cannot close private anchor fd {fd}: {error}")
    return errors


def create_private_work(out: Path) -> tuple[Path, int]:
    try:
        work = Path(tempfile.mkdtemp(prefix=".work-", dir=str(out)))
    except OSError as error:
        raise BuildError(
            f"cannot create private work directory: {error}") from error
    work_fd = -1
    try:
        created = os.lstat(work)
        if (not stat.S_ISDIR(created.st_mode) or
                created.st_uid != os.geteuid()):
            raise BuildError(
                f"private work directory is not owned: {work}")
        work_fd = os.open(
            work, os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
        opened = os.fstat(work_fd)
        if ((opened.st_dev, opened.st_ino) !=
                (created.st_dev, created.st_ino)):
            raise BuildError(
                f"private work directory changed: {work}")
        if not stat.S_ISDIR(opened.st_mode) or (opened.st_mode & 0o077) != 0:
            raise BuildError(
                f"private work directory is not private: {work}")
        os.fchmod(work_fd, 0o700)
        return work, work_fd
    except BaseException as error:
        close_errors = []
        if work_fd >= 0:
            try:
                os.close(work_fd)
            except OSError as close_error:
                close_errors.append(str(close_error))
        detail = str(error)
        if close_errors:
            detail += "; descriptor close failed: " + "; ".join(close_errors)
        raise BuildError(
            f"cannot initialize private work directory {work}: {detail}; "
            f"private work retained: {work}") from error


def record_private_entry(work: Path, work_fd: int, name: str,
                         entries: dict[str, tuple[int, int]],
                         anchors: list[int]) -> int:
    path = work / name
    try:
        path_stat = os.stat(name, dir_fd=work_fd, follow_symlinks=False)
    except OSError as error:
        raise BuildError(
            f"cannot inspect private entry {path}: {error}") from error
    if not stat.S_ISREG(path_stat.st_mode):
        raise BuildError(f"private entry is not a regular file: {path}")
    try:
        fd = os.open(name, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW,
                     dir_fd=work_fd)
    except OSError as error:
        raise BuildError(
            f"cannot anchor private entry {path}: {error}") from error
    try:
        opened = os.fstat(fd)
    except OSError as error:
        detail = f"cannot verify private entry {path}: {error}"
        try:
            os.close(fd)
        except OSError as close_error:
            detail += f"; descriptor close failed: {close_error}"
        raise BuildError(detail) from error
    if ((opened.st_dev, opened.st_ino) !=
            (path_stat.st_dev, path_stat.st_ino) or
            not stat.S_ISREG(opened.st_mode)):
        detail = f"private entry changed before anchoring: {path}"
        try:
            os.close(fd)
        except OSError as close_error:
            detail += f"; descriptor close failed: {close_error}"
        raise BuildError(detail)
    anchors.append(fd)
    entries[name] = (opened.st_dev, opened.st_ino)
    return fd


def write_private_metadata(work: Path, work_fd: int, name: str,
                           content: bytes, mode: int,
                           entries: dict[str, tuple[int, int]],
                           anchors: list[int]) -> int:
    path = work / name
    try:
        fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                     os.O_CLOEXEC | os.O_NOFOLLOW, 0o600, dir_fd=work_fd)
    except OSError as error:
        raise BuildError(
            f"cannot create private entry {path}: {error}") from error
    try:
        opened = os.fstat(fd)
    except OSError as error:
        detail = f"cannot verify private entry {path}: {error}"
        try:
            os.close(fd)
        except OSError as close_error:
            detail += f"; descriptor close failed: {close_error}"
        raise BuildError(detail) from error
    entries[name] = (opened.st_dev, opened.st_ino)
    anchors.append(fd)
    try:
        remaining = memoryview(content)
        while remaining:
            try:
                written = os.write(fd, remaining)
            except InterruptedError:
                continue
            if written <= 0:
                raise BuildError(
                    f"private entry write made no progress: {path}")
            remaining = remaining[written:]
        os.fchmod(fd, mode)
        os.fsync(fd)
    except BaseException as error:
        if isinstance(error, BuildError):
            raise
        if isinstance(error, OSError):
            raise BuildError(
                f"cannot write private entry {path}: {error}") from error
        raise
    return fd


def verify_private_entry(work: Path, work_fd: int, name: str,
                         entries: dict[str, tuple[int, int]]) -> None:
    expected = entries.get(name)
    if expected is None:
        raise BuildError(f"private entry was not anchored: {work / name}")
    try:
        current = os.stat(name, dir_fd=work_fd, follow_symlinks=False)
    except OSError as error:
        raise BuildError(
            f"cannot verify private entry {work / name}: {error}") from error
    if ((current.st_dev, current.st_ino) != expected or
            not stat.S_ISREG(current.st_mode)):
        raise BuildError(
            f"private entry changed before publication: {work / name}")


def publish_no_clobber(
        source: Path, target: Path,
        published: list[tuple[Path, int | None, int | None]] | None = None,
        source_fd: int | None = None,
        expected_identity: tuple[int, int] | None = None,
) -> tuple[int, int]:
    check_one_output_target(target)
    try:
        if source_fd is not None:
            source_stat = os.fstat(source_fd)
            link_source = Path(f"/proc/self/fd/{source_fd}")
        else:
            source_stat = os.stat(source, follow_symlinks=False)
            link_source = source
    except OSError as error:
        raise BuildError(
            f"cannot inspect private source {source}: {error}") from error
    if not stat.S_ISREG(source_stat.st_mode):
        raise BuildError(f"private source is not a regular file: {source}")
    if expected_identity is not None and \
            (source_stat.st_dev, source_stat.st_ino) != expected_identity:
        raise BuildError(
            f"private source changed before publication: {source}")
    try:
        # The /proc fd path resolves to the anchored inode, not to a pathname
        # that another process may have replaced.
        os.link(link_source, target, follow_symlinks=True)
    except FileExistsError as error:
        raise BuildError(
            f"refusing to overwrite existing output: {target}") from error
    except OSError as error:
        raise BuildError(f"cannot publish {target}: {error}") from error
    if published is not None:
        published.append((target, None, None))
    try:
        target_stat = os.lstat(target)
    except OSError as error:
        raise BuildError(
            f"cannot verify published output {target}: {error}; "
            f"published output may remain: {target}") from error
    if (not stat.S_ISREG(target_stat.st_mode) or
            (target_stat.st_dev, target_stat.st_ino) !=
            (source_stat.st_dev, source_stat.st_ino)):
        raise BuildError(
            f"published output changed after creation: {target}; "
            f"published output preserved: {target}")
    if published is not None:
        published[-1] = (target, target_stat.st_dev, target_stat.st_ino)
    return target_stat.st_dev, target_stat.st_ino


def verify_published_identity(
        target: Path, record: tuple[Path, int | None, int | None]) -> None:
    device, inode = record[1], record[2]
    if device is None or inode is None:
        raise BuildError(f"unverified published output preserved: {target}")
    try:
        current = os.lstat(target)
    except OSError as error:
        raise BuildError(
            f"cannot verify published output {target}: {error}") from error
    if ((current.st_dev, current.st_ino) != (device, inode) or
            not stat.S_ISREG(current.st_mode)):
        raise BuildError(
            f"published output changed after creation: {target}; "
            f"published output preserved: {target}")


def cleanup_published(
        published: list[tuple[Path, int | None, int | None]]) -> list[str]:
    errors = []
    for target, device, inode in reversed(published):
        try:
            current = os.lstat(target)
            if device is None or inode is None:
                errors.append(
                    f"unverified published output preserved: {target}")
            elif ((current.st_dev, current.st_ino) != (device, inode) or
                  not stat.S_ISREG(current.st_mode)):
                errors.append(
                    f"changed published output preserved: {target}")
            else:
                os.unlink(target)
        except FileNotFoundError:
            pass
        except OSError as error:
            errors.append(
                f"cannot remove owned published output {target}: {error}")
    return errors


def private_work_readlink(work_fd: int) -> str:
    try:
        return os.readlink(f"/proc/self/fd/{work_fd}")
    except OSError as error:
        return f"<unavailable: {error}>"


def cleanup_private_work(
        work: Path, work_fd: int,
        entries: dict[str, tuple[int, int]]) -> list[str]:
    errors = []
    try:
        names = sorted(os.listdir(work_fd))
    except OSError as error:
        actual = private_work_readlink(work_fd)
        return [f"cannot list private work {work}: {error}; "
                f"owned private work fd now at: {actual}; "
                f"private work retained: {actual}"]
    for name in names:
        path = work / name
        try:
            current = os.stat(name, dir_fd=work_fd, follow_symlinks=False)
        except FileNotFoundError:
            continue
        except OSError as error:
            errors.append(f"cannot inspect private entry {path}: {error}")
            continue
        expected = entries.get(name)
        if expected is None:
            errors.append(f"unknown private entry preserved: {path}")
        elif (current.st_dev, current.st_ino) != expected:
            errors.append(f"changed private entry preserved: {path}")
        elif not stat.S_ISREG(current.st_mode):
            errors.append(f"unverified private entry preserved: {path}")
        else:
            try:
                os.unlink(name, dir_fd=work_fd)
            except FileNotFoundError:
                pass
            except OSError as error:
                errors.append(
                    f"cannot remove owned private entry {path}: {error}")
    if errors:
        actual = private_work_readlink(work_fd)
        return errors + [f"private work retained: {actual}"]
    try:
        path_stat = os.lstat(work)
        fd_stat = os.fstat(work_fd)
    except OSError as error:
        actual = private_work_readlink(work_fd)
        return [f"cannot verify private work directory {work}: {error}; "
                f"owned private work fd now at: {actual}; "
                f"private work retained: {actual}"]
    if ((path_stat.st_dev, path_stat.st_ino) !=
            (fd_stat.st_dev, fd_stat.st_ino)):
        actual = private_work_readlink(work_fd)
        return [f"changed private work directory preserved: {work}; "
                f"owned private work fd now at: {actual}; "
                f"private work retained: {actual}"]
    try:
        os.rmdir(work)
    except OSError as error:
        errors.append(
            f"cannot remove private work directory {work}: {error}")
        errors.append(f"private work retained: {work}")
    return errors


def compile_helper(rootfs: Path, out: Path, cc: str) -> dict[str, object]:
    verify_compiler(cc)
    object_file = out / "cp_native_auth.o"
    start_file = out / "cp_native_start.o"
    helper = out / "carplay-native-auth"
    source_dir = NATIVE_C.parent
    compile_flags = [
        "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-fno-pie", "-fstack-protector-strong", "-march=x86-64",
        "-mtune=generic", "-fcf-protection=none",
    ]
    run([cc, *compile_flags, "-I", str(source_dir), "-c", str(NATIVE_C),
         "-o", str(object_file)])
    run([cc, "-c", "-fno-pie", str(NATIVE_START), "-o", str(start_file)])
    link_flags = [
        "-nostdlib", "-no-pie", "-Wl,-e,_start",
        f"-Wl,--dynamic-linker={TARGET_INTERPRETER}",
        "-Wl,-z,relro,-z,now,-z,noexecstack", "-Wl,--fatal-warnings",
    ]
    lib_dir = rootfs / "lib"
    run([
        cc, *link_flags, "-o", str(helper), str(start_file),
        str(object_file), "-L", str(lib_dir), "-Wl,--no-as-needed",
        "-l:libdl-2.25.so", "-l:libc-2.25.so",
    ])
    return {
        "path": str(helper),
        "compile_flags": compile_flags,
        "link_flags": link_flags + [
            "-L", str(lib_dir), "-Wl,--no-as-needed",
            "-l:libdl-2.25.so", "-l:libc-2.25.so",
        ],
    }


def metadata(rootfs: Path, coreutils: Path, out: Path | None,
             check_only: bool) -> dict[str, object]:
    expected_sha = header_define("CP_OEM_AUTH_COREUTILS_SHA256")
    expected_symbols = parse_expected_symbols()
    core = verify_coreutils(coreutils, expected_sha, expected_symbols)
    libs = verify_target_libs(rootfs)
    result: dict[str, object] = {
        "schema": 1,
        "check_only": check_only,
        "rootfs": str(rootfs),
        "coreutils": core,
        "target_libs": libs,
        "target_interpreter": TARGET_INTERPRETER,
        "max_glibc": ".".join(str(part) for part in MAX_GLIBC),
        "activation": {
            "status": "unverified",
            "helper_executed": False,
            "target_dependencies_resolved": False,
            "rootfs_aliases_verified": False,
            "car_validated": False,
            "note": "Build-only. Target loader dependency resolution, rootfs "
                    "aliases, helper/chip execution and vehicle behavior are "
                    "activation-time gates. No OEM code was executed and no "
                    "target symlinks were added.",
        },
    }
    if out is not None and not check_only:
        build_and_publish(rootfs, out, os.environ.get("CC", "gcc"), result)
    return result


def build_and_publish(rootfs: Path, out: Path, cc: str,
                      result: dict[str, object]) -> None:
    cc = verify_compiler(cc)
    ensure_output_dir(out)
    check_output_targets(out)
    work, work_fd = create_private_work(out)
    private_entries: dict[str, tuple[int, int]] = {}
    anchors: list[int] = []
    published: list[tuple[Path, int | None, int | None]] = []
    try:
        try:
            helper = compile_helper(rootfs, work, cc)
            helper_fd = -1
            for name in PRIVATE_SOURCE_NAMES:
                fd = record_private_entry(
                    work, work_fd, name, private_entries, anchors)
                if name == "carplay-native-auth":
                    helper_fd = fd
            helper_path = Path(helper["path"])
            helper_info = verify_helper(helper_path)
            verify_private_entry(
                work, work_fd, "carplay-native-auth", private_entries)
            os.fchmod(helper_fd, 0o755)
            helper_target = out / "carplay-native-auth"
            metadata_target = out / "build-metadata.json"
            helper_info["path"] = str(helper_target)
            result["helper"] = helper_info
            result["build"] = {
                "cc": cc,
                "compile_flags": helper["compile_flags"],
                "link_flags": helper["link_flags"],
            }
            metadata = (json.dumps(result, indent=2, sort_keys=True) +
                        "\n").encode("utf-8")
            metadata_fd = write_private_metadata(
                work, work_fd, "build-metadata.json", metadata, 0o644,
                private_entries, anchors)
            verify_private_entry(
                work, work_fd, "build-metadata.json", private_entries)
            helper_sha = sha256_file(Path(f"/proc/self/fd/{helper_fd}"))
            publish_no_clobber(
                helper_path, helper_target, published, source_fd=helper_fd,
                expected_identity=private_entries["carplay-native-auth"])
            publish_no_clobber(
                work / "build-metadata.json", metadata_target, published,
                source_fd=metadata_fd,
                expected_identity=private_entries["build-metadata.json"])
            helper_record = published[0]
            metadata_record = published[1]
            verify_published_identity(helper_target, helper_record)
            verify_published_identity(metadata_target, metadata_record)
            if sha256_file(helper_target) != helper_sha:
                raise BuildError("published helper readback hash mismatch")
            readback = json.loads(
                metadata_target.read_text(encoding="utf-8"))
            if readback.get("helper", {}).get("sha256") != helper_sha:
                raise BuildError("published metadata helper hash mismatch")
            verify_published_identity(helper_target, helper_record)
            verify_published_identity(metadata_target, metadata_record)
            result["publication"] = {
                "helper": str(helper_target),
                "metadata": str(metadata_target),
                "no_clobber": True,
            }
        except BaseException as error:
            rollback_errors = cleanup_published(published)
            if rollback_errors:
                private_errors = [
                    "private work retained because rollback incomplete: "
                    f"{work}"]
            else:
                private_errors = cleanup_private_work(
                    work, work_fd, private_entries)
            private_errors.extend(close_anchors(anchors))
            detail_parts = []
            if rollback_errors:
                detail_parts.append(
                    "rollback incomplete: " + "; ".join(rollback_errors))
            if private_errors:
                detail_parts.append(
                    "private cleanup incomplete: " +
                    "; ".join(private_errors))
            detail = ("; " + "; ".join(detail_parts)) if detail_parts else ""
            if isinstance(error, Exception):
                raise BuildError(
                    f"cannot publish output: {error}{detail}") from error
            if detail:
                print(f"build_carplay_native: {detail}", file=sys.stderr)
            raise
        else:
            cleanup_errors = cleanup_private_work(
                work, work_fd, private_entries)
            cleanup_errors.extend(close_anchors(anchors))
            result["cleanup"] = {
                "complete": not cleanup_errors,
                "errors": cleanup_errors,
            }
            if cleanup_errors:
                result["cleanup"]["retained"] = str(work)
                raise BuildError(
                    f"output published: helper={helper_target}, "
                    f"metadata={metadata_target}; private cleanup "
                    "incomplete: " + "; ".join(cleanup_errors))
    finally:
        leftover = close_anchors(anchors)
        if leftover:
            print(
                "build_carplay_native: warning: " + "; ".join(leftover),
                file=sys.stderr)
        try:
            os.close(work_fd)
        except OSError as error:
            print(
                "build_carplay_native: warning: cannot close private work "
                f"descriptor: {error}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--rootfs", required=True, type=Path)
    parser.add_argument("--coreutils", type=Path)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()

    rootfs = args.rootfs.resolve()
    coreutils = (args.coreutils if args.coreutils is not None
                 else rootfs / "app" / "lib" / "libCoreUtils.so").resolve()
    # Keep the final component unresolved so a symlinked --out is rejected by
    # ensure_output_dir instead of being silently followed.
    out = args.out.absolute()
    try:
        result = metadata(rootfs, coreutils, out, args.check_only)
    except BuildError as error:
        print(f"build_carplay_native: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
