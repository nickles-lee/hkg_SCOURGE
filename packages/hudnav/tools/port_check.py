#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Derive a HUD Nav firmware profile from a stock S5W_L rootfs.

Reads ONLY file bytes. No car, network, or firmware modification is used.
Standard library only; designed to be run as ``python -I``.

Usage:
    python -I port_check.py --input DIR_OR_TARBALL [--output profile.json]
    python -I port_check.py --input DIR_OR_TARBALL --report

The reference catalog below is derived from the *260319 AU* stock bytes and
from the literals currently hard-coded in src and scripts.
Every signature is a byte slice taken from that binary around a hook point.
A signature must match EXACTLY ONCE in the input binary: zero matches is
``missing``, more than one is ``ambiguous``. Both are hard stops for that hook.
See PROFILE_FORMAT.md for the output schema and signature-length rationale.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
import tarfile
import tempfile
from datetime import datetime, timezone

SCHEMA = "hudnav-firmware-profile/1"
TOOL_VERSION = "1.0.0"

# ---------------------------------------------------------------------------
# Reference catalog. All offsets are file offsets / PIE-relative vaddrs on
# 260319 AU, sha256 e663d5129177a8bd241cb875e36ec5e507d39a2c8111216acafc272bdc3fa88b.
# ---------------------------------------------------------------------------

REF_HASHES = {
    "hash.iAP2Service-2.0": (
        "iAP2Service-2.0",
        "e663d5129177a8bd241cb875e36ec5e507d39a2c8111216acafc272bdc3fa88b",
        "carplay", True,
        "src/cp_rg.c:5; src/cp_rg_send.h:8; "
        "scripts/make-car-kit.sh:58; scripts/cp-session.sh:34",
    ),
    "hash.AppProjection": (
        "AppProjection",
        "726c69ce627e9183771d11bdfac9acf9d92286a9b6c1a0d88bfbd6b5ac5a7d50",
        "android_auto", True,
        "scripts/make-car-kit.sh:56; scripts/aa-session.sh:31; "
        "docs/hudnav/AA_PROTOCOL.md:12",
    ),
    "hash.liblinux-head-unit.so": (
        "liblinux-head-unit.so",
        "59c31ae0359a8720e75afc3ee3037c3c18f15ffba71347fb46dd5d9691726865",
        "android_auto", True,
        "scripts/make-car-kit.sh:57; docs/hudnav/AA_PROTOCOL.md:22",
    ),
    "hash.libreceiver-lib.so": (
        "libreceiver-lib.so",
        "0d71cefcd2b091c00cf6bc027c71b03552151ffa652bfef6d484534b94e5c563",
        "android_auto", True,
        "docs/hudnav/PORTABILITY_INVENTORY.md:173; docs/hudnav/AA_PROTOCOL.md:13",
    ),
    "hash.AppLauncher.sh": (
        "AppLauncher.sh",
        "dc9487eb2f18218f20715881ea80011e597a29b778fc37ba754d90a8c4d4e39d",
        "both", False,  # boot-guard / kit gate, not a hook target
        "bootguard/hudnav-bootguard.sh:21; scripts/make-car-kit.sh:54",
    ),
    "hash.carplay_iAP2.service": (
        "carplay_iAP2.service",
        "f7cf915509005a4aaa18d09e3a6ccd0abdd72ef2b2816641ea9a592a1e3f4495",
        "both", False,
        "bootguard/hudnav-bootguard.sh:22; scripts/make-car-kit.sh:61",
    ),
}

