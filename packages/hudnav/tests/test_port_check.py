#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Acceptance test for tools/port_check.py against the 260319 AU stock files.

The tool must reproduce EXACTLY the offsets/hashes/symbols previously hard-coded
in src and scripts. Each expectation below cites the source
location it came from. The checked-in profile is compared with a fresh scan of
the stock files, then tools/profile_to_header.py generates the C header that
cp_rg.c/aa_shim.c include; its offsets/fingerprints/mangled names are checked
against the scan, so any drift between the tool's catalog, the checked-in
profile and what the shipped patcher compiles fails this test.

Stock OEM binaries are NEVER copied into a git-tracked directory. They are read
from HUDNAV_FIRMWARE_DIR, HUDNAV_STOCK_TARBALL, or the known
work/stock-plain.tar.gz path, extracted into a temp dir, and the
test SKIPs cleanly if none is present.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]          # .../prototype
TOOL_DIR = ROOT / "tools"
TOOL = TOOL_DIR / "port_check.py"
HEADER_TOOL = TOOL_DIR / "profile_to_header.py"
CHECKED_PROFILE = ROOT / "profiles" / "NE_E.AUS.S5W_L.001.001.260319.json"
PROFILE_ID = "NE_E.AUS.S5W_L.001.001.260319"
SRC = ROOT / "src"
sys.path.insert(0, str(TOOL_DIR))
import port_check  # noqa: E402  (path fixed up above; works under python -I)

STOCK_FILES = (
    "iAP2Service-2.0", "AppProjection", "liblinux-head-unit.so",
    "libreceiver-lib.so", "AppLauncher.sh", "carplay_iAP2.service",
)

