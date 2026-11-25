#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Verify the Wifi1b staged tree, manifest, metadata and ZIP agree."""
import hashlib
import json
import os
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from build_id import build_id

tree = ROOT / "out/usb"
if not (tree / "payload/build-id.txt").is_file() or not (ROOT / "out/wifi1b_usb.zip").is_file():
    print("SKIP: Wifi1b release artifacts absent in this source-only worktree; "
          "run scripts/build_payload.sh and rerun this release gate")
    raise SystemExit(0)
identity = (tree / "payload/build-id.txt").read_text().strip()
assert identity == build_id(), (identity, build_id())
metadata = json.loads((tree / "payload/build.json").read_text())
assert metadata["build_id"] == identity and metadata["app"] == "wifi1b"
daemon = (tree / "payload/bin/wifi1b_daemon").read_bytes()
assert daemon == (ROOT / "src/wifi1b_daemon").read_bytes() and identity.encode() in daemon
for name, expected in metadata["sha256"].items():
    assert hashlib.sha256((tree / name).read_bytes()).hexdigest() == expected, name
for line in (tree / "manifest.md5").read_text().splitlines():
    expected, name = line.split("  ", 1)
    assert hashlib.md5((tree / name).read_bytes()).hexdigest() == expected, name
with zipfile.ZipFile(ROOT / "out/wifi1b_usb.zip") as archive:
    files = {"usb/" + str(p.relative_to(tree)): p for p in tree.rglob("*") if p.is_file()}
    assert set(archive.namelist()) == set(files)
    for name, path in files.items():
        assert archive.read(name) == path.read_bytes(), name
    blob = b"".join(archive.read(n) for n in archive.namelist())
    # Synthetic fixture only: never embed an owner's credential, even as a
    # negative assertion. Owner inputs belong outside the release tree.
    assert b"Sup3rSecretPass" not in blob
ledger = set((tree / "payload/accepted-qml.md5").read_text().split())
for line in (ROOT / "artifacts/accepted-qml-legacy.md5").read_text().splitlines():
    line = line.strip()
    if len(line) == 32 and all(c in "0123456789abcdef" for c in line):
        assert line in ledger, f"legacy V11 QML digest missing from ledger: {line}"
cleanup = tree / "payload/lib/runtime_cleanup.sh"
assert cleanup.read_bytes() == (ROOT.parent / "common/runtime_cleanup.sh").read_bytes()
# Deterministic content stamp: no timestamp/PID, and it changes with payload.
lines = b""
paths = sorted((p for p in (tree / "payload").rglob("*") if p.is_file()
                and p.name not in {"build.json", "build-id.txt"}),
               key=lambda p: os.fsencode(str(p.relative_to(tree))))
for path in paths:
    rel = str(path.relative_to(tree))
    lines += hashlib.sha256(path.read_bytes()).hexdigest().encode() + b"  " + \
        rel.encode() + b"\n"
payload_digest = hashlib.sha256(lines).hexdigest()
for name in ("run_stuff.sh", "run_stuff_restore.sh"):
    text = (tree / name).read_text()
    assert f"# WIFI1B_PAYLOAD_DIGEST={payload_digest}" in text, name
    assert "WIFI1B_STANDALONE_STAMP=20" not in text, name
    assert "WIFI1B_RESTORE_STAMP=20" not in text, name
print(f"PASS: Wifi1b source/build identity, staged bytes, manifest, metadata, cleanup helper, deterministic payload stamp and ZIP agree ({identity[:12]})")
