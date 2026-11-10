#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Content identity for first-party inputs; no timestamps, credentials or OEM data."""
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def build_id():
    paths = [ROOT / "src/Makefile"]
    for directory, suffixes in [("src", {".c", ".h"}), ("scripts", {".py", ".sh"}),
                                ("artifacts/systemd", {".service"})]:
        paths.extend(p for p in (ROOT / directory).rglob("*") if p.suffix in suffixes)
    qml = ROOT / "artifacts/qml/AppHomeScreen/qml_eco3.0"
    paths.append(qml / "HomeScreen/WifiPage.qml")
    paths.extend((qml / "component").glob("Wifi*.qml"))
    digest = hashlib.sha256()
    for path in sorted(set(paths)):
        digest.update(str(path.relative_to(ROOT)).encode() + b"\0" + path.read_bytes() + b"\0")
    return digest.hexdigest()


if __name__ == "__main__":
    print(build_id())