# key, file, expected offset, bytes, source, required, rationale
SIGS = [
    (
        "cp.sig.P", "iAP2Service-2.0", 0x1CF4A,
        "554889e54883ec5048897dd88975d48955d048894dc84c8945c04c894db8"
        "64488b042528000000488945f831c048837dd800752e4c8d05b8950500b9de000000",
        "src/cp_rg.c:46,62-67", True,
        "64 B: the shipped fingerprint from cp_rg.c; unique on 260319 (a 12 B prefix "
        "already gives 8 hits, 16 B gives 1). Long enough to keep margin against "
        "prologue collisions in other builds; stays inside one function's prologue.",
    ),
    (
        "cp.sig.site1", "iAP2Service-2.0", 0x22633,
        "e812a9ffff48837df000740c488b45f0",
        "src/cp_rg.c:47,49,68-70", True,
        "16 B: the shipped call-site fingerprint; 5-byte call plus the distinctive "
        "null-check/store tail. Unique on 260319 even at 4 B, but a longer window "
        "guards against another identical rel32 call appearing after code drift.",
    ),
    (
        "cp.sig.site2", "iAP2Service-2.0", 0x22F60,
        "e8e59fffff48837df000740c488b45f0",
        "src/cp_rg.c:48,49,71-73", True,
        "16 B: same shape as site1 (CPRG_SITE_BYTES), keeps the paired site/param "
        "semantics; unique on 260319 and in the shipped patcher.",
    ),
    (
        "cp.sig.site3", "iAP2Service-2.0", 0x1367C,
        "e879040000e988000000807dd002752e",
        "src/cp_rg.c:50-51,74-76", True,
        "16 B: the shipped dispatcher-call fingerprint; includes the following jmp "
        "and byte-compare so the rel32 target check in cp_rg.c:335-340 stays meaningful.",
    ),
    (
        "cp.sig.dispatcher", "iAP2Service-2.0", 0x13AFA,
        "554889e5534883ec6848897db8488975",
        "src/cp_rg.c:50; docs/hudnav/CP_PROTOCOL.md:8", True,
        "16 B: minimal unique length is 13 B; 16 B adds margin and avoids embedding "
        "the rel32s found deeper in handleReceivedData.",
    ),
    (
        "cp.sig.encoder", "iAP2Service-2.0", 0x1D0A0,
        "554889e54883ec40897dec8975e84889",
        "src/cp_rg.c:52,359; src/cp_rg_send.h:9", True,
        "16 B: minimal unique length is 11 B; 16 B covers the argument stores and "
        "is the shortest stable prologue slice with comfortable margin.",
    ),
    (
        "cp.sig.packet_ctor", "iAP2Service-2.0", 0x1ABD6,
        "554889e54883ec4048897dd8488975d08955cc64488b042528000000488945f8"
        "31c0488b45d848c7",
        "src/cp_rg.c:53,360; src/cp_rg_send.h:10", True,
        "40 B: minimal unique length is 36 B (a generic prologue repeats 46 times at "
        "16 B); 40 B reaches past the canary store into function-specific code.",
    ),
    (
        "cp.sig.send", "iAP2Service-2.0", 0x152DE,
        "554889e54154534883ec4048897dc8488975c08955bc894db864488b042528000000"
        "488945e831c08b45bc4189c14c8d",
        "src/cp_rg.c:54,361; src/cp_rg_send.h:11", True,
        "48 B: minimal unique length is 43 B (two matches at 16-40 B); 48 B includes "
        "the callee-saved registers, canary store and first argument use.",
    ),
    (
        "cp.sig.packet_dtor", "iAP2Service-2.0", 0x1AD46,
        "554889e54883ec2048897de864488b042528000000488945f831c0"
        "488b45e8488b004885c0752b4c",
        "src/cp_rg.c:55,362; src/cp_rg_send.h:12", True,
        "40 B: minimal unique length is 39 B (252 matches at 16 B); 40 B clears the "
        "generic canary prologue and reaches the destructor's first load.",
    ),
    (
        "cp.sig.stock_sender", "iAP2Service-2.0", 0x204E6,
        "554889e5534881ecb80000004889bd58ffffff",
        "docs/hudnav/CP_PROTOCOL.md:29; src/cp_rg_send.h:14", False,
        "16 B: minimal unique length; evidence-only anchor for the 0x5200 send recipe, "
        "not used by shipped code.",
    ),
    (
        "cp.sig.new_array", "iAP2Service-2.0", 0x1CFEE,
        "e8ed76feff488945f048837df000752b",
        "src/cp_rg.c:20; docs/hudnav/CP_PROTOCOL.md:20", False,
        "16 B: already unique at 8 B on 260319; 16 B keeps the null-check tail. "
        "Contains one rel32 call, so a miss here is informational, not a gate.",
    ),
    (
        "cp.sig.memcpy", "iAP2Service-2.0", 0x1D03D,
        "e83e7bfeff488d55d0488b45c84889d6",
        "src/cp_rg.c:20; docs/hudnav/CP_PROTOCOL.md:21", False,
        "16 B: already unique at 8 B on 260319; same rel32 caveat as new_array.",
    ),
]

