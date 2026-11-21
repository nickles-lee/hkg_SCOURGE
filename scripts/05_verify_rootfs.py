#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""05_verify_rootfs.py — verify a stage-1 TE2 envelope end to end.

Verifies the car-validated build-5 layout and the build-6 WIP boot-message
refresh (all firmware locales) — the stub/ELF checks are identical for both.

Checks:
  1. envelope size == GENUINE_ENVELOPE_SIZE (879,126,179)
  2. TE2 inspect: metadata integrity, IV, body size
  3. decrypt -> exact GENUINE_BODY_SIZE plaintext, gzip -t, tar entries
  4. patched AppUpgrade: five engine stubs present (SCOMO four + FUMO)
  5. patched AppScomoDM: two button-gate stubs present
  6. patched .qm files carry the boot message in every firmware locale
  7. the four failsafe entries + SMACK flag are present in the tar

Usage: 05_verify_rootfs.py [--container out/stage1.te2]
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "payload"))
from patch_validators import (ENGINE_PATCHES, SCDM_PATCHES,  # noqa: E402
                              DMCLIENT_PATCHES, LAUNCHER_PATCHES,
                              LAUNCHER_STUB)
from boot_message import (load_messages, qm_entry,  # noqa: E402
                          read_qm_messages, safety_keys, ts_entry)