# item key -> (source location, expected value)
# "expected value" is an int file offset, a sha256 hex string, or a symbol name.
EXPECTED = {
    # Hash gates (C12, G1, G2, G3, A8)
    "hash.iAP2Service-2.0": (
        "src/cp_rg.c:5; src/cp_rg_send.h:8; scripts/make-car-kit.sh:58; scripts/cp-session.sh:34",
        "e663d5129177a8bd241cb875e36ec5e507d39a2c8111216acafc272bdc3fa88b"),
    "hash.AppProjection": (
        "scripts/make-car-kit.sh:56; scripts/aa-session.sh:31",
        "726c69ce627e9183771d11bdfac9acf9d92286a9b6c1a0d88bfbd6b5ac5a7d50"),
    "hash.liblinux-head-unit.so": (
        "scripts/make-car-kit.sh:57; docs/hudnav/AA_PROTOCOL.md:22",
        "59c31ae0359a8720e75afc3ee3037c3c18f15ffba71347fb46dd5d9691726865"),
    "hash.libreceiver-lib.so": (
        "docs/hudnav/PORTABILITY_INVENTORY.md:173; inventory.md A8",
        "0d71cefcd2b091c00cf6bc027c71b03552151ffa652bfef6d484534b94e5c563"),
    "hash.AppLauncher.sh": (
        "bootguard/hudnav-bootguard.sh:21; recovery/ssh_revert.sh:11",
        "dc9487eb2f18218f20715881ea80011e597a29b778fc37ba754d90a8c4d4e39d"),
    "hash.carplay_iAP2.service": (
        "bootguard/hudnav-bootguard.sh:22; recovery/ssh_revert.sh:12",
        "f7cf915509005a4aaa18d09e3a6ccd0abdd72ef2b2816641ea9a592a1e3f4495"),
    # CarPlay patch sites (C1-C4, C5 evidence)
    "cp.sig.P": ("src/cp_rg.c:46,62-67", 0x1CF4A),
    "cp.sig.site1": ("src/cp_rg.c:47,68-70", 0x22633),
    "cp.sig.site2": ("src/cp_rg.c:48,71-73", 0x22F60),
    "cp.sig.site3": ("src/cp_rg.c:50-51,74-76", 0x1367C),
    "cp.sig.dispatcher": ("src/cp_rg.c:50; T21_cp_receive_path.md:8", 0x13AFA),
    "cp.sig.encoder": ("src/cp_rg.c:52,359; src/cp_rg_send.h:9", 0x1D0A0),
    "cp.sig.packet_ctor": ("src/cp_rg.c:53,360; src/cp_rg_send.h:10", 0x1ABD6),
    "cp.sig.send": ("src/cp_rg.c:54,361; src/cp_rg_send.h:11", 0x152DE),
    "cp.sig.packet_dtor": ("src/cp_rg.c:55,362; src/cp_rg_send.h:12", 0x1AD46),
    "cp.sig.stock_sender": ("src/cp_rg_send.h:14; CPA_send_notes.md:29", 0x204E6),
    "cp.sig.new_array": ("src/cp_rg.c:20; CP_DESIGN.md:20", 0x1CFEE),
    "cp.sig.memcpy": ("src/cp_rg.c:20; CP_DESIGN.md:21", 0x1D03D),
    # PLT/GOT seams (C4, C6, C7)
    "cp.plt._ZdaPv": ("src/cp_rg.c:56,363; src/cp_rg_send.h:13", 0x4EF0),
    "cp.plt.read": ("src/cp_shim.c:18-19; src/cp_hook.c:10", 0x4FE0),
    "cp.plt.write": ("src/cp_shim.c:24; src/cp_hook.c:11", 0x4840),
    "cp.got.read": ("src/cp_shim.c:18; inventory.md C6", 0x299DD0),
    "cp.got.write": ("src/cp_shim.c:24; inventory.md C7", 0x299A00),
    "cp.got._ZdaPv": ("inventory.md C4 (derived from .rela.plt)", 0x299D58),
    # AA selftest offsets / exported mangled symbols (A1-A4, A7)
    "aa.sym.nav_setter": ("src/aa_shim.c:314-315,334-337; AA_NAV_CAR_SESSION.md:26", 0x69520),
    "aa.sym.phone_setter": ("src/aa_shim.c:305-308,334-337", 0x69710),
    "aa.sym.status_cb": ("src/aa_shim.c:196-199; inventory.md A4", 0x58C30),
    "aa.sym.distance_cb": ("src/aa_shim.c:209-213; projection_hookpoints.md:19-24", 0x58E20),
    "aa.sym.turn_cb": ("src/aa_shim.c:222-226; projection_hookpoints.md:19-24", 0x59080),
    "aa.sym.onChannelOpened": ("src/aa_shim.c:321-323,334-337", 0xD9760),
    "aa.sym.nav_start": ("src/aa_shim.c:328-330,334-337", 0xE5780),
    "aa.sym.nav_vtable": ("src/aa_shim.c:326-327,334-337", 0x40AA70),
    # Device path / build tag (C6, P4, A7)
    "string.dev_ttyIAP20": ("src/cp_shim.c:21; inventory.md:84", 0x73375),
    "aa.build_tag": ("docs/hudnav/AA_PROTOCOL.md:121-122; inventory.md A7", "260312102711"),
}


def generate_header(profile_path, output_path):
    """Build-time generator: profile JSON -> fw_profile.h."""
    proc = subprocess.run(
        [sys.executable, "-I", str(HEADER_TOOL), "--profile", str(profile_path),
         "--output", str(output_path)],
        capture_output=True, text=True, timeout=60)
    assert proc.returncode == 0, "profile_to_header failed: %s" % proc.stderr
    return output_path.read_text(encoding="utf-8")


def header_defines(text):
    """All simple #define NAME VALUE lines in the generated header."""
    return {m.group(1): m.group(2).strip().rstrip("\\").strip()
            for m in re.finditer(r"^#define[ \t]+(\w+)[ \t]+(.+)$", text, re.M)
            if not m.group(2).strip().startswith("{")}