# name, expected PLT offset, source, note
PLT_ITEMS = [
    ("_ZdaPv", 0x4EF0, "src/cp_rg.c:56,363; src/cp_rg_send.h:13",
     "operator delete[]: resolve the dynamic symbol, never a fixed PLT slot at runtime."),
    ("read", 0x4FE0, "src/cp_shim.c:18-19; src/cp_hook.c:10",
     "inbound iAP2 seam; interpose read@GLIBC_2.2.5 by symbol, not by address."),
    ("write", 0x4840, "src/cp_shim.c:24; src/cp_hook.c:11",
     "outbound Identify / 0x5200 submit seam."),
]

# symbol, library, expected st_value, source, note
AA_SYMBOLS = [
    ("_ZN21CAndroidAutoInterface37setInstanceINavigationStatusServiceCBEP26INavigationStatusServiceCB",
     "liblinux-head-unit.so", 0x69520,
     "src/aa_shim.c:314-315; src/aa_shim.c:334-337; docs/hudnav/AA_PROTOCOL.md:26",
     "nav setter; aa_shim selftest prints object+0x69520."),
    ("_ZN21CAndroidAutoInterface32setInstanceIPhoneStatusServiceCBEP21IPhoneStatusServiceCB",
     "liblinux-head-unit.so", 0x69710,
     "src/aa_shim.c:305-308; src/aa_shim.c:334-337",
     "last stock setter; aa_shim selftest prints object+0x69710."),
    ("_ZN24NavigationStatusCallback24navigationStatusCallbackEi",
     "liblinux-head-unit.so", 0x58C30,
     "src/aa_shim.c:196-199; docs/hudnav/PORTABILITY_INVENTORY.md:96 (A4)",
     "status callback; pinned selftest offset 0x58c30."),
    ("_ZN24NavigationStatusCallback34navigationNextTurnDistanceCallbackEiii45NavigationNextTurnDistanceEvent_DistanceUnits",
     "liblinux-head-unit.so", 0x58E20,
     "src/aa_shim.c:209-213; docs/hudnav/AA_PROTOCOL.md:19-24",
     "distance callback."),
    ("_ZN24NavigationStatusCallback26navigationNextTurnCallbackERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEiiS7_ii",
     "liblinux-head-unit.so", 0x59080,
     "src/aa_shim.c:222-226; docs/hudnav/AA_PROTOCOL.md:19-24",
     "next-turn callback."),
    ("_ZN20ProtocolEndpointBase15onChannelOpenedEh",
     "libreceiver-lib.so", 0xD9760,
     "src/aa_shim.c:321-323; src/aa_shim.c:334-337",
     "channel-open interposer; selftest offset 0xd9760."),
    ("_ZN24NavigationStatusEndpoint5startEv",
     "libreceiver-lib.so", 0xE5780,
     "src/aa_shim.c:328-330; src/aa_shim.c:334-337",
     "start() sender; selftest offset 0xe5780."),
    ("_ZTV24NavigationStatusEndpoint",
     "libreceiver-lib.so", 0x40AA70,
     "src/aa_shim.c:326-327; src/aa_shim.c:334-337",
     "vtable; selftest offset 0x40aa70. liblinux-head-unit.so also has an undefined "
     "reference with st_value=0 - only defined symbols count."),
]

AA_SYMBOL_KEYS = {
    "_ZN21CAndroidAutoInterface37setInstanceINavigationStatusServiceCBEP26INavigationStatusServiceCB": "aa.sym.nav_setter",
    "_ZN21CAndroidAutoInterface32setInstanceIPhoneStatusServiceCBEP21IPhoneStatusServiceCB": "aa.sym.phone_setter",
    "_ZN24NavigationStatusCallback24navigationStatusCallbackEi": "aa.sym.status_cb",
    "_ZN24NavigationStatusCallback34navigationNextTurnDistanceCallbackEiii45NavigationNextTurnDistanceEvent_DistanceUnits": "aa.sym.distance_cb",
    "_ZN24NavigationStatusCallback26navigationNextTurnCallbackERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEiiS7_ii": "aa.sym.turn_cb",
    "_ZN20ProtocolEndpointBase15onChannelOpenedEh": "aa.sym.onChannelOpened",
    "_ZN24NavigationStatusEndpoint5startEv": "aa.sym.nav_start",
    "_ZTV24NavigationStatusEndpoint": "aa.sym.nav_vtable",
}