# config/firmware_locks.env
GEN_SIZE = 879126179
BODY_SIZE = 879109759
BASENAME = "mango-rootfs.tar.gz"
GENUINE_TAR_ENTRIES = 66836
NEW_FILES = [
    "./etc/initial_root/run_stuff_loader.sh",
    "./etc/initial_root/updater_guard.sh",
    "./etc/systemd/system/initial-root-failsafe.service",
    "./etc/systemd/system/multi-user.target.wants/initial-root-failsafe.service",
    "./etc/smack_permissive",
]


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--container", default=os.path.join(HERE, "out/stage1.te2"))
    ap.add_argument("--work", default=os.path.join(HERE, "work"))
    ap.add_argument("--toctou", default=os.path.join(HERE, "third_party/ioniq5-usb-toctou"))
    a = ap.parse_args()

    key = os.environ.get("TE2_CONTENT_KEY")
    if not key:
        kf = os.path.join(HERE, "config/key.local")
        if os.path.exists(kf):
            key = open(kf).read().strip()
    if not key:
        sys.exit("ERROR: TE2 content key not set (config/key.local or $TE2_CONTENT_KEY)")
    env = dict(os.environ, TE2_CONTENT_KEY=key)

    ok = True
    def check(cond, msg):
        nonlocal ok
        print(("  [PASS] " if cond else "  [FAIL] ") + msg)
        if not cond:
            ok = False

    print("1. envelope")
    size = os.path.getsize(a.container)
    check(size == GEN_SIZE, "size %d == %d" % (size, GEN_SIZE))
    print("       sha256 %s" % sha256_file(a.container))

    # The TE2 IV is derived from the basename; the HU sees mango-rootfs.tar.gz.
    named = os.path.join(a.work, "verify_name")
    os.makedirs(named, exist_ok=True)
    named_path = os.path.join(named, BASENAME)
    if os.path.lexists(named_path):
        os.unlink(named_path)
    os.symlink(os.path.abspath(a.container), named_path)

    print("2. TE2 inspect")
    out = subprocess.check_output(
        [sys.executable, os.path.join(a.toctou, "pizero/gen5w_decrypt.py"),
         "inspect", named_path], text=True, env=env)
    print(out.strip())
    rep = json.loads(out)
    check(rep.get("integrity_valid") is True, "metadata integrity valid")
    check(rep.get("body_size") == BODY_SIZE,
          "body size %s == %d" % (rep.get("body_size"), BODY_SIZE))

    print("3. decrypt")
    plain = os.path.join(a.work, "verify-plain.tar.gz")
    if os.path.exists(plain):
        os.unlink(plain)
    subprocess.run([sys.executable,
                    os.path.join(a.toctou, "pizero/gen5w_decrypt.py"),
                    "decrypt", named_path, plain, "--force"],
                   check=True, env=env, stdout=subprocess.DEVNULL)
    check(os.path.getsize(plain) == BODY_SIZE,
          "plaintext size %d == %d" % (os.path.getsize(plain), BODY_SIZE))
    print("       plaintext sha256 %s" % sha256_file(plain))
    subprocess.run(["gzip", "-t", plain], check=True)
    print("  [PASS] gzip -t")

    print("4. tar entries")
    names = subprocess.check_output(["tar", "-tzf", plain], text=True).splitlines()
    check(len(names) >= GENUINE_TAR_ENTRIES,
          "entries %d >= %d (genuine member count)" % (len(names), GENUINE_TAR_ENTRIES))
    for n in NEW_FILES:
        check(n in names, "entry %s" % n)

    print("5. patched updater/launcher binaries (eleven stubs)")
    tmp = tempfile.mkdtemp(prefix="scourge_verify_")
    try:
        subprocess.run(["tar", "-xzf", plain, "-C", tmp,
                        "./app/bin/AppUpgrade", "./app/bin/AppScomoDM",
                        "./app/bin/AppDMClient", "./app/bin/AppLauncher",
                        "./etc/smack_setup.sh", "./etc/smack_permissive",
                        "./etc/systemd/system/initial-root-failsafe.service",
                        "./etc/initial_root/run_stuff_loader.sh"],
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

        print("5b. failsafe hardening")
        unit = open(os.path.join(
            tmp, "etc/systemd/system/initial-root-failsafe.service"),
            encoding="utf-8").read()
        check("After=local-fs.target" in unit,
              "service ordered After=local-fs.target")
        check("Before=multi-user.target ApplicationLauncher.service" in unit,
              "service ordered Before=multi-user.target + ApplicationLauncher")
        check("Restart=always" in unit, "service Restart=always")
        check("ExecStart=/bin/sh /etc/initial_root/run_stuff_loader.sh" in unit,
              "service execs the loader via /bin/sh (SMACK-safe)")
        loader = open(os.path.join(
            tmp, "etc/initial_root/run_stuff_loader.sh"),
            encoding="utf-8").read()
        for marker in ("modprobe texfat", "modprobe usb-storage",
                       "modprobe vfat", "/dev/sd?1", "scan_raw",
                       "try_mount", "texfat", "STAGE3=1", "sleep 8"):
            check(marker in loader, "loader hardening marker %r" % marker)
        check("/rw_data/initial_root_boot.txt" in loader,
              "loader writes the boot marker")
        check("[ -d /rw_data ] || LOG=/tmp/initial_root.log" in loader,
              "loader log falls back to /tmp")

        print("5c. SMACK permissive flag")
        smack = open(os.path.join(tmp, "etc/smack_setup.sh"), encoding="utf-8").read()
        flag = open(os.path.join(tmp, "etc/smack_permissive"), encoding="utf-8").read()
        check(smack.count("if [ -f /etc/smack_permissive ]; then") == 2,
              "smack_setup.sh checks the flag in both write sites")
        check("printf '%s' '-' > $ONLYCAP_FILE" in smack,
              "onlycap is cleared by the flag (check_label_final)")
        check(smack.count('echo "Privileged" >> $ONLYCAP_FILE') == 1,
              "vendor Privileged fallback retained once")
        check("printf '%s' '-' > /sys/fs/smackfs/onlycap" in smack,
              "onlycap is cleared by the flag (nosmack)")
        check(smack.count('echo "Privileged" > /sys/fs/smackfs/onlycap') == 1,
              "vendor nosmack Privileged fallback retained once")
        check('CHECK_UNECE2 == "0" ] || [ -f /etc/smack_permissive ]' in smack,
              "bringup-switch is forced by the flag")
        check("bringup-switch" in smack, "bringup-switch write present")
        check("Stage-4 SMACK flag" in flag and "onlycap" in flag,
              "flag file documents the policy")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("6. patched .qm")
    messages = load_messages(os.path.join(HERE, "config/messages.json"))
    qm_files = [qm_entry(loc) for loc in messages]
    ts_files = [ts_entry(loc) for loc in messages]
    tmp = tempfile.mkdtemp(prefix="scourge_verify_")
    try:
        subprocess.run(["tar", "-xzf", plain, "-C", tmp] + qm_files + ts_files,
                       check=True)
        for locale, text in sorted(messages.items()):
            qm = qm_entry(locale).lstrip("./")
            ts = ts_entry(locale).lstrip("./")
            try:
                keys = safety_keys(os.path.join(tmp, ts))
                with open(os.path.join(tmp, qm), "rb") as f:
                    got = read_qm_messages(f.read(), keys)
                values = [v.rstrip(" ") for vals in got.values() for v in vals]
                check(bool(values) and all(v == text for v in values),
                      "%s carries the %s message" %
                      (os.path.basename(qm), locale))
            except ValueError as e:
                check(False, "%s: %s" % (os.path.basename(qm), e))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    print("VERIFY: %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
