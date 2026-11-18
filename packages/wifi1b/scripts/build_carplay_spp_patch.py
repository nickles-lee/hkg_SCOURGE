#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Build the C3 experimental vbtd-63/vbtd-51 SPP RX hook patch.

TARGET: Hyundai Ioniq 5 Gen5W_L, EU firmware NE_E.EUR.S5W_L.001.001.260128,
        stock /app/bin/vbtd-63 or vbtd-51.

This is a BUILD-ONLY artifact generator.  It never loads, executes, or
activates the OEM binary, and it never writes to the input file.  The Wifi1b
installer does not consume its output.

What it does:
  1. Requires a user-supplied stock vbtd target and a user-chosen ignored output
     directory.
  2. Gates the whole input by the target VBTD*_STOCK_SHA256/SIZE from
     config/firmware_locks.env.
  3. Maps the pinned ELF vaddr to a file offset through an executable PT_LOAD
     and requires that mapping to equal VBTD*_SPP_RX_OFFSET.
  4. Gates the exact VBTSppReceivedDataCfm preimage by
     VBTD*_SPP_RX_STOCK_SHA256 and VBTD*_SPP_RX_LEN.
  5. Compiles the freestanding first-party hook, links it PIC at the pinned
     fixed address, and extracts one contiguous .text+.rodata blob.
  6. Rejects relocations, undefined symbols, unexpected allocated sections,
     oversize blobs, non-ret terminal text, and missing default wire strings.
  7. Writes a copy with only the pinned function span replaced; remaining
     bytes in that span are NOP-filled.  It verifies every byte outside the
     span is unchanged before publishing.
  8. Writes an experimental-not-car-validated manifest with input/output/blob
     SHA256 values.

Usage:
  python3 packages/wifi1b/scripts/build_carplay_spp_patch.py \
      --input /path/to/vbtd-XX --out /tmp/opencode/carplay-spp-patch --target XX
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
PKG_ROOT = HERE.parent
REPO_ROOT = PKG_ROOT.parent.parent
HOOK_SOURCE = PKG_ROOT / "src" / "carplay" / "cp_spp_rx_hook.c"
HOOK_HEADER = PKG_ROOT / "src" / "carplay" / "cp_spp_rx.h"
DEFAULT_LOCKS = REPO_ROOT / "config" / "firmware_locks.env"
BUILD_ROOT = Path("/tmp/opencode")

# Pinned targets.  FUNCTION_VADDR/FUNCTION_LEN are set per build so the
# existing hook/link/test helpers remain single-target code.
# Lock keys are selected per target:
#   VBTD63_STOCK_SHA256, VBTD63_SIZE, VBTD63_SPP_RX_VADDR,
#   VBTD63_SPP_RX_OFFSET, VBTD63_SPP_RX_LEN,
#   VBTD63_SPP_RX_STOCK_SHA256
#   VBTD51_STOCK_SHA256, VBTD51_SIZE, VBTD51_SPP_RX_VADDR,
#   VBTD51_SPP_RX_OFFSET, VBTD51_SPP_RX_LEN,
#   VBTD51_SPP_RX_STOCK_SHA256
TARGETS = {
    "63": {"name": "vbtd-63", "vaddr": 0x491C90, "len": 0x355,
           "prefix": "VBTD63"},
    "51": {"name": "vbtd-51", "vaddr": 0x490EC0, "len": 0x355,
           "prefix": "VBTD51"},
}
FUNCTION_LEN = TARGETS["63"]["len"]
FUNCTION_VADDR = TARGETS["63"]["vaddr"]

PT_LOAD = 1
PF_X = 1
SHF_ALLOC = 0x2
SHT_NOBITS = 8
SHT_REL = 9
SHT_RELA = 4
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHN_UNDEF = 0
STB_GLOBAL = 1
STT_FUNC = 2
STT_OBJECT = 1
STT_FILE = 4

DEFAULT_PATH = b"/run/wifi1b/carplay/rx.sock"
UUID_CAFF = b"00000000-DECA-FADE-DECA-DEAFDECACAFF"
UUID_CAFE = b"00000000-DECA-FADE-DECA-DEAFDECACAFE"
UUID_CARPLAY = b"EC884348-CD41-40A2-9727-575D50BF1FD3"

HEX64_RE = re.compile(r"^[0-9a-fA-F]{64}$")
LOCK_RE = re.compile(
    r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
    r'(?:"([^"]*)"|(\S+))\s*(?:#.*)?$'
)


