#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""verify_package.py — verify a SCOURGE normal-USB update package end to end.

Checks:
  1. package tree: all stock files present; only the expected files differ
  2. rootfs container: size/IV/integrity/decrypt/gzip/tar/message/six stubs
  3. .ver rootfs row: CRC32 + size match the new container; version is 260128
  4. .lge.upgrade.xml: stock bytes, decrypts, parses as XML
  5. AppUpgrade wrapper: stock (second) or patched (wrapper), class 0xf5
  6. .ver AppUpgrade row 341142: CRC32 + size match the package wrapper
  7. MANIFEST.sha256 verifies

TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).

Usage:
  verify_package.py --variant second|wrapper \
      --package out/package_second --stock input/package_stock \
      --container out/stage1.te2 --ver out/package_second/IONIQ5_EU.ver
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zlib

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "payload"))
from patch_validators import (ENGINE_PATCHES, SCDM_PATCHES,  # noqa: E402
                              DMCLIENT_PATCHES, LAUNCHER_PATCHES,
                              LAUNCHER_STUB)

FIRMWARE_ID = "NE_E.EUR.S5W_L.001.001.260128"
GEN_SIZE = 879126179
BODY_SIZE = 879109759
STOCK_IV = "5412d68d2f22bf3d30f3c1ff1e715326"
GENUINE_TAR_ENTRIES = 66836
VER_FILE = "IONIQ5_EU.ver"
NEW_FILES = [
    "./etc/initial_root/run_stuff_loader.sh",
    "./etc/initial_root/updater_guard.sh",
    "./etc/systemd/system/initial-root-failsafe.service",
    "./etc/systemd/system/multi-user.target.wants/initial-root-failsafe.service",
]
QM_FILES = [
    "./app/share/AppStandbyClock/localization/qm/translation_en-gb.qm",
    "./app/share/AppStandbyClock/localization/qm/translation_en-au.qm",
]
EXPECTED_EXTRA = {"BUILD.txt", "MANIFEST.sha256"}

_ok = True


def check(cond, msg):
    global _ok
    print(("  [PASS] " if cond else "  [FAIL] ") + msg)
    if not cond:
        _ok = False


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def crc32_file(p):
    c = 0
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            c = zlib.crc32(b, c)
    return c & 0xffffffff


def signed32(v):
    return v - (1 << 32) if v >= (1 << 31) else v


def rel_files(root):
    out = set()
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            full = os.path.join(dirpath, name)
            out.add(os.path.relpath(full, root))
    return out


def te2_tool():
    d = os.environ.get("SCOURGE_TOCTOU_REPO",
                      os.path.join(HERE, "third_party/ioniq5-usb-toctou"))
    return os.path.join(d, "pizero/gen5w_decrypt.py")


def te2_key():
    key = os.environ.get("TE2_CONTENT_KEY")
    if not key:
        kf = os.path.join(HERE, "config/key.local")
        if os.path.exists(kf):
            key = open(kf).read().strip()
    if not key:
        sys.exit("ERROR: TE2 content key not set (config/key.local or $TE2_CONTENT_KEY)")
    return key