# path bytes, file, expected first offset, require unique, source, side, required
STRINGS = [
    (b"/dev/ttyIAP20", "iAP2Service-2.0", 0x73375, True,
     "src/cp_shim.c:21; docs/hudnav/PORTABILITY_INVENTORY.md:84 (C6)", "carplay", True),
    (b"/dev/mfd-iap2", "iAP2Service-2.0", 0x721F4, False,
     "src/cp_shim.c:35-36; docs/hudnav/PORTABILITY_INVENTORY.md:145 (P4)", "carplay", False),
    (b"/dev/i2c-1", "iAP2Service-2.0", 0x75BDF, False,
     "src/cp_shim.c:36; docs/hudnav/PORTABILITY_INVENTORY.md:145 (P4)", "carplay", False),
]

RECEIVER_TAG = "260312102711"  # docs/hudnav/AA_PROTOCOL.md:121-122 (A7)

# ---------------------------------------------------------------------------
# Minimal ELF64 reader (section headers, .dynsym, relocations, PLT stubs)
# ---------------------------------------------------------------------------

class Elf64Error(Exception):
    pass


class Elf64:
    """Read-only ELF64 parser; never executes or maps target code."""

    SHT_DYNSYM = 11

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as fh:
            self.data = fh.read()
        self.sections = []
        self.error = None
        try:
            self._parse()
        except Exception as exc:  # a foreign/corrupt ELF must not abort the tool
            self.error = "%s: %s" % (type(exc).__name__, exc)
            self.sections = []

    def _parse(self):
        d = self.data
        if len(d) < 64 or d[:4] != b"\x7fELF" or d[4] != 2:
            raise Elf64Error("not an ELF64 file")
        self.e_shoff, = struct.unpack_from("<Q", d, 0x28)
        self.e_shentsize, self.e_shnum, self.e_shstrndx = struct.unpack_from("<HHH", d, 0x3A)
        if not self.e_shoff:
            raise Elf64Error("no section header table")
        if self.e_shentsize < 64:
            raise Elf64Error("bad e_shentsize")
        if self.e_shnum == 0:  # extended numbering: sh_size of section 0
            self.e_shnum, = struct.unpack_from("<Q", d, self.e_shoff + 0x20)
        if self.e_shstrndx == 0xFFFF:  # SHN_XINDEX: sh_link of section 0
            self.e_shstrndx, = struct.unpack_from("<I", d, self.e_shoff + 0x28)
        if not 0 <= self.e_shstrndx < self.e_shnum or self.e_shnum > 100000:
            raise Elf64Error("bad e_shstrndx/e_shnum")
        for i in range(self.e_shnum):
            o = self.e_shoff + i * self.e_shentsize
            if o + 64 > len(d):
                raise Elf64Error("truncated section header %d" % i)
            (name, typ, flags, addr, off, size, link, info,
             align, entsize) = struct.unpack_from("<IIQQQQIIQQ", d, o)
            self.sections.append(dict(index=i, name=name, typ=typ, addr=addr,
                                      off=off, size=size, link=link,
                                      entsize=entsize))
        strtab = self._slice(self.sections[self.e_shstrndx])
        for s in self.sections:
            end = strtab.find(b"\0", s["name"])
            s["sname"] = strtab[s["name"]:end].decode("ascii", "replace") if end >= 0 else ""

    def _slice(self, sec):
        off, size = sec["off"], sec["size"]
        if off > len(self.data) or size > len(self.data) - off:
            raise Elf64Error("section %s out of file range" % sec.get("sname", sec["index"]))
        return self.data[off:off + size]

    def section(self, name):
        for s in self.sections:
            if s.get("sname") == name:
                return s
        return None

    def dynsym(self):
        sec = next((s for s in self.sections if s["typ"] == self.SHT_DYNSYM), None)
        if sec is None or sec["link"] >= len(self.sections):
            return []
        strings = self._slice(self.sections[sec["link"]])
        step = sec["entsize"] or 24
        out = []
        off = sec["off"]
        for pos in range(off, off + sec["size"], step):
            if pos + 24 > len(self.data):
                break
            name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", self.data, pos)
            end = strings.find(b"\0", name)
            symbol = strings[name:end].decode("ascii", "replace") if end >= 0 else ""
            out.append(dict(name=symbol, value=value, size=size, shndx=shndx,
                            info=info, defined=(shndx != 0 and value != 0)))
        return out

    def relocations(self):
        """Yield (got_offset, symbol_name) for the JUMP_SLOT relocations."""
        for sname, wide in ((".rela.plt", True), (".rel.plt", False)):
            sec = self.section(sname)
            if sec is None:
                continue
            step = sec["entsize"] or (24 if wide else 16)
            syms = self.dynsym()
            for pos in range(sec["off"], sec["off"] + sec["size"], step):
                need = 24 if wide else 16
                if pos + need > len(self.data):
                    break
                r_offset, r_info = struct.unpack_from("<QQ", self.data, pos)
                idx = r_info >> 32
                name = syms[idx]["name"] if idx < len(syms) else ""
                yield r_offset, name

    def plt(self):
        """Map GOT address -> PLT stub address for x86-64 jump stubs."""
        sec = self.section(".plt") or self.section(".plt.sec")
        if sec is None:
            return {}
        step = sec["entsize"] or 16
        out = {}
        for pos in range(sec["off"], sec["off"] + sec["size"], step):
            if pos + 6 > len(self.data) or self.data[pos:pos + 2] != b"\xff\x25":
                continue
            disp, = struct.unpack_from("<i", self.data, pos + 2)
            addr = sec["addr"] + (pos - sec["off"])
            out[addr + 6 + disp] = addr
        return out

    def plt_for(self, symbol):
        got = next((g for g, name in self.relocations() if name == symbol), None)
        if got is None:
            return None
        return self.plt().get(got)

    def got_for(self, symbol):
        return next((g for g, name in self.relocations() if name == symbol), None)


