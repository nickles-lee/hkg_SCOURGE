#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Turn a hudnav-firmware-profile/1 JSON into a generated C header.

This is a BUILD-TIME tool. It never runs inside the injected libraries: the
shipped .so files keep their current runtime gates (fingerprint verification,
rel32 validation, fail-closed wrappers) and gain no file/JSON parsing at all.

Usage:
    python3 -I tools/profile_to_header.py \
        --profile profiles/NE_E.AUS.S5W_L.001.001.260319.json \
        --output build/fw_profile.h

Every C literal that is firmware-specific is read from a required profile row.
A missing/ambiguous/unusable row is a hard build error with an explicit message;
there is deliberately no fallback to the 260319 literals. Porters add a profile,
they do not edit src/cp_rg.c or src/aa_shim.c.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys

SCHEMA = "hudnav-firmware-profile/1"
TOOL_VERSION = "1.0.0"

# profile item key -> generated macro (fixed offsets in iAP2Service-2.0)
OFFSET_MACROS = (
    ("cp.sig.P", "CPRG_P_OFF"),
    ("cp.sig.site1", "CPRG_SITE1_OFF"),
    ("cp.sig.site2", "CPRG_SITE2_OFF"),
    ("cp.sig.site3", "CPRG_SITE3_OFF"),
    ("cp.sig.dispatcher", "CPRG_DISP_OFF"),
    ("cp.sig.encoder", "CPRG_ENC_OFF"),
    ("cp.sig.packet_ctor", "CPRG_PKTC_OFF"),
    ("cp.sig.send", "CPRG_SEND_OFF"),
    ("cp.sig.packet_dtor", "CPRG_PKTD_OFF"),
    ("cp.plt._ZdaPv", "CPRG_DELARR_OFF"),
)

# profile item key -> initializer macro (the short signatures already in cp_rg.c)
SIGNATURE_MACROS = (
    ("cp.sig.P", "CPRG_P_FP"),
    ("cp.sig.site1", "CPRG_SITE1_FP"),
    ("cp.sig.site2", "CPRG_SITE2_FP"),
    ("cp.sig.site3", "CPRG_SITE3_FP"),
)

# profile item key -> generated macro (exported mangled names looked up by aa_shim.c)
AA_SYMBOL_MACROS = (
    ("aa.sym.status_cb", "HUDNAV_AA_SYM_STATUS_CB"),
    ("aa.sym.distance_cb", "HUDNAV_AA_SYM_DISTANCE_CB"),
    ("aa.sym.turn_cb", "HUDNAV_AA_SYM_TURN_CB"),
    ("aa.sym.phone_setter", "HUDNAV_AA_SYM_PHONE_SETTER"),
    ("aa.sym.nav_setter", "HUDNAV_AA_SYM_NAV_SETTER"),
    ("aa.sym.onChannelOpened", "HUDNAV_AA_SYM_ON_CHANNEL_OPENED"),
    ("aa.sym.nav_start", "HUDNAV_AA_SYM_NAV_START"),
    ("aa.sym.nav_vtable", "HUDNAV_AA_SYM_NAV_VTABLE"),
)

HEX_BYTES = re.compile(r"^[0-9a-fA-F]*$")


class ProfileError(Exception):
    pass


def _row(items, key, errors):
    row = items.get(key)
    if not isinstance(row, dict):
        errors.append("items[%r]: required field missing" % key)
        return None
    status = row.get("status")
    if status != "found":
        errors.append("items[%r].status: %r (expected 'found'; a missing or "
                      "ambiguous hook must not be built)" % (key, status))
    return row


def _offset(items, key, macro, errors):
    row = _row(items, key, errors)
    if row is None:
        return None
    value = row.get("value")
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        errors.append("items[%r].value: %r (expected a non-negative integer "
                      "offset for %s)" % (key, value, macro))
        return None
    return value


def _signature(items, key, macro, errors):
    row = _row(items, key, errors)
    if row is None:
        return None
    hexsig = row.get("signature_hex")
    if not isinstance(hexsig, str) or not hexsig or len(hexsig) % 2 or not HEX_BYTES.match(hexsig):
        errors.append("items[%r].signature_hex: %r (expected a non-empty, "
                      "even-length hex string for %s)" % (key, hexsig, macro))
        return None
    return bytes.fromhex(hexsig)


def _symbol(items, key, macro, errors):
    row = _row(items, key, errors)
    if row is None:
        return None
    name = row.get("dynamic_symbol")
    if not isinstance(name, str) or not name or any(ord(c) < 0x20 or c in '"\\' for c in name):
        errors.append("items[%r].dynamic_symbol: %r (expected a plain mangled "
                      "symbol name for %s)" % (key, name, macro))
        return None
    return name


def _c_bytes(data):
    """Format a byte string like the original cp_rg.c arrays (16/line).

    Every emitted line is a continued preprocessor line: the trailing ``}``
    written by generate() terminates the macro definition.
    """
    chunks = [data[i:i + 16] for i in range(0, len(data), 16)]
    lines = []
    for index, chunk in enumerate(chunks):
        text = "        " + ", ".join("0x%02x" % b for b in chunk)
        if index < len(chunks) - 1:
            text += ","
        lines.append(text + " \\")
    return "\n".join(lines)