def header_initializers(text):
    """CPRG_*_FP_INIT byte arrays emitted by profile_to_header.py."""
    out = {}
    pattern = re.compile(r"^#define[ \t]+(\w+)_FP_INIT[ \t]+\{.*?^\}", re.M | re.S)
    for match in pattern.finditer(text):
        out[match.group(1) + "_FP"] = "".join(
            re.findall(r"0x([0-9a-fA-F]{2})", match.group(0))).lower()
    return out


HEADER_OFFSETS = {
    "CPRG_P_OFF": "cp.sig.P",
    "CPRG_SITE1_OFF": "cp.sig.site1",
    "CPRG_SITE2_OFF": "cp.sig.site2",
    "CPRG_SITE3_OFF": "cp.sig.site3",
    "CPRG_DISP_OFF": "cp.sig.dispatcher",
    "CPRG_ENC_OFF": "cp.sig.encoder",
    "CPRG_PKTC_OFF": "cp.sig.packet_ctor",
    "CPRG_SEND_OFF": "cp.sig.send",
    "CPRG_PKTD_OFF": "cp.sig.packet_dtor",
    "CPRG_DELARR_OFF": "cp.plt._ZdaPv",
}
HEADER_FPS = {
    "CPRG_P_FP": "cp.sig.P",
    "CPRG_SITE1_FP": "cp.sig.site1",
    "CPRG_SITE2_FP": "cp.sig.site2",
    "CPRG_SITE3_FP": "cp.sig.site3",
}
HEADER_SYMS = {
    "HUDNAV_AA_SYM_STATUS_CB": "aa.sym.status_cb",
    "HUDNAV_AA_SYM_DISTANCE_CB": "aa.sym.distance_cb",
    "HUDNAV_AA_SYM_TURN_CB": "aa.sym.turn_cb",
    "HUDNAV_AA_SYM_PHONE_SETTER": "aa.sym.phone_setter",
    "HUDNAV_AA_SYM_NAV_SETTER": "aa.sym.nav_setter",
    "HUDNAV_AA_SYM_ON_CHANNEL_OPENED": "aa.sym.onChannelOpened",
    "HUDNAV_AA_SYM_NAV_START": "aa.sym.nav_start",
    "HUDNAV_AA_SYM_NAV_VTABLE": "aa.sym.nav_vtable",
}


def resolve_files(root):
    """Map each logical stock file name to its actual (possibly nested) path."""
    resolver = port_check.FirmwareFiles(str(root))
    found = {}
    for name in STOCK_FILES:
        path = resolver.get(name)
        found[name] = Path(path) if path else None
    return found


def find_stock():
    """Return (directory, cleanup) for the stock files, or (None, noop) to SKIP."""
    env_dir = os.environ.get("HUDNAV_FIRMWARE_DIR")
    if env_dir and Path(env_dir).is_dir():
        return Path(env_dir), lambda: None
    tar = None
    env_tar = os.environ.get("HUDNAV_STOCK_TARBALL")
    if env_tar and Path(env_tar).is_file():
        tar = Path(env_tar)
    else:
        for candidate in (
            r"work/stock-plain.tar.gz",
            "work/stock-plain.tar.gz",
        ):
            known = Path(candidate)
            if known.is_file():
                tar = known
                break
    if tar is None:
        return None, lambda: None
    tmp = tempfile.mkdtemp(prefix="hudnav-port-test-")
    with tarfile.open(tar, "r:*") as archive:
        wanted = set(STOCK_FILES)
        members = [m for m in archive.getmembers() if m.isfile()
                   and (m.name.split("/")[-1] in wanted or m.name.endswith(".ver"))]
        try:
            archive.extractall(tmp, members=members, filter="data")
        except TypeError:  # Python < 3.12
            archive.extractall(tmp, members=members)
    return Path(tmp), lambda: shutil.rmtree(tmp, ignore_errors=True)


def check(profile, key, expected_value, source):
    row = profile["items"].get(key)
    assert row is not None, "missing profile item %s (%s)" % (key, source)
    assert row["status"] == "found", "%s: status=%s (expected found), source=%s" % (
        key, row["status"], source)
    assert row.get("reference_match") is True, "%s: value=%r expected=%r, source=%s" % (
        key, row["value"], expected_value, source)
    assert row["value"] == expected_value, "%s: value=%r expected=%r, source=%s" % (
        key, row["value"], expected_value, source)