# ---------------------------------------------------------------------------
# Input handling
# ---------------------------------------------------------------------------

WANTED_FILES = {_spec[0] for _spec in REF_HASHES.values()}
for _spec in SIGS:
    WANTED_FILES.add(_spec[1])
for _name, _lib, _off, _src, _note in AA_SYMBOLS:
    WANTED_FILES.add(_lib)
WANTED_FILES.add("iAP2Service-2.0")


class FirmwareFiles:
    """Resolve a logical file name inside a rootfs directory."""

    def __init__(self, root):
        self.root = root
        self._index = None

    def _build_index(self):
        index = {}
        for dirpath, dirnames, filenames in os.walk(self.root):
            dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
            for filename in filenames:
                index.setdefault(filename, []).append(os.path.join(dirpath, filename))
        for paths in index.values():
            paths.sort(key=lambda p: (len(p), p))
        self._index = index

    def get(self, name):
        direct = os.path.join(self.root, name)
        if os.path.isfile(direct):
            return direct
        if self._index is None:
            self._build_index()
        candidates = self._index.get(name, [])
        return candidates[0] if candidates else None


def prepare_input(path):
    """Return (directory, cleanup_callable). Accepts a dir or a tar/tar.gz."""
    if os.path.isdir(path):
        return path, lambda: None
    if not os.path.isfile(path):
        raise ValueError("input does not exist: %s" % path)
    try:
        is_tar = tarfile.is_tarfile(path)
    except OSError:
        is_tar = False
    if not is_tar:
        raise ValueError("input must be a directory or a tar archive: %s" % path)
    tmp = tempfile.mkdtemp(prefix="hudnav-port-check-")
    wanted = set(WANTED_FILES)
    with tarfile.open(path, "r:*") as archive:
        members = [m for m in archive.getmembers()
                   if m.isfile() and (os.path.basename(m.name) in wanted
                                      or m.name.endswith(".ver"))]
        try:
            archive.extractall(tmp, members=members, filter="data")
        except TypeError:  # Python < 3.12 has no extraction filters
            archive.extractall(tmp, members=members)
    return tmp, lambda: _rmtree(tmp)


def _rmtree(path):
    import shutil
    shutil.rmtree(path, ignore_errors=True)


