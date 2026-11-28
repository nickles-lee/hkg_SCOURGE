#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Acceptance tests for the build-time profile -> C header generator.

The checked-in 260319 profile is the only fixture: no stock binaries, no car,
no network. A profile that is missing a required field (or whose required row
is ambiguous/malformed) must fail the build with an error naming the field,
and must not leave a header behind. There is no fallback to 260319 literals.
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools" / "profile_to_header.py"
PROFILE = ROOT / "profiles" / "NE_E.AUS.S5W_L.001.001.260319.json"


def base():
    return json.loads(PROFILE.read_text(encoding="utf-8"))


def run(profile):
    with tempfile.TemporaryDirectory(prefix="hudnav-hdr-test-") as tmp:
        src = Path(tmp) / "profile.json"
        out = Path(tmp) / "fw_profile.h"
        src.write_text(json.dumps(profile), encoding="utf-8")
        proc = subprocess.run(
            [sys.executable, "-I", str(TOOL), "--profile", str(src),
             "--output", str(out)],
            capture_output=True, text=True, timeout=60)
        header = out.read_text(encoding="utf-8") if out.exists() else None
        return proc, header


def expect_rejected(profile, needle):
    proc, header = run(profile)
    assert proc.returncode != 0, "expected a build failure, got rc=0"
    assert needle in proc.stderr, "error must name %r:\n%s" % (needle, proc.stderr)
    assert "build stopped" in proc.stderr, proc.stderr
    assert header is None, "no header may be written for an invalid profile"
    print("  rejected as expected: %s" % needle)


def test_good_profile():
    proc, header = run(base())
    assert proc.returncode == 0, proc.stderr
    assert header is not None
    assert "SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors" in header
    assert "SPDX-License-Identifier: AGPL-3.0-only" in header
    for macro, value in (
        ("CPRG_P_OFF", "0x1cf4a"),
        ("CPRG_SITE1_OFF", "0x22633"),
        ("CPRG_DELARR_OFF", "0x4ef0"),
        ("CPRG_SITE_BYTES", "16"),
        ("CPRG_P_FP_LEN", "64"),
        ("HUDNAV_AA_SYM_NAV_VTABLE", '"_ZTV24NavigationStatusEndpoint"'),
        ("HUDNAV_AA_SYM_NAV_SETTER",
         '"_ZN21CAndroidAutoInterface37setInstanceINavigationStatusServiceCBEP26INavigationStatusServiceCB"'),
    ):
        assert "#define %s %s" % (macro, value) in header, (macro, value)
    assert "0x55, 0x48, 0x89, 0xe5" in header
    print("  valid profile generates the expected SPDX header and macros")


def test_missing_field():
    p = base()
    del p["items"]["cp.sig.site1"]
    expect_rejected(p, "cp.sig.site1")


def test_ambiguous_row():
    p = base()
    p["items"]["aa.sym.nav_start"]["status"] = "ambiguous"
    expect_rejected(p, "aa.sym.nav_start")


def test_missing_scan_result():
    p = base()
    p["items"]["cp.sig.P"]["status"] = "missing"
    expect_rejected(p, "cp.sig.P")


def test_bad_signature_hex():
    p = base()
    p["items"]["cp.sig.P"]["signature_hex"] = "zz"
    expect_rejected(p, "cp.sig.P")


def test_missing_dynamic_symbol():
    p = base()
    del p["items"]["aa.sym.nav_vtable"]["dynamic_symbol"]
    expect_rejected(p, "aa.sym.nav_vtable")


def test_bad_offset_type():
    p = base()
    p["items"]["cp.sig.send"]["value"] = "0x152de"
    expect_rejected(p, "cp.sig.send")


def test_site_length_mismatch():
    p = base()
    short = p["items"]["cp.sig.site2"]["signature_hex"][:16]
    p["items"]["cp.sig.site2"]["signature_hex"] = short
    expect_rejected(p, "lengths differ")


def test_bad_schema():
    p = base()
    p["schema"] = "hudnav-firmware-profile/2"
    expect_rejected(p, "schema")


def test_missing_firmware_id():
    p = base()
    del p["firmware"]["id"]
    expect_rejected(p, "firmware.id")


def main():
    print("profile_to_header acceptance test")
    for test in (
        test_good_profile, test_missing_field, test_ambiguous_row,
        test_missing_scan_result, test_bad_signature_hex,
        test_missing_dynamic_symbol, test_bad_offset_type,
        test_site_length_mismatch, test_bad_schema, test_missing_firmware_id,
    ):
        test()
    print("profile_to_header acceptance test passed")


if __name__ == "__main__":
    main()