def test_reproduces_hardcoded_values(firmware_dir, files):
    profile = port_check.analyze(str(firmware_dir), PROFILE_ID)
    for key, (source, expected) in EXPECTED.items():
        if key == "aa.build_tag":
            row = profile["items"][key]
            assert row["status"] == "found" and row["match_count"] >= 1, (key, source)
            assert row["expected"] == expected, (key, source)
        else:
            check(profile, key, expected, source)
    assert profile["verdict"]["overall"] == "CarPlay ready + AA ready", profile["verdict"]
    print("  %d hard-coded values reproduced (hashes, offsets, symbols, strings)" % len(EXPECTED))

    # The checked-in profile must be the one the generator derives from stock.
    checked = json.loads(CHECKED_PROFILE.read_text(encoding="utf-8"))
    assert checked["schema"] == port_check.SCHEMA, checked["schema"]
    assert checked["firmware"]["id"] == PROFILE_ID, checked["firmware"]
    for key in EXPECTED:
        row, fresh = checked["items"][key], profile["items"][key]
        assert row["status"] == "found" and row.get("reference_match") is True, (key, row)
        assert row["value"] == fresh["value"], (key, row["value"], fresh["value"])
    for key in HEADER_FPS.values():
        assert checked["items"][key]["signature_hex"] == profile["items"][key]["signature_hex"], key
    print("  checked-in 260319 profile matches a fresh scan of the stock files")

    # Generate the C header exactly as the build scripts do, then compare every
    # firmware-specific literal the two injected libraries actually compile.
    with tempfile.TemporaryDirectory(prefix="hudnav-header-") as tmp:
        header = generate_header(CHECKED_PROFILE, Path(tmp) / "fw_profile.h")
    defines = header_defines(header)
    assert defines["HUDNAV_FW_PROFILE_ID"].strip('"') == PROFILE_ID
    assert int(defines["CPRG_SITE_BYTES"], 0) == 16
    assert int(defines["CPRG_P_FP_LEN"], 0) == 64
    for macro, key in HEADER_OFFSETS.items():
        assert int(defines[macro], 0) == profile["items"][key]["value"], macro
    inits = header_initializers(header)
    for macro, key in HEADER_FPS.items():
        assert inits[macro] == profile["items"][key]["signature_hex"], macro
    for macro, key in HEADER_SYMS.items():
        assert defines[macro].strip('"') == profile["items"][key]["dynamic_symbol"], macro
    print("  generated fw_profile.h offsets/fingerprints/symbols match the profile")

    # The C sources must consume the generated header, not re-declare literals.
    cp_c = (SRC / "cp_rg.c").read_text(encoding="utf-8")
    shim = (SRC / "aa_shim.c").read_text(encoding="utf-8")
    assert '#include "fw_profile.h"' in cp_c and '#include "fw_profile.h"' in shim
    for macro in list(HEADER_OFFSETS) + list(HEADER_FPS) + ["CPRG_SITE_BYTES", "CPRG_P_FP_LEN"]:
        assert macro in cp_c, "%s is not used by src/cp_rg.c" % macro
    for macro in HEADER_SYMS:
        assert macro in shim, "%s is not used by src/aa_shim.c" % macro
    print("  cp_rg.c and aa_shim.c read all firmware values from fw_profile.h")


def test_cli_report(firmware_dir):
    proc = subprocess.run([sys.executable, "-I", str(TOOL), "--input", str(firmware_dir),
                           "--report", "--firmware-id", PROFILE_ID],
                          capture_output=True, text=True, timeout=120)
    assert proc.returncode == 0, proc.stderr
    out = proc.stdout
    for needle in ("HUD NAV PORT CHECK", "260319", "CarPlay ready + AA ready",
                   "OK        cp.sig.site1", "OK        aa.sym.nav_vtable", "Totals:"):
        assert needle in out, "report missing %r\n%s" % (needle, out)
    print("  python -I port_check.py --report produced the expected matrix")