def detect_firmware(root, override=None):
    """Parse the S5W_L identity from a .ver header or an explicit override."""
    identity = override
    brand = region = platform = ""
    source = "--firmware-id"
    if not identity:
        for dirpath, _dirs, files in os.walk(root):
            for name in sorted(files):
                if not name.endswith(".ver"):
                    continue
                try:
                    with open(os.path.join(dirpath, name), "r",
                              encoding="utf-8", errors="replace") as fh:
                        first = fh.readline()
                except OSError:
                    continue
                match = re.search(r"[|]([A-Za-z0-9_]+\.[A-Z]+\.S5W_L\.[\d.]+\d{6})[|]", first)
                if match:
                    identity = match.group(1)
                    fields = first.strip().lstrip("+").split("|")
                    if len(fields) > 4:
                        region, platform, brand = fields[2], fields[3], fields[4]
                    source = os.path.join(dirpath, name)
                    break
            if identity:
                break
    version = ""
    if identity:
        parts = identity.split(".")
        version = parts[-1] if parts else ""
        region = region or (parts[1] if len(parts) > 1 else "")
        platform = platform or (parts[2] if len(parts) > 2 else "")
    if not identity:
        candidate = os.path.basename(os.path.normpath(root))
        match = re.search(r"(\d{6})", candidate)
        version = match.group(1) if match else ""
        identity = "unknown" + (("/" + version) if version else "")
        source = "directory name (no .ver found; use --firmware-id)"
    return dict(id=identity, version=version, brand=brand, region=region,
                platform=platform, detected_from=source)


# ---------------------------------------------------------------------------
# Profile construction
# ---------------------------------------------------------------------------

def _item(items, key, category, status, expected, value, source, note,
          side, required, reference_match=None, **extra):
    row = dict(category=category, status=status, expected=expected, value=value,
               source=source, note=note, side=side, required=required)
    if reference_match is not None:
        row["reference_match"] = bool(reference_match)
    row.update(extra)
    items[key] = row


def _scan(data, signature):
    """Return every file offset at which signature occurs."""
    hits = []
    pos = data.find(signature)
    while pos >= 0:
        hits.append(pos)
        pos = data.find(signature, pos + 1)
    return hits