class BuildError(RuntimeError):
    """A gate or build-input failure; the caller must not publish output."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_locks(path: Path) -> dict[str, str]:
    """Read the simple KEY="value" firmware-lock file without executing it."""
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise BuildError(f"locks file is not a regular non-symlink file: {path}")
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise BuildError(f"cannot read locks file {path}: {exc}") from exc

    locks: dict[str, str] = {}
    for line_no, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        match = LOCK_RE.match(stripped)
        if match is None:
            raise BuildError(f"{path}:{line_no}: unsupported lock line")
        key = match.group(1)
        value = match.group(2) if match.group(2) is not None else match.group(3)
        if key in locks:
            raise BuildError(f"{path}:{line_no}: duplicate key {key}")
        locks[key] = value
    return locks


def _required(locks: dict[str, str], key: str) -> str:
    value = locks.get(key)
    if not value:
        raise BuildError(f"missing required firmware lock {key}")
    return value


def _required_int(locks: dict[str, str], key: str) -> int:
    raw = _required(locks, key)
    try:
        value = int(raw, 0)
    except ValueError as exc:
        raise BuildError(f"{key} is not an integer: {raw!r}") from exc
    if value < 0:
        raise BuildError(f"{key} must be non-negative")
    return value


def _required_sha256(locks: dict[str, str], key: str) -> str:
    raw = _required(locks, key)
    if HEX64_RE.fullmatch(raw) is None:
        raise BuildError(f"{key} is not a 64-hex SHA256")
    return raw.lower()


def _elf_header(data: bytes) -> dict[str, int]:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise BuildError("input is not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise BuildError("input ELF is not 64-bit little-endian")
    fields = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    header = {
        "type": fields[0],
        "machine": fields[1],
        "version": fields[2],
        "entry": fields[3],
        "phoff": fields[4],
        "shoff": fields[5],
        "flags": fields[6],
        "ehsize": fields[7],
        "phentsize": fields[8],
        "phnum": fields[9],
        "shentsize": fields[10],
        "shnum": fields[11],
        "shstrndx": fields[12],
    }
    if header["machine"] != 62:
        raise BuildError("input ELF is not x86-64")
    return header


def _program_headers(data: bytes, header: dict[str, int]) -> list[dict[str, int]]:
    phnum = header["phnum"]
    phoff = header["phoff"]
    phentsize = header["phentsize"]
    if phnum == 0:
        return []
    if phentsize != 56:
        raise BuildError("unsupported ELF program-header size")
    if phoff < 64 or phoff + phnum * phentsize > len(data):
        raise BuildError("ELF program headers are outside the file")
    result = []
    for index in range(phnum):
        offset = phoff + index * phentsize
        field = struct.unpack_from("<IIQQQQQQ", data, offset)
        result.append(
            {
                "index": index,
                "type": field[0],
                "flags": field[1],
                "offset": field[2],
                "vaddr": field[3],
                "paddr": field[4],
                "filesz": field[5],
                "memsz": field[6],
                "align": field[7],
            }
        )
    return result


def _section_headers(data: bytes, header: dict[str, int]) -> list[dict[str, object]]:
    shnum = header["shnum"]
    shoff = header["shoff"]
    shentsize = header["shentsize"]
    shstrndx = header["shstrndx"]
    if shnum == 0:
        return []
    if shentsize != 64:
        raise BuildError("unsupported ELF section-header size")
    if shoff < 64 or shoff + shnum * shentsize > len(data):
        raise BuildError("ELF section headers are outside the file")
    if shstrndx >= shnum:
        raise BuildError("ELF section-name table index is invalid")

    raw = []
    for index in range(shnum):
        offset = shoff + index * shentsize
        field = struct.unpack_from("<IIQQQQIIQQ", data, offset)
        raw.append(
            {
                "index": index,
                "name_off": field[0],
                "type": field[1],
                "flags": field[2],
                "addr": field[3],
                "offset": field[4],
                "size": field[5],
                "link": field[6],
                "info": field[7],
                "addralign": field[8],
                "entsize": field[9],
            }
        )

    shstr = raw[shstrndx]
    if shstr["type"] != SHT_STRTAB:
        raise BuildError("ELF section-name table is not a string table")
    if shstr["offset"] + shstr["size"] > len(data):
        raise BuildError("ELF section-name table is outside the file")
    shstr_data = data[shstr["offset"] : shstr["offset"] + shstr["size"]]

    for entry in raw:
        start = int(entry["name_off"])
        if start >= len(shstr_data):
            raise BuildError("ELF section name offset is outside the table")
        end = shstr_data.find(b"\x00", start)
        if end < 0:
            raise BuildError("ELF section name is not terminated")
        entry["name"] = shstr_data[start:end].decode("ascii", "replace")
    return raw


def _map_vaddr(data: bytes, phdrs: list[dict[str, int]], vaddr: int,
               length: int) -> int:
    if length <= 0:
        raise BuildError("function length must be positive")
    matches = []
    for ph in phdrs:
        if ph["type"] != PT_LOAD or not (ph["flags"] & PF_X):
            continue
        start = ph["vaddr"]
        end = start + ph["filesz"]
        if start <= vaddr and vaddr + length <= end:
            file_off = ph["offset"] + (vaddr - start)
            matches.append((file_off, ph))
    if not matches:
        raise BuildError(
            "pinned function vaddr is not fully covered by one executable "
            "PT_LOAD"
        )
    if len(matches) > 1:
        raise BuildError("pinned function vaddr matches multiple PT_LOADs")
    file_off, segment = matches[0]
    if file_off < 0 or file_off + length > len(data):
        raise BuildError("mapped function span is outside the input file")
    if segment["offset"] + segment["filesz"] > len(data):
        raise BuildError("executable PT_LOAD exceeds the input file")
    return file_off


def _reject_symlink_components(path: Path, what: str) -> None:
    if ".." in path.parts:
        raise BuildError(f"{what} path contains a parent-directory component")
    absolute = Path(os.path.abspath(path))
    for component in [absolute, *absolute.parents]:
        if component.is_symlink():
            raise BuildError(f"{what} path component is a symlink: {component}")


def _is_ignored_outdir(path: Path) -> bool:
    resolved = path.resolve(strict=False)
    tmp_root = BUILD_ROOT.resolve(strict=False)
    if resolved == tmp_root or tmp_root in resolved.parents:
        return True

    try:
        repo = subprocess.run(
            ["git", "-C", str(HERE), "rev-parse", "--show-toplevel"],
            check=True,
            capture_output=True,
            text=True,
            timeout=30,
        ).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return False
    if not repo:
        return False
    try:
        rel = os.path.relpath(resolved, repo)
    except ValueError:
        return False
    if rel == ".." or rel.startswith(".." + os.sep):
        return False
    ignored = subprocess.run(
        ["git", "-C", repo, "check-ignore", "-q", "--", rel],
        check=False,
        timeout=30,
    )
    return ignored.returncode == 0


def _linker_script() -> str:
    script = """OUTPUT_FORMAT("elf64-x86-64")