def generate(profile, source_name="profile.json"):
    """Return (header_text, errors). Never raises on a malformed profile."""
    errors = []
    if not isinstance(profile, dict):
        return "", ["profile is not a JSON object"]
    if profile.get("schema") != SCHEMA:
        errors.append("schema: %r (expected %r)" % (profile.get("schema"), SCHEMA))
    firmware = profile.get("firmware")
    fwid = firmware.get("id") if isinstance(firmware, dict) else None
    if not isinstance(fwid, str) or not fwid:
        errors.append("firmware.id: %r (expected a non-empty string)" % fwid)
    items = profile.get("items")
    if not isinstance(items, dict):
        errors.append("items: %r (expected a JSON object)" % (items,))
        return "", errors

    offsets = [(key, macro, _offset(items, key, macro, errors))
               for key, macro in OFFSET_MACROS]
    sigs = [(key, macro, _signature(items, key, macro, errors))
            for key, macro in SIGNATURE_MACROS]
    syms = [(key, macro, _symbol(items, key, macro, errors))
            for key, macro in AA_SYMBOL_MACROS]

    site_lengths = {len(data) for key, _m, data in sigs
                    if key in ("cp.sig.site1", "cp.sig.site2", "cp.sig.site3") and data is not None}
    if len(site_lengths) > 1:
        errors.append("cp.sig.site1/site2/site3 signature lengths differ (%s): "
                      "CPRG_SITE_BYTES must be one value" %
                      ", ".join(str(n) for n in sorted(site_lengths)))
    site_bytes = site_lengths.pop() if len(site_lengths) == 1 else None
    if site_bytes is not None and site_bytes < 5:
        errors.append("cp.sig.site* signatures are %d bytes; at least 5 are "
                      "needed to cover the rel32 call" % site_bytes)
    p_data = next((data for key, _m, data in sigs if key == "cp.sig.P"), None)
    if p_data is not None and len(p_data) < 16:
        errors.append("cp.sig.P signature is %d bytes; at least 16 are needed "
                      "for a safe function fingerprint" % len(p_data))

    if errors:
        return "", errors

    out = []
    w = out.append
    w("/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors")
    w(" * SPDX-License-Identifier: AGPL-3.0-only")
    w(" *")
    w(" * GENERATED FILE - do not edit by hand. Regenerate with:")
    w(" *   python3 -I tools/profile_to_header.py --profile PROFILE --output build/fw_profile.h")
    w(" * Profile: %s" % os.path.basename(source_name))
    w(" * Firmware: %s" % fwid)
    w(" * Generator: hudnav-firmware-profile/1 -> header v%s" % TOOL_VERSION)
    w(" */")
    w("#ifndef HUDNAV_FW_PROFILE_H")
    w("#define HUDNAV_FW_PROFILE_H")
    w("")
    w('#define HUDNAV_FW_PROFILE_ID "%s"' % fwid)
    w("")
    w("/* Fixed offsets / PLT slot in %s (file offsets == PIE-relative vaddrs). */" %
      ("iAP2Service-2.0"))
    for _key, macro, value in offsets:
        w("#define %s 0x%x" % (macro, value))
    w("")
    w("/* Fingerprint bytes. cp_rg.c keeps all memcmp/rel32 gates unchanged. */")
    w("#define CPRG_P_FP_LEN %d" % len(p_data))
    w("#define CPRG_SITE_BYTES %d" % site_bytes)
    for _key, macro, data in sigs:
        w("#define %s_INIT { \\" % macro)
        w(_c_bytes(data))
        w("}")
    w("")
    w("/* Exported mangled names looked up at runtime via dl_iterate_phdr/.dynsym. */")
    for _key, macro, name in syms:
        w('#define %s "%s"' % (macro, name))
    w("")
    w("#endif /* HUDNAV_FW_PROFILE_H */")
    return "\n".join(out) + "\n", []


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--profile", required=True,
                        help="hudnav-firmware-profile/1 JSON file")
    parser.add_argument("--output", required=True,
                        help="generated C header path")
    args = parser.parse_args(argv)
    try:
        with open(args.profile, "r", encoding="utf-8") as fh:
            profile = json.load(fh)
    except (OSError, ValueError) as exc:
        print("profile_to_header: cannot read profile %s: %s" % (args.profile, exc),
              file=sys.stderr)
        return 2
    header, errors = generate(profile, args.profile)
    if errors:
        print("profile_to_header: %s is not a usable firmware profile:" % args.profile,
              file=sys.stderr)
        for err in errors:
            print("  - %s" % err, file=sys.stderr)
        print("  (build stopped: add the missing rows with tools/port_check.py; "
              "never edit src/cp_rg.c or src/aa_shim.c for a new firmware)",
              file=sys.stderr)
        return 2
    directory = os.path.dirname(os.path.abspath(args.output))
    try:
        if directory:
            os.makedirs(directory, exist_ok=True)
        with open(args.output, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(header)
    except OSError as exc:
        print("profile_to_header: cannot write %s: %s" % (args.output, exc),
              file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