def analyze(root, firmware_id=None):
    files = FirmwareFiles(root)
    items = {}
    cache = {}

    def blob(name):
        if name not in cache:
            path = files.get(name)
            cache[name] = open(path, "rb").read() if path else None
        return cache[name]

    def elf(name):
        key = "elf:" + name
        if key not in cache:
            path = files.get(name)
            cache[key] = Elf64(path) if path else None
        return cache[key]

    # --- hashes -------------------------------------------------------------
    for key, (name, expected_hash, side, required, source) in REF_HASHES.items():
        data = blob(name)
        if data is None:
            _item(items, key, "hash", "missing", expected_hash, None, source,
                  "stock file not present in input", side, required)
        else:
            digest = hashlib.sha256(data).hexdigest()
            _item(items, key, "hash", "found", expected_hash, digest, source,
                  "sha256 of the supplied stock file", side, required,
                  reference_match=(digest == expected_hash), bytes=len(data))

    # --- CarPlay signatures -------------------------------------------------
    for key, name, expected, hexsig, source, required, rationale in SIGS:
        data = blob(name)
        sig = bytes.fromhex(hexsig)
        if data is None:
            _item(items, key, "signature", "missing", expected, None, source,
                  "host binary not present", "carplay", required,
                  signature_hex=hexsig, signature_length=len(sig), match_count=0,
                  rationale=rationale)
            continue
        hits = _scan(data, sig)
        if not hits:
            status = "missing"
        elif len(hits) > 1:
            status = "ambiguous"
        else:
            status = "found"
        _item(items, key, "signature", status, expected,
              hits[0] if len(hits) == 1 else (hits if hits else None), source,
              "unique byte-signature match required", "carplay", required,
              reference_match=(status == "found" and hits[0] == expected),
              signature_hex=hexsig, signature_length=len(sig),
              match_count=len(hits), rationale=rationale)

    # --- PLT / GOT derived from .dynsym + relocations -----------------------
    cp_elf = elf("iAP2Service-2.0")
    for symbol, expected_plt, source, note in PLT_ITEMS:
        got = cp_elf.got_for(symbol) if cp_elf else None
        plt = cp_elf.plt_for(symbol) if cp_elf else None
        status = "found" if plt is not None else "missing"
        _item(items, "cp.plt." + symbol, "dynsym", status, expected_plt, plt, source,
              note, "carplay", True, reference_match=(plt == expected_plt),
              dynamic_symbol=symbol, got_offset=got,
              parse_error=(cp_elf.error if cp_elf else "binary not present"))
        _item(items, "cp.got." + symbol, "dynsym", "found" if got is not None else "missing",
              {"_ZdaPv": 0x299D58, "read": 0x299DD0, "write": 0x299A00}[symbol], got,
              source, "JUMP_SLOT relocation for %s" % symbol, "carplay", True,
              reference_match=(got == {"_ZdaPv": 0x299D58, "read": 0x299DD0,
                                       "write": 0x299A00}[symbol]),
              dynamic_symbol=symbol)

    da = cp_elf.dynsym() if cp_elf else []
    found_da = [s for s in da if s["name"] == "_ZdaPv"]
    _item(items, "cp.dynsym._ZdaPv", "dynsym", "found" if found_da else "missing",
          "present (imported)", "present" if found_da else None,
          "src/cp_rg.c:56,363; docs/hudnav/PORTABILITY_INVENTORY.md:66 (C4)",
          "operator delete[] must resolve by dynamic symbol, not by fixed PLT",
          "carplay", True, reference_match=bool(found_da),
          st_value=(found_da[0]["value"] if found_da else None),
          st_shndx=(found_da[0]["shndx"] if found_da else None))

    # --- AA exported symbols ------------------------------------------------
    for symbol, library, expected, source, note in AA_SYMBOLS:
        obj = elf(library)
        syms = [s for s in (obj.dynsym() if obj else []) if s["name"] == symbol and s["defined"]]
        key = AA_SYMBOL_KEYS[symbol]
        if not syms:
            status = "missing"
            value = None
        elif len(syms) > 1:
            status = "ambiguous"
            value = [s["value"] for s in syms]
        else:
            status = "found"
            value = syms[0]["value"]
        _item(items, key, "dynsym", status, expected, value, source, note,
              "android_auto", True,
              reference_match=(status == "found" and value == expected),
              dynamic_symbol=symbol, library=library, match_count=len(syms),
              parse_error=(obj.error if obj else "library not present"))

    # --- device strings -----------------------------------------------------
    for needle, filename, expected, unique, source, side, required in STRINGS:
        data = blob(filename)
        hits = _scan(data, needle) if data is not None else []
        if not hits:
            status = "missing"
        elif unique and len(hits) > 1:
            status = "ambiguous"
        else:
            status = "found"
        _item(items, "string." + needle.decode("ascii").lstrip("/").replace("/", "_"),
              "string", status, expected, hits[0] if hits else None, source,
              "device path literal in %s" % filename, side, required,
              reference_match=(bool(hits) and hits[0] == expected),
              string=needle.decode("ascii"), match_count=len(hits),
              require_unique=unique)

    # --- receiver build tag / protobuf feature probe ------------------------
    tag_data = blob("libreceiver-lib.so")
    tag_hits = _scan(tag_data, RECEIVER_TAG.encode("ascii")) if tag_data is not None else []
    _item(items, "aa.build_tag", "build_tag", "found" if tag_hits else "missing",
          RECEIVER_TAG, tag_hits[0] if tag_hits else None,
          "docs/hudnav/AA_PROTOCOL.md:121-122; docs/hudnav/PORTABILITY_INVENTORY.md:99 (A7)",
          "projectionprotocol receiver-lib 1.5 (0.6.9-r3) build tag",
          "android_auto", True, reference_match=bool(tag_hits),
          match_count=len(tag_hits))
    newer = _scan(tag_data, b"NavigationNotification") if tag_data is not None else []
    _item(items, "aa.probe.NavigationNotification", "feature_probe",
          "found" if newer else "missing", "absent on 260319",
          "present" if newer else "absent",
          "docs/hudnav/AA_PROTOCOL.md:18,52; inventory.md:99 (A7)",
          "absence is expected: the legacy receiver has no lanes/destination distance",
          "android_auto", False, reference_match=(not newer), match_count=len(newer))

    firmware = detect_firmware(root, firmware_id)
    verdict = make_verdict(items)
    return dict(schema=SCHEMA, tool_version=TOOL_VERSION,
                # JSON has no comment syntax, so SPDX metadata is carried as
                # top-level fields rather than a C-style SPDX header.
                **{"SPDX-FileCopyrightText": "2026 The SCOURGE Project contributors",
                   "SPDX-License-Identifier": "AGPL-3.0-only"},
                generated_utc=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                input=os.path.abspath(root), firmware=firmware, verdict=verdict,
                summary=summarize(items), items=items)


