#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W / IONIQ 5, x86-64, target rootfs glibc 2.25
"""Build the experimental CarPlay broker helper against the target rootfs.

This is a build-only tool.  It reads the owner's rootfs and never executes an
OEM binary.  It links the first-party broker sources with the target libc/
libpthread/libdl/libm/libz/libdbus/libxml2, uses a minimal _start, and refuses
a host-glibc or host-dbus link.  cp_target_compat.c supplies the target-2.25
fstat/fstatat and pre-C23 strtol/strtoul entry points.  Output is an ignored
directory; the helper is not installed or activated by any installer.

Usage:
  python3 packages/wifi1b/scripts/build_carplay_broker.py \
      --rootfs /path/to/rootfs --out packages/wifi1b/out/carplay-broker-target
  ... --check-only
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
SRC = WIFI1B / "src" / "carplay"
START = SRC / "cp_native_start.S"
DEFAULT_OUT = WIFI1B / "out" / "carplay-broker-target"
TARGET_INTERPRETER = "/lib/ld-linux-x86-64.so.2"
MAX_GLIBC = (2, 25)

SOURCES = [
    "cp_broker_main.c", "cp_wire.c", "cp_link.c", "cp_bootstrap.c",
    "cp_session.c", "cp_auth_broker.c", "cp_oem_auth_async.c",
    "cp_spp_dbus.c", "cp_spp_socket.c", "cp_runtime_broker.c",
]
# Compiled separately with -std=gnu11 so the host fstat/fstatat declarations
# are visible; the file itself avoids <stdlib.h> to prevent the C23 redirect.
COMPAT_SOURCE = "cp_target_compat.c"
ALLOWED_NEEDED = {
    "libdbus-1.so.3", "libxml2.so.2", "libpthread.so.0", "libdl.so.2",
    "libm.so.6", "libz.so.1", "libc.so.6",
}


class BuildError(RuntimeError):
    pass


def run(cmd: list[str], cwd: Path | None = None) -> str:
    r = subprocess.run(cmd, cwd=str(cwd) if cwd else None, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=180, check=False)
    if r.returncode != 0:
        raise BuildError(f"{' '.join(cmd)} failed ({r.returncode}):\n"
                         f"{(r.stderr or r.stdout).strip()}")
    return r.stdout


def pkg_config_cflags(package: str) -> list[str]:
    """Host header flags only; never host libraries or link paths."""
    try:
        out = run(["pkg-config", "--cflags-only-I", package])
    except OSError as exc:
        raise BuildError(f"cannot run pkg-config for {package}: {exc}") from exc
    return out.split()


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def elf_soname(p: Path) -> str | None:
    out = run(["readelf", "-d", str(p)])
    m = re.search(r"\(SONAME\)\s+Library soname: \[([^\]]+)\]", out)
    return m.group(1) if m else None


def target_libs(rootfs: Path) -> dict[str, dict[str, str]]:
    expected = {
        "libc": (rootfs / "lib" / "libc-2.25.so", "libc.so.6"),
        "libdl": (rootfs / "lib" / "libdl-2.25.so", "libdl.so.2"),
        "libpthread": (rootfs / "lib" / "libpthread-2.25.so",
                       "libpthread.so.0"),
        "libm": (rootfs / "lib" / "libm-2.25.so", "libm.so.6"),
        "ld": (rootfs / "lib" / "ld-2.25.so", "ld-linux-x86-64.so.2"),
        "libz": (rootfs / "usr" / "lib" / "libz.so.1.2.13", "libz.so.1"),
        "libdbus": (rootfs / "usr" / "lib" / "libdbus-1.so.3.8.13",
                    "libdbus-1.so.3"),
        "libxml2": (rootfs / "usr" / "lib" / "libxml2.so.2.9.14",
                    "libxml2.so.2"),
    }
    result: dict[str, dict[str, str]] = {}
    for name, (p, soname) in expected.items():
        if not p.is_file() or p.is_symlink():
            raise BuildError(f"missing target library: {p}")
        actual = elf_soname(p)
        if actual != soname:
            raise BuildError(f"{name} SONAME {actual!r}, expected {soname!r}")
        result[name] = {"path": str(p), "sha256": sha256_file(p),
                        "size": str(p.stat().st_size), "soname": actual}
    return result


def verify_glibc(path: Path) -> dict[str, object]:
    out = run(["objdump", "-T", str(path)])
    for token in sorted(set(re.findall(r"GLIBC_[A-Za-z0-9_.]+", out))):
        value = token[len("GLIBC_"):]
        if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", value):
            raise BuildError(f"unexpected GLIBC version token {token}")
    versions = sorted(set(re.findall(r"GLIBC_([0-9.]+)", out)))
    parsed = []
    for v in versions:
        parts = v.split(".")
        try:
            t = tuple(int(x) for x in parts[:2])
        except ValueError:
            raise BuildError(f"non-numeric GLIBC version {v}")
        if t > MAX_GLIBC:
            raise BuildError(f"helper requires GLIBC {v} > {MAX_GLIBC}")
        parsed.append(v)
    return {"versions": parsed}


def verify_helper(path: Path) -> dict[str, object]:
    hdr = run(["readelf", "-h", str(path)])
    if "Class:                             ELF64" not in hdr:
        raise BuildError("helper is not ELF64")
    if "Advanced Micro Devices X86-64" not in hdr:
        raise BuildError("helper is not x86-64")
    prog = run(["readelf", "-l", str(path)])
    interp = re.findall(r"\[Requesting program interpreter: ([^\]]+)\]", prog)
    if interp != [TARGET_INTERPRETER]:
        raise BuildError(f"interpreter {interp!r}, expected {TARGET_INTERPRETER!r}")
    dyn = run(["readelf", "-d", str(path)])
    if "RPATH" in dyn or "RUNPATH" in dyn:
        raise BuildError("helper must not contain RPATH/RUNPATH")
    needed = re.findall(r"\(NEEDED\)\s+Shared library: \[([^\]]+)\]", dyn)
    unexpected = [n for n in needed if n not in ALLOWED_NEEDED]
    if unexpected:
        raise BuildError(f"unexpected NEEDED entries: {unexpected}")
    if "libdbus-1.so.3" not in needed or "libxml2.so.2" not in needed:
        raise BuildError("helper is missing the target dbus/xml2 dependencies")

    symbols = run(["readelf", "-sW", str(path)])
    undefined: set[str] = set()
    defined: set[str] = set()
    for line in symbols.splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(":"):
            continue
        name = fields[7].split("@", 1)[0]
        if fields[6] == "UND":
            undefined.add(name)
        else:
            defined.add(name)
    for name in ("__isoc23_strtol", "__isoc23_strtoul", "fstat", "fstatat"):
        if name in undefined:
            raise BuildError(f"helper still has undefined target-absent {name}")
        if name not in defined:
            raise BuildError(f"helper is missing compat shim {name}")
    if "GLIBC_PRIVATE" in run(["readelf", "-V", str(path)]):
        raise BuildError("helper has its own GLIBC_PRIVATE version reference")
    return {"needed": needed,
            "compat_shims": ["fstat", "fstatat", "__isoc23_strtol",
                             "__isoc23_strtoul"],
            **verify_glibc(path)}


def build(rootfs: Path, out: Path) -> dict[str, object]:
    libs = target_libs(rootfs)
    cc = os.environ.get("CC", "gcc")
    if not out.is_absolute():
        raise BuildError("--out must be absolute")
    if out.exists():
        if out.is_symlink() or not out.is_dir():
            raise BuildError("output is not a real directory")
        if any(out.iterdir()):
            raise BuildError("output directory must be empty")
    else:
        out.mkdir(parents=True, mode=0o700)
        out.chmod(0o700)
    flags = [
        "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
        "-fno-pie", "-fstack-protector-strong", "-march=x86-64",
        "-mtune=generic", "-fcf-protection=none",
        "-I", str(SRC), *pkg_config_cflags("dbus-1"),
        *pkg_config_cflags("libxml-2.0"),
    ]
    objs: list[str] = []
    for name in SOURCES:
        obj = out / (name + ".o")
        run([cc, *flags, "-c", str(SRC / name), "-o", str(obj)])
        objs.append(str(obj))
    compat_flags = ["-std=gnu11" if f == "-std=c11" else f for f in flags]
    compat_obj = out / "cp_target_compat.o"
    run([cc, *compat_flags, "-c", str(SRC / COMPAT_SOURCE),
         "-o", str(compat_obj)])
    start_obj = out / "cp_native_start.o"
    run([cc, "-c", "-fno-pie", str(START), "-o", str(start_obj)])
    helper = out / "carplay-broker"
    r = rootfs
    link = [
        "-nostdlib", "-no-pie", "-Wl,-e,_start",
        f"-Wl,--dynamic-linker={TARGET_INTERPRETER}",
        "-Wl,-z,relro,-z,now,-z,noexecstack", "-Wl,--fatal-warnings",
        # The paired 2.25 libpthread/libdl/libm/libc reference GLIBC_PRIVATE
        # symbols supplied by the matching target ld.so at runtime (for
        # example _dl_make_stack_executable).  This flag relaxes only
        # shared-library-internal undefineds; our own objects are still fully
        # checked.  Loadability requires the matching 2.25 runtime.
        "-Wl,--allow-shlib-undefined",
        "-o", str(helper), str(start_obj), *objs, str(compat_obj),
        "-L", str(r / "usr" / "lib"), "-L", str(r / "lib"),
        "-Wl,--no-as-needed",
        "-l:libdbus-1.so.3.8.13", "-l:libxml2.so.2.9.14",
        "-l:libz.so.1.2.13", "-l:libpthread-2.25.so",
        "-l:libdl-2.25.so", "-l:libm-2.25.so", "-l:libc-2.25.so",
    ]
    run([cc, *link])
    info = verify_helper(helper)
    mode = stat.S_IMODE(helper.stat().st_mode) & 0o777
    helper.chmod(mode | 0o755)
    manifest = {
        "schema": "scourge.carplay.broker-target.v1",
        "status": "EXPERIMENTAL_BUILD_ONLY_NOT_CAR_VALIDATED",
        "car_validated": False,
        "installer_activated": False,
        "oem_binary_executed": False,
        "rootfs": str(rootfs),
        "libraries": libs,
        "helper": {
            "path": str(helper),
            "sha256": sha256_file(helper),
            "size": helper.stat().st_size,
            **info,
        },
        "sources": [*SOURCES, COMPAT_SOURCE],
    }
    (out / "build-metadata.json").write_text(json.dumps(manifest, indent=2))
    return manifest


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--rootfs", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT)
    ap.add_argument("--check-only", action="store_true")
    args = ap.parse_args(argv)
    try:
        libs = target_libs(args.rootfs)
        if args.check_only:
            print(json.dumps({"ok": True, "libraries": libs}, indent=2))
            return 0
        manifest = build(args.rootfs, args.out.resolve())
        print(json.dumps({"ok": True, "helper": manifest["helper"]}, indent=2))
        return 0
    except (BuildError, OSError) as exc:
        print(f"REFUSED: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