def test_corrupted_copy_is_missing(files, work):
    corrupt = work / "corrupt"
    corrupt.mkdir()
    for name, source in files.items():
        shutil.copy2(source, corrupt / name)
    cp = bytearray((corrupt / "iAP2Service-2.0").read_bytes())
    for key in ("cp.sig.P", "cp.sig.site1", "cp.sig.site2", "cp.sig.site3",
                "cp.sig.dispatcher", "cp.sig.encoder", "cp.sig.packet_ctor",
                "cp.sig.send", "cp.sig.packet_dtor"):
        offset = EXPECTED[key][1]
        cp[offset] ^= 0xFF  # destroy one byte of each unique signature
    (corrupt / "iAP2Service-2.0").write_bytes(cp)
    for name in ("liblinux-head-unit.so", "libreceiver-lib.so"):
        elf = bytearray((corrupt / name).read_bytes())
        elf[0:4] = b"NOPE"  # .dynsym walker must fail gracefully
        (corrupt / name).write_bytes(elf)

    profile = port_check.analyze(str(corrupt), "NE_E.AUS.S5W_L.001.001.260319")
    for key in ("cp.sig.P", "cp.sig.site1", "cp.sig.site2", "cp.sig.site3"):
        assert profile["items"][key]["status"] == "missing", (key, profile["items"][key])
    for key, row in profile["items"].items():
        if key.startswith("aa.sym."):
            assert row["status"] == "missing", (key, row)
    assert profile["items"]["hash.iAP2Service-2.0"]["reference_match"] is False
    assert profile["verdict"]["overall"] == "unsupported", profile["verdict"]
    print("  corrupted bytes -> MISSING signatures/symbols; verdict unsupported")


def test_duplicated_signature_is_ambiguous(files, work):
    dup = work / "duplicate"
    dup.mkdir()
    for name, source in files.items():
        shutil.copy2(source, dup / name)
    original = (dup / "iAP2Service-2.0").read_bytes()
    site1 = bytes.fromhex(port_check.SIGS[1][3])
    site3 = bytes.fromhex(port_check.SIGS[3][3])
    (dup / "iAP2Service-2.0").write_bytes(original + b"\x90" * 16 + site1 + site3)
    profile = port_check.analyze(str(dup), "NE_E.AUS.S5W_L.001.001.260319")
    for key in ("cp.sig.site1", "cp.sig.site3"):
        row = profile["items"][key]
        assert row["status"] == "ambiguous", (key, row)
        assert row["match_count"] == 2, (key, row)
    assert profile["items"]["cp.sig.site2"]["status"] == "found"
    assert profile["verdict"]["carplay"] == "not-ready", profile["verdict"]
    print("  duplicated signature -> AMBIGUOUS, patcher must stop")


def main():
    firmware_dir, cleanup = find_stock()
    if firmware_dir is None:
        print("SKIP: no stock 260319 files found. Set HUDNAV_FIRMWARE_DIR or "
              "HUDNAV_STOCK_TARBALL (or place stock-plain.tar.gz in "
              "work) to run this acceptance test.")
        return
    try:
        files = resolve_files(firmware_dir)
        missing = [name for name, path in files.items() if path is None or not path.is_file()]
        if missing:
            print("SKIP: stock tree lacks %s" % ", ".join(missing))
            return
        print("port_check acceptance test (stock: %s)" % firmware_dir)
        test_reproduces_hardcoded_values(firmware_dir, files)
        test_cli_report(firmware_dir)
        with tempfile.TemporaryDirectory(prefix="hudnav-port-negative-") as tmp:
            work = Path(tmp)
            test_corrupted_copy_is_missing(files, work)
            test_duplicated_signature_is_ambiguous(files, work)
        print("port_check acceptance test passed")
    finally:
        cleanup()


if __name__ == "__main__":
    main()