def make_verdict(items):
    def state(side):
        rows = [(k, v) for k, v in items.items()
                if v["required"] and v["side"] in (side, "both")]
        if not rows:
            return "not-ready"
        if all(v["status"] == "found" and v.get("reference_match") for _k, v in rows):
            return "ready"
        if all(v["status"] == "found" for _k, v in rows):
            return "needs-review"
        return "not-ready"

    cp, aa = state("carplay"), state("android_auto")
    if cp == "ready" and aa == "ready":
        overall = "CarPlay ready + AA ready"
    elif cp == "ready":
        overall = "CarPlay ready"
    elif aa == "ready":
        overall = "AA ready"
    elif "needs-review" in (cp, aa):
        overall = "unsupported (unique matches but build differs; RE review required)"
    else:
        overall = "unsupported"
    return dict(carplay=cp, android_auto=aa, overall=overall)


def summarize(items):
    out = dict(found=0, missing=0, ambiguous=0, different=0, expected=0, total=len(items))
    for row in items.values():
        out[row["status"]] += 1
        if row["status"] == "found" and row.get("reference_match") is False:
            out["different"] += 1
        if row["status"] != "found" and row.get("reference_match") is True:
            out["expected"] += 1
    out["missing"] -= out["expected"]
    return out


# ---------------------------------------------------------------------------
# Human-readable report (Discord-pasteable, no markdown tables)
# ---------------------------------------------------------------------------

def _fmt(value, row):
    if row["category"] == "hash" and isinstance(value, str):
        return value[:8] + "..." + value[-6:]
    if isinstance(value, int):
        return hex(value)
    return str(value)


def render_report(profile):
    fw = profile["firmware"]
    lines = [
        "HUD NAV PORT CHECK v%s" % profile["tool_version"],
        "Firmware: %s (version %s, brand %s, region %s, platform %s)" % (
            fw["id"], fw["version"] or "?", fw["brand"] or "?",
            fw["region"] or "?", fw["platform"] or "?"),
        "Input:    %s" % profile["input"],
        "Verdict:  %s  (CarPlay: %s / AA: %s)" % (
            profile["verdict"]["overall"], profile["verdict"]["carplay"],
            profile["verdict"]["android_auto"]),
        "",
    ]
    for side, heading in (("carplay", "CarPlay / iAP2Service-2.0"),
                          ("android_auto", "Android Auto"),
                          ("both", "Shared gates")):
        lines.append(heading)
        rows = [(k, v) for k, v in profile["items"].items() if v["side"] == side]
        for key, row in rows:
            tag = {"found": "OK", "missing": "MISSING", "ambiguous": "AMBIGUOUS"}[row["status"]]
            if row["status"] == "found" and row.get("reference_match") is False:
                tag = "DIFF"
            elif row["status"] != "found" and row.get("reference_match") is True:
                tag = "EXPECTED"  # e.g. NavigationNotification is absent on 260319 by design
            detail = _fmt(row["value"], row)
            if row.get("match_count", 1) > 1:
                detail += " (%d matches)" % row["match_count"]
            lines.append("  %-9s %-46s %s" % (tag, key, detail))
        lines.append("")
    s = profile["summary"]
    lines.append("Totals: %d OK / %d MISSING / %d AMBIGUOUS / %d DIFFERENT / %d EXPECTED-ABSENT (of %d)" % (
        s["found"], s["missing"], s["ambiguous"], s["different"], s["expected"], s["total"]))
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--input", required=True,
                        help="directory with stock files, or the stock tar/tar.gz")
    parser.add_argument("--output", help="write the JSON profile to this path")
    parser.add_argument("--report", action="store_true",
                        help="print a Discord-pasteable PASS/FAIL matrix")
    parser.add_argument("--firmware-id", help="override firmware identity (e.g. NE_E.AUS.S5W_L.001.001.260319)")
    args = parser.parse_args(argv)

    try:
        path, cleanup = prepare_input(args.input)
    except (ValueError, OSError, tarfile.TarError) as exc:
        print("port_check: %s" % exc, file=sys.stderr)
        return 2
    try:
        profile = analyze(path, args.firmware_id)
        profile["input"] = os.path.abspath(args.input)  # keep the caller's path, not the temp dir
    finally:
        cleanup()
    payload = json.dumps(profile, indent=2, sort_keys=True) + "\n"
    if args.output:
        with open(args.output, "w", encoding="utf-8") as fh:
            fh.write(payload)
    if args.report:
        print(render_report(profile))
        if args.output:
            print("\nProfile written to %s" % args.output)
    else:
        sys.stdout.write(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