ENTRY(cp_spp_rx_hook)
SECTIONS
{
  . = 0x491c90;
  .text : SUBALIGN(1)
  {
    KEEP(*(.text.cp_spp_rx_hook))
    *(.text.cp_spp_rx_hook.*)
    *(.text*)
  }
  .rodata : SUBALIGN(1)
  {
    *(.rodata*)
  }
  .comment 0 : { *(.comment) }
  .note.GNU-stack 0 : { *(.note.GNU-stack) }
  /* Compiler/ELF placeholders.  They must remain zero-sized; the validator
   * rejects any non-empty allocated section outside .text/.rodata. */
  .data 0 : { *(.data) }
  .bss 0 (NOLOAD) : { *(.bss) }
  .got 0 : { *(.got) }
  .got.plt 0 : { *(.got.plt) }
  .iplt 0 : { *(.iplt) }
  .igot.plt 0 : { *(.igot.plt) }
  .rela.got 0 : { *(.rela.got) }
  .rela.iplt 0 : { *(.rela.iplt) }
  /DISCARD/ : { *(.note.gnu.property) *(.note.GNU-property) }
}
"""
    return script.replace("0x491c90", f"{FUNCTION_VADDR:#x}")

def _decode_symbols(data: bytes, sections: list[dict[str, object]]) -> list[dict[str, object]]:
    symtab = None
    for section in sections:
        if section["type"] == SHT_SYMTAB:
            if symtab is not None:
                raise BuildError("multiple ELF symbol tables")
            symtab = section
    if symtab is None:
        raise BuildError("hook ELF has no symbol table")

    strtab_index = int(symtab["link"])
    if strtab_index >= len(sections):
        raise BuildError("hook ELF symbol string-table link is invalid")
    strtab = sections[strtab_index]
    if strtab["type"] != SHT_STRTAB:
        raise BuildError("hook ELF symbol link is not a string table")
    if strtab["offset"] + strtab["size"] > len(data):
        raise BuildError("hook ELF symbol string table is outside the file")
    names = data[strtab["offset"] : strtab["offset"] + strtab["size"]]

    if symtab["offset"] + symtab["size"] > len(data):
        raise BuildError("hook ELF symbol table is outside the file")
    ent = int(symtab["entsize"])
    if ent != 24:
        raise BuildError("unsupported hook ELF symbol size")

    symbols = []
    for offset in range(symtab["offset"], symtab["offset"] + symtab["size"], ent):
        name_off, info, other, shndx, value, size = struct.unpack_from(
            "<IBBHQQ", data, offset
        )
        if name_off >= len(names):
            raise BuildError("hook ELF symbol name offset is outside the table")
        end = names.find(b"\x00", name_off)
        if end < 0:
            raise BuildError("hook ELF symbol name is not terminated")
        symbols.append(
            {
                "name": names[name_off:end].decode("ascii", "replace"),
                "bind": info >> 4,
                "type": info & 0xF,
                "shndx": shndx,
                "value": value,
                "size": size,
            }
        )
    return symbols


def _validate_hook_elf(data: bytes) -> dict[str, object]:
    header = _elf_header(data)
    if header["type"] != 2:
        raise BuildError("hook ELF must be a static ET_EXEC image")
    sections = _section_headers(data, header)
    if not sections:
        raise BuildError("hook ELF has no section headers")

    relocs = [
        s for s in sections
        if s["type"] in (SHT_REL, SHT_RELA) and int(s["size"]) > 0
    ]
    if relocs:
        names = ", ".join(str(s["name"]) for s in relocs)
        raise BuildError(f"hook ELF contains relocations: {names}")

    alloc = []
    for section in sections:
        name = str(section["name"])
        size = int(section["size"])
        flags = int(section["flags"])
        if section["type"] == SHT_NOBITS and size > 0:
            raise BuildError(f"hook ELF contains non-empty NOBITS section {name}")
        if flags & SHF_ALLOC:
            if size == 0 and name in (
                ".data",
                ".bss",
                ".got",
                ".got.plt",
                ".iplt",
                ".igot.plt",
            ):
                continue
            if not (name.startswith(".text") or name.startswith(".rodata")):
                raise BuildError(
                    f"hook ELF contains unexpected allocated section {name}"
                )
            if section["type"] == SHT_NOBITS:
                raise BuildError(f"hook ELF allocated section {name} is NOBITS")
            alloc.append(section)

    if not alloc:
        raise BuildError("hook ELF has no allocated .text/.rodata content")

    for section in alloc:
        offset = int(section["offset"])
        size = int(section["size"])
        if offset < 0 or size <= 0 or offset + size > len(data):
            raise BuildError(
                f"hook ELF section {section['name']} is outside the file"
            )

    alloc.sort(key=lambda s: int(s["addr"]))
    first = alloc[0]
    if not str(first["name"]).startswith(".text"):
        raise BuildError("hook ELF first allocated section is not .text")
    if int(first["addr"]) != FUNCTION_VADDR:
        raise BuildError(
            f"hook ELF .text starts at {int(first['addr']):#x}, "
            f"expected {FUNCTION_VADDR:#x}"
        )

    expected = FUNCTION_VADDR
    saw_rodata = False
    for section in alloc:
        name = str(section["name"])
        addr = int(section["addr"])
        size = int(section["size"])
        if addr != expected:
            raise BuildError(
                f"hook ELF allocation is not contiguous at {name}: "
                f"{addr:#x} != {expected:#x}"
            )
        if name.startswith(".rodata"):
            saw_rodata = True
        elif saw_rodata:
            raise BuildError("hook ELF has .text after .rodata")
        expected += size

    if not saw_rodata:
        raise BuildError("hook ELF has no .rodata")

    blob = b"".join(
        data[int(s["offset"]) : int(s["offset"]) + int(s["size"])] for s in alloc
    )
    if len(blob) != expected - FUNCTION_VADDR:
        raise BuildError("hook ELF blob span is inconsistent")
    if len(blob) > FUNCTION_LEN:
        raise BuildError(
            f"hook blob is {len(blob)} bytes, exceeds function extent "
            f"{FUNCTION_LEN} bytes"
        )
    text_sections = [s for s in alloc if str(s["name"]).startswith(".text")]
    if not text_sections:
        raise BuildError("hook ELF has no .text")
    last_text = text_sections[-1]
    last_text_bytes = data[
        int(last_text["offset"]) : int(last_text["offset"]) + int(last_text["size"])
    ]
    if not last_text_bytes.endswith(b"\xc3"):
        raise BuildError(
            "hook .text does not end in a ret; refusing fall-through into rodata"
        )

    for needle, label in (
        (DEFAULT_PATH, "default socket path"),
        (UUID_CAFF, "CAFF UUID"),
        (UUID_CAFE, "CAFE UUID"),
        (UUID_CARPLAY, "CarPlay UUID"),
    ):
        if needle not in blob:
            raise BuildError(f"hook blob is missing {label}")

    symbols = _decode_symbols(data, sections)
    undefined = [
        s["name"]
        for s in symbols
        if s["shndx"] == SHN_UNDEF
        and s["name"]
        and s["type"] != STT_FILE
    ]
    if undefined:
        raise BuildError(
            "hook ELF has undefined symbols: " + ", ".join(sorted(set(undefined)))
        )

    global_funcs = [
        s
        for s in symbols
        if s["bind"] == STB_GLOBAL and s["type"] == STT_FUNC and s["size"] > 0
    ]
    if len(global_funcs) != 1:
        raise BuildError(
            "hook ELF must export exactly one defined function, found "
            + ", ".join(s["name"] for s in global_funcs)
        )
    hook = global_funcs[0]
    if hook["name"] != "cp_spp_rx_hook":
        raise BuildError(f"unexpected hook symbol {hook['name']}")
    if hook["value"] != FUNCTION_VADDR:
        raise BuildError(
            f"hook entry is {hook['value']:#x}, expected {FUNCTION_VADDR:#x}"
        )
    if hook["shndx"] == SHN_UNDEF or hook["shndx"] >= len(sections):
        raise BuildError("hook symbol section index is invalid")
    hook_section = sections[hook["shndx"]]
    if not str(hook_section["name"]).startswith(".text"):
        raise BuildError("hook symbol is not in .text")

    global_objects = [
        s
        for s in symbols
        if s["bind"] == STB_GLOBAL and s["type"] == STT_OBJECT and s["size"] > 0
    ]
    if global_objects:
        raise BuildError(
            "hook ELF contains global writable data: "
            + ", ".join(s["name"] for s in global_objects)
        )

    return {
        "blob": blob,
        "blob_sha256": sha256_bytes(blob),
        "blob_size": len(blob),
        "text_sections": [s["name"] for s in alloc if str(s["name"]).startswith(".text")],
        "rodata_sections": [
            s["name"] for s in alloc if str(s["name"]).startswith(".rodata")
        ],
    }


def _tool_identity(command: str) -> dict[str, str]:
    executable = shutil.which(command)
    if executable is None:
        raise BuildError(f"build tool not found: {command}")
    try:
        version = subprocess.run(
            [executable, "--version"], check=True, capture_output=True,
            text=True, timeout=10,
        ).stdout.splitlines()
        digest = sha256_bytes(Path(executable).read_bytes())
    except (OSError, subprocess.SubprocessError) as exc:
        raise BuildError(f"cannot identify build tool {command}: {exc}") from exc
    if not version or len(version[0]) > 1024:
        raise BuildError(f"invalid build tool version: {command}")
    return {"name": Path(executable).name, "version": version[0], "sha256": digest}


def build_blob(hook_source: Path = HOOK_SOURCE) -> dict[str, object]:
    """Compile and link the first-party hook into a validated contiguous blob."""
    hook_source = Path(hook_source)
    if hook_source.is_symlink() or not hook_source.is_file():
        raise BuildError(f"hook source is not a regular non-symlink file: {hook_source}")
    if not HOOK_HEADER.is_file():
        raise BuildError(f"hook header not found: {HOOK_HEADER}")

    BUILD_ROOT.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get("CP_SPP_RX_CC", os.environ.get("CC", "cc"))
    ld = os.environ.get("CP_SPP_RX_LD", os.environ.get("LD", "ld"))
    toolchain = {"compiler": _tool_identity(cc), "linker": _tool_identity(ld)}

    with tempfile.TemporaryDirectory(prefix="cp-spp-rx-", dir=str(BUILD_ROOT)) as tmp:
        work = Path(tmp)
        script = work / "hook.ld"
        obj = work / "hook.o"
        elf = work / "hook.elf"
        script.write_text(_linker_script(), encoding="ascii")

        compile_cmd = [
            cc,
            "-std=c11",
            "-Os",
            "-ffreestanding",
            "-fPIC",
            "-fno-stack-protector",
            "-fno-builtin",
            "-fno-tree-loop-distribute-patterns",
            "-fno-asynchronous-unwind-tables",
            "-fno-unwind-tables",
            "-fno-common",
            "-ffunction-sections",
            "-fdata-sections",
            "-fvisibility=hidden",
            "-fno-ident",
            "-fcf-protection=none",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(HOOK_HEADER.parent),
            "-I",
            str(hook_source.parent),
            "-c",
            str(hook_source),
            "-o",
            str(obj),
        ]
        try:
            subprocess.run(
                compile_cmd, check=True, capture_output=True, text=True, timeout=120
            )
        except subprocess.CalledProcessError as exc:
            raise BuildError(
                "hook compile failed:\n" + (exc.stderr or exc.stdout or "")
            ) from exc

        link_cmd = [
            ld,
            "-nostdlib",
            "-static",
            "-T",
            str(script),
            "--orphan-handling=error",
            "--build-id=none",
            "-z",
            "noexecstack",
            "-o",
            str(elf),
            str(obj),
        ]
        try:
            subprocess.run(
                link_cmd, check=True, capture_output=True, text=True, timeout=120
            )
        except subprocess.CalledProcessError as exc:
            raise BuildError(
                "hook link failed:\n" + (exc.stderr or exc.stdout or "")
            ) from exc

        try:
            linked = elf.read_bytes()
        except OSError as exc:
            raise BuildError(f"cannot read linked hook: {exc}") from exc

    result = _validate_hook_elf(linked)
    result["source_sha256"] = sha256_bytes(hook_source.read_bytes())
    result["header_sha256"] = sha256_bytes(HOOK_HEADER.read_bytes())
    # Preserve flags without embedding private host paths in the manifest.
    result["toolchain"] = toolchain
    result["compile_flags"] = compile_cmd[1:compile_cmd.index("-I")] + [
        "-I", "<header-dir>", "-I", "<source-dir>",
        "-c", "<source>", "-o", "<object>",
    ]
    result["link_flags"] = [
        "-nostdlib", "-static", "-T", "<linker-script>",
        "--orphan-handling=error", "--build-id=none", "-z", "noexecstack",
        "-o", "<hook-elf>", "<object>",
    ]
    result["linker_script_sha256"] = sha256_bytes(_linker_script().encode("ascii"))
    return result


def _verify_outside_delta(original: bytes, patched: bytes, offset: int,
                          length: int, blob: bytes) -> None:
    if len(original) != len(patched):
        raise BuildError("patched output size changed")
    if patched[:offset] != original[:offset]:
        raise BuildError("bytes before the patched span changed")
    if patched[offset + length :] != original[offset + length :]:
        raise BuildError("bytes after the patched span changed")
    if patched[offset : offset + len(blob)] != blob:
        raise BuildError("blob readback mismatch")
    padding = patched[offset + len(blob) : offset + length]
    if padding != b"\x90" * (length - len(blob)):
        raise BuildError("NOP padding readback mismatch")
    if patched == original:
        raise BuildError("patched output is identical to input")


def _cleanup_owned_files(records: list[tuple[Path, int | None, int | None]],
                         label: str) -> list[str]:
    errors = []
    for path, device, inode in reversed(records):
        try:
            current = path.lstat()
            if device is None or inode is None:
                errors.append(f"unverified {label} preserved: {path}")
            elif (current.st_dev, current.st_ino) != (device, inode):
                errors.append(f"changed {label} preserved: {path}")
            else:
                path.unlink()
        except FileNotFoundError:
            pass
        except OSError as exc:
            errors.append(f"cannot remove owned {label} {path}: {exc}")
    return errors


def _create_private_temp(prefix: str, directory: Path,
                         records: list[tuple[Path, int | None, int | None]],
                         anchors: list[int]) -> tuple[int, Path]:
    fd, name = tempfile.mkstemp(prefix=prefix, dir=str(directory))
    # Own the raw descriptor until the outer finally. No fdopen transfer can
    # fail between descriptor creation and registration.
    anchors.append(fd)
    path = Path(name)
    index = len(records)
    records.append((path, None, None))
    try:
        identity = os.fstat(fd)
    except OSError:
        # Abort on the failed gate, but recover identity for cleanup through
        # the still-open descriptor when /proc is available. Otherwise retain
        # and explicitly report the unverified path instead of deleting it.
        try:
            identity = os.stat(f"/proc/self/fd/{fd}")
        except OSError:
            pass
        else:
            records[index] = (path, identity.st_dev, identity.st_ino)
        raise
    records[index] = (path, identity.st_dev, identity.st_ino)
    return fd, path


def _write_private_temp(fd: int, content: bytes, mode: int) -> None:
    remaining = memoryview(content)
    while remaining:
        try:
            written = os.write(fd, remaining)
        except InterruptedError:
            continue
        if written <= 0:
            raise OSError("private output write made no progress")
        remaining = remaining[written:]
    os.fchmod(fd, mode)
    os.fsync(fd)


def build_patch(input_path: Path, out_dir: Path,
                locks_path: Path = DEFAULT_LOCKS,
                *, target: str = "63",
                allow_test_geometry: bool = False) -> dict[str, object]:
    """Gate, patch, verify, and write the experimental output copy.

    ``allow_test_geometry`` is for offline synthetic-ELF tests only.  The CLI
    never exposes it.  It permits a test input whose function VADDR is not the
    production pinned address; the hook blob is still linked at the production
    address and the output is never executed.
    """
    if target not in TARGETS:
        raise BuildError(f"unsupported target {target!r}")
    meta = TARGETS[target]
    global FUNCTION_VADDR, FUNCTION_LEN
    FUNCTION_VADDR = meta["vaddr"]
    FUNCTION_LEN = meta["len"]
    prefix = meta["prefix"]
    target_name = meta["name"]

    input_path = Path(input_path)
    out_dir = Path(out_dir)
    locks_path = Path(locks_path)

    if not allow_test_geometry and locks_path.resolve() != DEFAULT_LOCKS.resolve():
        raise BuildError("production builds require canonical config/firmware_locks.env")

    _reject_symlink_components(input_path, "input")
    if input_path.is_symlink() or not input_path.is_file():
        raise BuildError(f"input is not a regular non-symlink file: {input_path}")

    locks = load_locks(locks_path)
    stock_size = _required_int(locks, f"{prefix}_SIZE")
    stock_sha = _required_sha256(locks, f"{prefix}_STOCK_SHA256")
    function_vaddr = _required_int(locks, f"{prefix}_SPP_RX_VADDR")
    function_offset = _required_int(locks, f"{prefix}_SPP_RX_OFFSET")
    function_len = _required_int(locks, f"{prefix}_SPP_RX_LEN")
    function_sha = _required_sha256(locks, f"{prefix}_SPP_RX_STOCK_SHA256")

    if not allow_test_geometry and function_vaddr != FUNCTION_VADDR:
        raise BuildError(
            f"{prefix}_SPP_RX_VADDR is {function_vaddr:#x}, "
            f"expected {FUNCTION_VADDR:#x}"
        )
    if function_len != FUNCTION_LEN:
        raise BuildError(
            f"{prefix}_SPP_RX_LEN is {function_len:#x}, expected {FUNCTION_LEN:#x}"
        )

    try:
        input_stat = input_path.stat()
    except OSError as exc:
        raise BuildError(f"cannot stat input: {exc}") from exc
    if input_stat.st_size != stock_size:
        raise BuildError(
            f"whole input size {input_stat.st_size} != pinned {stock_size}"
        )
    try:
        data = input_path.read_bytes()
    except OSError as exc:
        raise BuildError(f"cannot read input: {exc}") from exc
    if len(data) != stock_size:
        raise BuildError("input size changed while reading")
    whole_sha = sha256_bytes(data)
    if whole_sha != stock_sha:
        raise BuildError(
            f"whole input SHA256 {whole_sha} != pinned {stock_sha}"
        )

    header = _elf_header(data)
    if header["type"] not in (2, 3):
        raise BuildError("input ELF is not ET_EXEC or ET_DYN")
    phdrs = _program_headers(data, header)
    mapped = _map_vaddr(data, phdrs, function_vaddr, function_len)
    if mapped != function_offset:
        raise BuildError(
            f"ELF mapping gives file offset {mapped:#x}, "
            f"pinned {function_offset:#x}"
        )
    if function_offset + function_len > len(data):
        raise BuildError("pinned function span exceeds the input file")

    preimage = data[function_offset : function_offset + function_len]
    preimage_sha = sha256_bytes(preimage)
    if preimage_sha != function_sha:
        raise BuildError(
            f"function preimage SHA256 {preimage_sha} != pinned {function_sha}"
        )

    blob_info = build_blob()
    blob = blob_info["blob"]
    if not isinstance(blob, bytes):
        raise BuildError("internal hook blob type error")
    if len(blob) > function_len:
        raise BuildError("hook blob exceeds the pinned function span")

    _reject_symlink_components(out_dir, "output")
    if out_dir.exists() and (out_dir.is_symlink() or not out_dir.is_dir()):
        raise BuildError(f"output is not a real directory: {out_dir}")
    if not out_dir.exists():
        try:
            out_dir.mkdir(parents=True, mode=0o700)
        except OSError as exc:
            raise BuildError(f"cannot create output directory: {exc}") from exc
    if not _is_ignored_outdir(out_dir):
        raise BuildError(
            "output directory is not ignored by Git and is not under "
            "/tmp/opencode"
        )

    output_file = out_dir / f"{target_name}.spp-rx.patched"
    manifest_file = out_dir / f"{target_name}.spp-rx.manifest.json"
    for candidate in (output_file, manifest_file):
        if candidate.is_symlink():
            raise BuildError(f"refusing symlink output path: {candidate}")
        if candidate.exists():
            raise BuildError(f"refusing existing output path: {candidate}")

    try:
        if output_file.exists() and os.path.samefile(output_file, input_path):
            raise BuildError("refusing output/input alias")
    except OSError:
        pass
    if output_file.resolve(strict=False) == input_path.resolve(strict=False):
        raise BuildError("refusing output/input alias")

    patched = bytearray(data)
    patched[function_offset : function_offset + function_len] = (
        blob + b"\x90" * (function_len - len(blob))
    )
    patched_bytes = bytes(patched)
    _verify_outside_delta(data, patched_bytes, function_offset, function_len, blob)

    output_sha = sha256_bytes(patched_bytes)
    manifest = {
        "schema": "scourge.carplay.spp-rx-patch.v1",
        "status": "EXPERIMENTAL_BUILD_ONLY_NOT_CAR_VALIDATED",
        "car_validated": False,
        "installer_activated": False,
        "oem_binary_executed": False,
        "target": "synthetic-test-elf" if allow_test_geometry else target_name,
        "test_fixture": bool(allow_test_geometry),
        "firmware_id": locks.get("SCOURGE_FIRMWARE_ID", "UNKNOWN"),
        "input_name": input_path.name,
        "input_size": len(data),
        "input_sha256": whole_sha,
        "function_elf_vaddr": f"{function_vaddr:#x}",
        "function_file_offset": f"{function_offset:#x}",
        "function_length": function_len,
        "function_preimage_sha256": preimage_sha,
        "blob_size": len(blob),
        "blob_sha256": blob_info["blob_sha256"],
        "nop_fill_bytes": function_len - len(blob),
        "patched_region_sha256": sha256_bytes(
            patched_bytes[function_offset : function_offset + function_len]
        ),
        "output_name": output_file.name,
        "output_size": len(patched_bytes),
        "output_sha256": output_sha,
        "hook_source_sha256": blob_info["source_sha256"],
        "hook_header_sha256": blob_info["header_sha256"],
        "locks_sha256": sha256_bytes(locks_path.read_bytes()),
        "builder_sha256": sha256_bytes(Path(__file__).read_bytes()),
        "toolchain": blob_info["toolchain"],
        "compile_flags": blob_info["compile_flags"],
        "link_flags": blob_info["link_flags"],
        "linker_script_sha256": blob_info["linker_script_sha256"],
        "notes": [
            ("Synthetic test fixture; not an OEM artifact."
             if allow_test_geometry else
             "Build-only replacement of the exact VBTSppReceivedDataCfm span."),
            "Remaining bytes in the span are NOP-filled.",
            "The input was read only; it was never executed.",
            "Not activated by any installer and not car-validated.",
        ],
    }

    output_mode = stat.S_IMODE(input_stat.st_mode) & 0o777
    if output_mode == 0:
        output_mode = 0o644

    tmp_output = None
    tmp_manifest = None
    published: list[tuple[Path, int, int]] = []
    private_temps: list[tuple[Path, int | None, int | None]] = []
    anchors: list[int] = []
    try:
        fd, tmp_output = _create_private_temp(
            f".{target_name}.spp-rx.patched.", out_dir, private_temps, anchors)
        _write_private_temp(fd, patched_bytes, output_mode)

        fd, tmp_manifest = _create_private_temp(
            f".{target_name}.spp-rx.manifest.", out_dir, private_temps, anchors)
        manifest_bytes = (
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        ).encode("utf-8")
        _write_private_temp(fd, manifest_bytes, 0o644)

        # Retain the private temporary links until both files and their
        # readback succeed; they anchor the identities used for rollback.
        for temporary, destination in ((tmp_output, output_file),
                                        (tmp_manifest, manifest_file)):
            expected = next(record for record in private_temps if record[0] == temporary)
            identity = temporary.lstat()
            if ((identity.st_dev, identity.st_ino) != expected[1:] or
                    not stat.S_ISREG(identity.st_mode)):
                raise BuildError(f"private temp changed before publication: {temporary}")
            os.link(temporary, destination)
            published.append((destination, identity.st_dev, identity.st_ino))

        readback = output_file.read_bytes()
        manifest_readback = json.loads(manifest_file.read_text(encoding="utf-8"))
        if sha256_bytes(readback) != output_sha:
            raise BuildError("output SHA256 readback mismatch")
        if manifest_readback != manifest:
            raise BuildError("manifest readback mismatch")
        _verify_outside_delta(data, readback, function_offset, function_len, blob)
        cleanup_errors = _cleanup_owned_files(private_temps, "private temp")
        if cleanup_errors:
            raise BuildError("private temp cleanup incomplete: " + "; ".join(cleanup_errors))
    except (OSError, ValueError, BuildError) as exc:
        cleanup_errors = _cleanup_owned_files(published, "output")
        cleanup_errors.extend(_cleanup_owned_files(private_temps, "private temp"))
        detail = "; ".join(cleanup_errors)
        raise BuildError(f"cannot publish output: {exc}" +
                         (f"; rollback incomplete: {detail}" if detail else "")) from exc
    finally:
        # Keep the created inodes alive through every identity check, even if
        # a pathname was replaced. No blind pathname deletion occurs here.
        for anchor in anchors:
            os.close(anchor)

    return {
        "output_file": str(output_file),
        "manifest_file": str(manifest_file),
        "manifest": manifest,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Build-only experimental vbtd SPP RX hook patch (target 63/51). "
            "Never executes or activates the OEM binary."
        )
    )
    parser.add_argument("--input", required=True,
                        help="stock /app/bin/vbtd-63 or vbtd-51 to read")
    parser.add_argument("--out", required=True,
                        help="ignored output directory (new files only)")
    parser.add_argument("--target", choices=sorted(TARGETS), default="63",
                        help="pinned vbtd target (default: 63)")
    args = parser.parse_args(argv)

    try:
        result = build_patch(Path(args.input), Path(args.out),
                             target=args.target)
    except (BuildError, OSError) as exc:
        print(f"REFUSED: {exc}", file=sys.stderr)
        return 2

    manifest = result["manifest"]
    print("EXPERIMENTAL BUILD-ONLY ARTIFACT — NOT CAR-VALIDATED")
    print(f"output: {result['output_file']}")
    print(f"manifest: {result['manifest_file']}")
    print(f"input sha256:  {manifest['input_sha256']}")
    print(f"output sha256: {manifest['output_sha256']}")
    print(f"blob sha256:   {manifest['blob_sha256']} "
          f"({manifest['blob_size']} bytes)")
    print("The OEM binary was not executed and no installer integration was added.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
