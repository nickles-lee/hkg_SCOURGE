#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""03_patch_payload.py — extract + patch the stage-1 payload binaries.

TARGET: Ioniq 5 Gen5W_L, firmware NE_E.EUR.S5W_L.001.001.260128 (EU).

From the stock plaintext rootfs tar, produce in work/payload/ (build 5
carries the eleven-stub patch set and four binaries; build 6 adds the WIP
all-locale .qm refresh):
  - AppUpgrade.patched    five engine stubs (31 C0 C3: SCOMO four + FUMO)
  - AppScomoDM.patched    two UsbUpdateManager stubs (31 C0 C3)
  - AppDMClient.patched   two button-gate stubs (31 C0 C3)
  - AppLauncher.patched   two .sign-gate stubs (B8 01 00 00 00 C3 —
                          true-on-success; prerequisite for a modified
                          AppDMClient to survive a boot)
  - translation_<locale>.qm
                          every SAFETY_OBEY translation replaced with the
                          locale's text from config/messages.json,
                          U+0020-padded to the original byte length
                          (structure and length preserved)

Every binary is hash-gated by payload/patch_validators.py against
config/firmware_locks.env before any byte is touched. The message patcher
refuses to leave a firmware locale unmapped or to grow a translation past
its original slot.

Usage:
  03_patch_payload.py --stock-tar work/stock-plain.tar.gz \
      --messages config/messages.json --out work/payload

  # message-only refresh from an extracted localization/ tree
  03_patch_payload.py --stock-qm-dir work/rootfs/app/share/AppStandbyClock/localization \
      --messages config/messages.json --out work/payload-qm
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "payload"))
from patch_validators import apply as apply_engine       # noqa: E402
from patch_validators import apply_scomo as apply_scomo  # noqa: E402
from patch_validators import apply_dmclient as apply_dmclient  # noqa: E402
from patch_validators import apply_launcher as apply_launcher  # noqa: E402
from boot_message import (QM_DIR, load_messages, locales_in_dir,  # noqa: E402
                          patch_qm, qm_entry, safety_keys, ts_entry)

APPUPGRADE = "./app/bin/AppUpgrade"
APPSCDM = "./app/bin/AppScomoDM"
APPDMCLIENT = "./app/bin/AppDMClient"
APPLAUNCHER = "./app/bin/AppLauncher"


def extract(stock_tar, members, dest):
    os.makedirs(dest, exist_ok=True)
    subprocess.run(["tar", "-xzf", stock_tar, "-C", dest] + members, check=True)


def stock_localization(stock_tar):
    """Return (qm members, ts members, locales) for the stock rootfs tar."""
    names = subprocess.check_output(["tar", "-tzf", stock_tar],
                                    text=True).splitlines()
    qms = sorted(n for n in names
                 if n.startswith(QM_DIR + "/translation_") and n.endswith(".qm"))
    if not qms:
        raise SystemExit("no AppStandbyClock .qm entries in %s" % stock_tar)
    locales = [n[len(QM_DIR + "/translation_"):-len(".qm")] for n in qms]
    tss = [ts_entry(loc) for loc in locales]
    missing = [ts for ts in tss if ts not in names]
    if missing:
        raise SystemExit("stock .ts missing for: %s" % ", ".join(missing))
    return qms, tss, locales


def check_coverage(messages, locales, source):
    missing = sorted(set(locales) - set(messages))
    extra = sorted(set(messages) - set(locales))
    if missing:
        raise SystemExit("%s: no message for firmware locale(s): %s" %
                         (source, ", ".join(missing)))
    if extra:
        raise SystemExit("%s: message locale(s) not in firmware: %s" %
                         (source, ", ".join(extra)))


def verify_qm(path, new_text, keys):
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
    for source, _comment in keys:
        s = t.translate("QMLTranslationModel", source)
        if first_line not in s:
            raise SystemExit("%s: patched text missing for %s" % (path, source))
    print("  QTranslator OK: %s" % os.path.basename(path))


def patch_locales(qm_sources, ts_sources, messages, out_dir):
    tmp_out = tempfile.mkdtemp(prefix=".qm-", dir=out_dir)
    try:
        for locale in sorted(messages):
            qm_src = qm_sources[locale]
            ts_src = ts_sources[locale]
            keys = safety_keys(ts_src)
            if not keys:
                raise SystemExit("%s: no SAFETY_OBEY translation" % ts_src)
            data = bytearray(open(qm_src, "rb").read())
            try:
                count = patch_qm(data, keys, messages[locale])
            except ValueError as e:
                raise SystemExit("%s: %s" % (os.path.basename(qm_src), e))
            dst = os.path.join(tmp_out, "translation_%s.qm" % locale)
            open(dst, "wb").write(data)
            print("patched %s (%d bytes, %d entry/entries)" %
                  (os.path.basename(dst), len(data), count))
            verify_qm(dst, messages[locale], keys)
        fresh = set(os.listdir(tmp_out))
        for name in fresh:
            os.replace(os.path.join(tmp_out, name),
                       os.path.join(out_dir, name))
        for name in os.listdir(out_dir):
            if (name.startswith("translation_") and name.endswith(".qm")
                    and name not in fresh):
                os.unlink(os.path.join(out_dir, name))
    finally:
        shutil.rmtree(tmp_out, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock-tar", default=os.path.join(HERE, "work/stock-plain.tar.gz"))
    ap.add_argument("--stock-qm-dir",
                    help="extracted AppStandbyClock/localization directory; "
                         "patch .qm files only")
    ap.add_argument("--messages",
                    default=os.path.join(HERE, "config/messages.json"))
    ap.add_argument("--out", default=os.path.join(HERE, "work/payload"))
    ap.add_argument("--qm-only", action="store_true",
                    help="skip the four binary patches")
    a = ap.parse_args()

    messages = load_messages(a.messages)
    os.makedirs(a.out, exist_ok=True)

    if a.stock_qm_dir:
        locales = locales_in_dir(a.stock_qm_dir)
        check_coverage(messages, locales, a.stock_qm_dir)
        qm_sources = {loc: os.path.join(a.stock_qm_dir, "qm",
                                        "translation_%s.qm" % loc)
                      for loc in locales}
        ts_sources = {loc: os.path.join(a.stock_qm_dir, "ts",
                                        "translation_%s.ts" % loc)
                      for loc in locales}
        missing = [p for p in list(qm_sources.values()) + list(ts_sources.values())
                   if not os.path.isfile(p)]
        if missing:
            raise SystemExit("missing stock localization file(s): %s" %
                             ", ".join(missing))
        patch_locales(qm_sources, ts_sources, messages, a.out)
        print("payload ready in %s" % a.out)
        return 0

    qm_members, ts_members, locales = stock_localization(a.stock_tar)
    check_coverage(messages, locales, a.stock_tar)
    tmp = tempfile.mkdtemp(prefix="scourge_patch_")
    try:
        members = qm_members + ts_members
        if not a.qm_only:
            members = [APPUPGRADE, APPSCDM, APPDMCLIENT, APPLAUNCHER] + members
        extract(a.stock_tar, members, tmp)

        if not a.qm_only:
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

        qm_sources = {loc: os.path.join(tmp, qm_entry(loc).lstrip("./"))
                      for loc in locales}
        ts_sources = {loc: os.path.join(tmp, ts_entry(loc).lstrip("./"))
                      for loc in locales}
        patch_locales(qm_sources, ts_sources, messages, a.out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("payload ready in %s" % a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
