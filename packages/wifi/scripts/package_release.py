#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Package the current staging tree atomically, and verify its build identity."""
import hashlib
import json
import os
from pathlib import Path
import sys
import zipfile

from build_id import build_id

tree = Path(sys.argv[1]).resolve()
identity = build_id()
embedded = (tree / "payload/bin/wifi_daemon").read_bytes()
if identity.encode() not in embedded:
    raise SystemExit("daemon build identity does not match current source inputs")
(tree / "payload/build-id.txt").write_text(identity + "\n")
metadata = {"build_id": identity, "validation": "built; hardware/car validation pending",
            "sha256": {str(p.relative_to(tree)): hashlib.sha256(p.read_bytes()).hexdigest()
                       for p in sorted(tree.rglob("*")) if p.is_file()
                       and p.name not in {"manifest.md5", "build.json"}}}
(tree / "payload/build.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
if "--metadata-only" in sys.argv:
    raise SystemExit(0)
archive = tree.parent / "wifi_app_usb.zip"
temporary = archive.with_suffix(".zip.tmp")
with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_DEFLATED) as output:
    for path in sorted(tree.rglob("*")):
        if path.is_file():
            entry = zipfile.ZipInfo("usb/" + str(path.relative_to(tree)), (2026, 1, 1, 0, 0, 0))
            entry.external_attr = (0o100755 if os.access(path, os.X_OK) else 0o100644) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            output.writestr(entry, path.read_bytes())
os.replace(temporary, archive)
print(f"Release build {identity}: {archive}")