def inspect_container(path, basename):
    """Run gen5w_decrypt.py inspect on a symlink with the HU-visible basename."""
    tmp = tempfile.mkdtemp(prefix="scourge_inspect_")
    try:
        link = os.path.join(tmp, basename)
        os.symlink(os.path.abspath(path), link)
        out = subprocess.check_output(
            [sys.executable, te2_tool(), "inspect", link],
            text=True, stderr=subprocess.DEVNULL, env=dict(os.environ))
        return json.loads(out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def decrypt_container(path, basename, out_path):
    tmp = tempfile.mkdtemp(prefix="scourge_decrypt_")
    try:
        link = os.path.join(tmp, basename)
        os.symlink(os.path.abspath(path), link)
        env = dict(os.environ, TE2_CONTENT_KEY=te2_key())
        subprocess.run(
            [sys.executable, te2_tool(), "decrypt", link, out_path, "--force"],
            check=True, env=env, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", choices=("second", "wrapper"), default="second")
    ap.add_argument("--package", required=True)
    ap.add_argument("--stock", required=True)
    ap.add_argument("--container", required=True)
    ap.add_argument("--ver", required=True)
    a = ap.parse_args()

    pkg = os.path.abspath(a.package)
    stock = os.path.abspath(a.stock)
    container = os.path.abspath(a.container)
    ver = os.path.abspath(a.ver)

    print("1. package tree")
    stock_files = rel_files(stock)
    pkg_files = rel_files(pkg)
    missing = stock_files - pkg_files
    extra = pkg_files - stock_files
    check(not missing, "all %d stock files present" % len(stock_files))
    if missing:
        print("       missing: %s" % sorted(missing)[:10])
    check(extra == EXPECTED_EXTRA,
          "extra files == %s (got %s)" % (sorted(EXPECTED_EXTRA), sorted(extra)))

    expected_changed = {"HU/images/mango-rootfs.tar.gz", VER_FILE}
    if a.variant == "wrapper":
        expected_changed.add("AppUpgrade")

    # Unchanged files should be hardlinks (same inode) or byte-identical.
    changed = []
    for rel in sorted(stock_files - expected_changed):
        sp = os.path.join(stock, rel)
        pp = os.path.join(pkg, rel)
        if not os.path.exists(pp):
            continue
        ss, ps = os.stat(sp), os.stat(pp)
        if (ss.st_dev, ss.st_ino) == (ps.st_dev, ps.st_ino):
            continue
        if sha256_file(sp) != sha256_file(pp):
            changed.append(rel)
    check(not changed, "unchanged stock files are byte-identical")
    if changed:
        print("       changed unexpectedly: %s" % changed[:10])

    print("2. rootfs container")
    check(os.path.getsize(container) == GEN_SIZE,
          "size %d == %d" % (os.path.getsize(container), GEN_SIZE))
    print("       sha256 %s" % sha256_file(container))
    rep = inspect_container(container, "mango-rootfs.tar.gz")
    check(rep.get("integrity_valid") is True, "metadata integrity valid")
    check(rep.get("body_size") == BODY_SIZE,
          "body size %s == %d" % (rep.get("body_size"), BODY_SIZE))
    check(rep.get("byte3_hex") == "0xe0", "class 0xe0 (got %s)" % rep.get("byte3_hex"))
    check(rep.get("iv") == STOCK_IV,
          "IV matches stock (got %s)" % rep.get("iv"))

    plain = os.path.join(tempfile.gettempdir(), "scourge_verify_plain.tar.gz")
    if os.path.exists(plain):
        os.unlink(plain)
    decrypt_container(container, "mango-rootfs.tar.gz", plain)
    check(os.path.getsize(plain) == BODY_SIZE,
          "plaintext size %d == %d" % (os.path.getsize(plain), BODY_SIZE))
    subprocess.run(["gzip", "-t", plain], check=True)
    check(True, "gzip -t")

    names = subprocess.check_output(["tar", "-tzf", plain], text=True).splitlines()
    check(len(names) >= GENUINE_TAR_ENTRIES,
          "tar entries %d >= %d" % (len(names), GENUINE_TAR_ENTRIES))
    for n in NEW_FILES:
        check(n in names, "entry %s" % n)

    tmp = tempfile.mkdtemp(prefix="scourge_verify_")
    try:
        subprocess.run(["tar", "-xzf", plain, "-C", tmp,
                        "./app/bin/AppUpgrade", "./app/bin/AppScomoDM",
                        "./app/bin/AppDMClient", "./app/bin/AppLauncher"]
                       + QM_FILES,
                       check=True)
        data = open(os.path.join(tmp, "app/bin/AppUpgrade"), "rb").read()
        for name, off, _expect in ENGINE_PATCHES:
            check(data[off:off + 3] == b"\x31\xc0\xc3",
                  "%s bytes at %d = %s" % (name, off, data[off:off + 3].hex()))
        sdata = open(os.path.join(tmp, "app/bin/AppScomoDM"), "rb").read()
        for name, off, _expect in SCDM_PATCHES:
            check(sdata[off:off + 3] == b"\x31\xc0\xc3",
                  "%s bytes at %#x = %s" % (name, off, sdata[off:off + 3].hex()))
        ddata = open(os.path.join(tmp, "app/bin/AppDMClient"), "rb").read()
        for name, off, _expect in DMCLIENT_PATCHES:
            check(ddata[off:off + 3] == b"\x31\xc0\xc3",
                  "%s bytes at %#x = %s" % (name, off, ddata[off:off + 3].hex()))
        ldata = open(os.path.join(tmp, "app/bin/AppLauncher"), "rb").read()
        for name, off, _expect in LAUNCHER_PATCHES:
            check(ldata[off:off + len(LAUNCHER_STUB)] == LAUNCHER_STUB,
                  "%s bytes at %d = %s" % (name, off,
                                           ldata[off:off + len(LAUNCHER_STUB)].hex()))
        msg = open(os.path.join(HERE, "config/message.txt"),
                   encoding="utf-8").read().strip("\n")
        msg_b = msg.encode("utf-16be")
        for qm in QM_FILES:
            d = open(os.path.join(tmp, qm.lstrip("./")), "rb").read()
            check(msg_b in d, "%s carries the message" % os.path.basename(qm))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    os.unlink(plain)

    print("3. .ver rootfs row")
    lines = open(ver, encoding="utf-8").read().splitlines()
    check(FIRMWARE_ID in lines[0], "header version %s" % FIRMWARE_ID)
    rows = [l.split("|") for l in lines
            if len(l.split("|")) >= 6
            and l.split("|")[0].endswith("HU\\images")
            and l.split("|")[1] == "mango-rootfs.tar.gz"]
    check(len(rows) == 1, "exactly one mango-rootfs row (got %d)" % len(rows))
    if rows:
        row = rows[0]
        crc = crc32_file(container)
        check(int(row[3]) == signed32(crc),
              "row CRC %s == %d (0x%08x)" % (row[3], signed32(crc), crc))
        check(int(row[4]) == os.path.getsize(container),
              "row size %s == %d" % (row[4], os.path.getsize(container)))
        print("       row: %s" % "|".join(row))

    print("4. .lge.upgrade.xml")
    stock_xml = os.path.join(stock, ".lge.upgrade.xml")
    pkg_xml = os.path.join(pkg, ".lge.upgrade.xml")
    check(sha256_file(stock_xml) == sha256_file(pkg_xml),
          "package XML is the stock signed XML")
    xml_plain = os.path.join(tempfile.gettempdir(), "scourge_verify_xml")
    if os.path.exists(xml_plain):
        os.unlink(xml_plain)
    decrypt_container(pkg_xml, ".lge.upgrade.xml", xml_plain)
    try:
        root = ET.parse(xml_plain).getroot()
        check(root.tag == "wideavn", "XML parses (root <%s>)" % root.tag)
        comps = [e.tag for e in root]
        check("firmware" in comps, "XML has firmware entries")
    finally:
        os.unlink(xml_plain)

    print("5. AppUpgrade wrapper")
    stock_ap = os.path.join(stock, "AppUpgrade")
    pkg_ap = os.path.join(pkg, "AppUpgrade")
    if a.variant == "second":
        check(sha256_file(stock_ap) == sha256_file(pkg_ap),
              "package AppUpgrade is the stock wrapper")
    else:
        check(sha256_file(stock_ap) != sha256_file(pkg_ap),
              "package AppUpgrade is the patched wrapper")
    rep = inspect_container(pkg_ap, "AppUpgrade")
    check(rep.get("integrity_valid") is True, "wrapper metadata integrity valid")
    check(rep.get("byte3_hex") == "0xf5", "wrapper class 0xf5 (got %s)" % rep.get("byte3_hex"))

    if a.variant == "wrapper":
        wrapper_plain = os.path.join(tempfile.gettempdir(), "scourge_verify_wrapper")
        if os.path.exists(wrapper_plain):
            os.unlink(wrapper_plain)
        decrypt_container(pkg_ap, "AppUpgrade", wrapper_plain)
        wdata = open(wrapper_plain, "rb").read()
        check(len(wdata) == 10787256,
              "wrapper plaintext size %d == 10787256" % len(wdata))
        for name, off, _expect in ENGINE_PATCHES:
            check(wdata[off:off + 3] == b"\x31\xc0\xc3",
                  "wrapper %s bytes at %d = %s" % (name, off,
                                                   wdata[off:off + 3].hex()))
        os.unlink(wrapper_plain)

    print("6. .ver AppUpgrade row 341142")
    ap_rows = [l.split("|") for l in lines
               if len(l.split("|")) >= 6 and l.split("|")[2] == "341142"]
    check(len(ap_rows) == 1, "exactly one AppUpgrade row (got %d)" % len(ap_rows))
    if ap_rows:
        row = ap_rows[0]
        crc = crc32_file(pkg_ap)
        check(int(row[3]) == signed32(crc),
              "row CRC %s == %d (0x%08x)" % (row[3], signed32(crc), crc))
        check(int(row[4]) == os.path.getsize(pkg_ap),
              "row size %s == %d" % (row[4], os.path.getsize(pkg_ap)))
        print("       row: %s" % "|".join(row))

    print("7. MANIFEST.sha256")
    p = subprocess.run(["sha256sum", "-c", "MANIFEST.sha256"], cwd=pkg,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    check(p.returncode == 0, "manifest verifies")
    if p.returncode != 0:
        print(p.stderr[-2000:])

    print()
    print("PACKAGE VERIFY: %s" % ("OK" if _ok else "FAIL"))
    return 0 if _ok else 1


if __name__ == "__main__":
    sys.exit(main())
