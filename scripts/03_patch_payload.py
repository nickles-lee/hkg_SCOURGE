#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""03_patch_payload.py — extract + patch the stage-1 payload binaries.

TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).

From the stock plaintext rootfs tar, produce in work/payload/ (build 4 —
the ten-stub patch set, four binaries):
  - AppUpgrade.patched    four validator stubs (31 C0 C3)
  - AppScomoDM.patched    two UsbUpdateManager stubs (31 C0 C3)
  - AppDMClient.patched   two button-gate stubs (31 C0 C3)
  - AppLauncher.patched   two .sign-gate stubs (B8 01 00 00 00 C3 —
                          true-on-success; prerequisite for a modified
                          AppDMClient to survive a boot)
  - translation_en-gb.qm / translation_en-au.qm
                          the SAFETY_OBEY translations replaced with
                          config/message.txt, U+0020-padded to the original
                          byte length (structure and length preserved)

Every binary is hash-gated by payload/patch_validators.py against
config/firmware_locks.env before any byte is touched.

Usage:
  03_patch_payload.py --stock-tar work/stock-plain.tar.gz \
      --message config/message.txt --out work/payload
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "payload"))
from patch_validators import apply as apply_engine       # noqa: E402
from patch_validators import apply_scomo as apply_scomo  # noqa: E402
from patch_validators import apply_dmclient as apply_dmclient  # noqa: E402
from patch_validators import apply_launcher as apply_launcher  # noqa: E402

APPUPGRADE = "./app/bin/AppUpgrade"
APPSCDM = "./app/bin/AppScomoDM"
APPDMCLIENT = "./app/bin/AppDMClient"
APPLAUNCHER = "./app/bin/AppLauncher"
QM_FILES = [
    "./app/share/AppStandbyClock/localization/qm/translation_en-gb.qm",
    "./app/share/AppStandbyClock/localization/qm/translation_en-au.qm",
]
TS_FILES = [
    "./app/share/AppStandbyClock/localization/ts/translation_en-gb.ts",
    "./app/share/AppStandbyClock/localization/ts/translation_en-au.ts",
]


def extract(stock_tar, members, dest):
    os.makedirs(dest, exist_ok=True)
    subprocess.run(["tar", "-xzf", stock_tar, "-C", dest] + members, check=True)


def safety_translations(ts_path):
    tree = ET.parse(ts_path)
    out = []
    for ctx in tree.getroot().findall("context"):
        for msg in ctx.findall("message"):
            src = msg.findtext("source") or ""
            if "SAFETY_OBEY" in src:
                tr = msg.findtext("translation") or ""
                if tr and tr not in out:
                    out.append(tr)
    return out


def patch_qm(src, dst, old_strings, new_text):
    data = bytearray(open(src, "rb").read())
    new = new_text.encode("utf-16be")
    for old in old_strings:
        old_b = old.encode("utf-16be")
        idx = data.find(old_b)
        if idx < 0:
            raise SystemExit("translation not found in %s: %r" % (src, old[:40]))
        if len(new) > len(old_b):
            raise SystemExit("new message too long for %s (%d > %d)" %
                             (src, len(new), len(old_b)))
        pad = (len(old_b) - len(new)) // 2
        repl = new + b"\x00\x20" * pad
        data[idx:idx + len(old_b)] = repl
    open(dst, "wb").write(data)
    print("patched %s (%d bytes, %d translation(s))" %
          (os.path.basename(dst), len(data), len(old_strings)))


def verify_qm(path, new_text):
    try:
        from PyQt5.QtCore import QCoreApplication, QTranslator
    except Exception:
        print("  (PyQt5 not available; skipping runtime .qm check)")
        return
    app = QCoreApplication(sys.argv)
    t = QTranslator()
    if not t.load(path):
        raise SystemExit("QTranslator failed to load %s" % path)
    first_line = new_text.splitlines()[0]
    for key in ("STR_PROFILE_WC_DETAIL_SAFETY_OBEY",
                "STR_WC_DETAIL_SAFETY_OBEY_IKAV"):
        s = t.translate("QMLTranslationModel", key)
        if first_line not in s:
            raise SystemExit("%s: patched text missing for %s" % (path, key))
    print("  QTranslator OK: %s" % os.path.basename(path))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock-tar", default=os.path.join(HERE, "work/stock-plain.tar.gz"))
    ap.add_argument("--message", default=os.path.join(HERE, "config/message.txt"))
    ap.add_argument("--out", default=os.path.join(HERE, "work/payload"))
    a = ap.parse_args()

    new_text = open(a.message, encoding="utf-8").read().strip("\n")
    if len(new_text) > 206:
        raise SystemExit("message exceeds %d characters (the shorter .qm key)"
                         % 206)
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="scourge_patch_")
    try:
        extract(a.stock_tar, [APPUPGRADE, APPSCDM, APPDMCLIENT, APPLAUNCHER]
                + QM_FILES + TS_FILES, tmp)
        data = bytearray(open(os.path.join(tmp, APPUPGRADE.lstrip("./")), "rb").read())
        apply_engine(data)
        open(os.path.join(a.out, "AppUpgrade.patched"), "wb").write(data)
        data = bytearray(open(os.path.join(tmp, APPSCDM.lstrip("./")), "rb").read())
        apply_scomo(data)
        open(os.path.join(a.out, "AppScomoDM.patched"), "wb").write(data)
        data = bytearray(open(os.path.join(tmp, APPDMCLIENT.lstrip("./")), "rb").read())
        apply_dmclient(data)
        open(os.path.join(a.out, "AppDMClient.patched"), "wb").write(data)
        data = bytearray(open(os.path.join(tmp, APPLAUNCHER.lstrip("./")), "rb").read())
        apply_launcher(data)
        open(os.path.join(a.out, "AppLauncher.patched"), "wb").write(data)
        for qm, ts in zip(QM_FILES, TS_FILES):
            old = safety_translations(os.path.join(tmp, ts.lstrip("./")))
            dst = os.path.join(a.out, os.path.basename(qm))
            patch_qm(os.path.join(tmp, qm.lstrip("./")), dst, old, new_text)
            verify_qm(dst, new_text)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("payload ready in %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
