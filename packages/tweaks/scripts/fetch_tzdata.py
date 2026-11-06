#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Fetch and compile the latest IANA tzdata for the Tweaks payload.

Outputs:
  build/tzdata/zoneinfo/     compiled TZif tree
  build/tzdata/zones.txt     canonical zone names for the Tweaks picker
  build/tzdata/version.txt   IANA version and SHA-256 of the tarball

The tarball is cached.  Set TZDATA_TARBALL to use a local archive, or
TZDATA_REFRESH=1 to force a new download.
"""
from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build", "tzdata")
URL = "https://data.iana.org/time-zones/tzdata-latest.tar.gz"
CACHE_MAX_AGE = 0
TZDATA_FILES = [
    "africa", "antarctica", "asia", "australasia", "europe",
    "northamerica", "southamerica", "etcetera", "backward", "factory",
]


def log(msg: str) -> None:
    print(f"[tzdata] {msg}")


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def download(dest: str) -> None:
    local = os.environ.get("TZDATA_TARBALL")
    if local:
        log(f"using local tarball {local}")
        shutil.copyfile(local, dest)
        return
    if os.path.exists(dest) and not os.environ.get("TZDATA_REFRESH"):
        age = time.time() - os.path.getmtime(dest)
        if age < CACHE_MAX_AGE:
            log(f"using cached tarball ({age / 3600:.1f}h old)")
            return
    log(f"downloading {URL}")
    tmp = dest + ".part"
    try:
        with urllib.request.urlopen(URL, timeout=60) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f)
        os.replace(tmp, dest)
    except Exception as e:
        if os.path.exists(dest):
            log(f"download failed ({e}); using cached tarball")
            return
        raise SystemExit(f"cannot download tzdata: {e}")


def safe_extract(tar: tarfile.TarFile, dest: str) -> None:
    base = os.path.abspath(dest)
    for member in tar.getmembers():
        target = os.path.abspath(os.path.join(dest, member.name))
        if not target.startswith(base + os.sep) and target != base:
            raise SystemExit(f"unsafe tar member: {member.name}")
    tar.extractall(dest)


def tzdata_version(src: str) -> str:
    path = os.path.join(src, "tzdata.zi")
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.search(r"version\s+(\S+)", line)
                if m:
                    return m.group(1)
    path = os.path.join(src, "version")
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read().strip()
    return "unknown"


def build_zones(src: str, out: str) -> list[str]:
    zones: list[str] = []
    for name in ("zone1970.tab", "zone.tab"):
        path = os.path.join(src, name)
        if not os.path.exists(path):
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if not line or line.startswith("#"):
                    continue
                fields = line.rstrip("\n").split("\t")
                if len(fields) >= 3:
                    zone = fields[2].strip()
                    if zone and zone not in zones:
                        zones.append(zone)
    for extra in ("UTC", "Etc/UTC"):
        if extra not in zones:
            zones.append(extra)
    zones.sort()
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(zones) + "\n")
    return zones


def main() -> int:
    os.makedirs(BUILD, exist_ok=True)
    tarball = os.path.join(BUILD, "tzdata-latest.tar.gz")
    src = os.path.join(BUILD, "src")
    zoneinfo = os.path.join(BUILD, "zoneinfo")
    zones_txt = os.path.join(BUILD, "zones.txt")
    version_txt = os.path.join(BUILD, "version.txt")

    download(tarball)
    digest = sha256(tarball)
    shutil.rmtree(src, ignore_errors=True)
    os.makedirs(src)
    with tarfile.open(tarball, "r:gz") as tar:
        safe_extract(tar, src)

    version = tzdata_version(src)
    log(f"version {version} sha256 {digest}")

    zic = shutil.which("zic")
    if not zic:
        raise SystemExit("zic is required to compile tzdata")
    shutil.rmtree(zoneinfo, ignore_errors=True)
    os.makedirs(zoneinfo)
    files = [os.path.join(src, name) for name in TZDATA_FILES]
    missing = [p for p in files if not os.path.exists(p)]
    if missing:
        raise SystemExit(f"tzdata source files missing: {missing}")
    subprocess.run([zic, "-b", "fat", "-d", zoneinfo] + files, check=True)

    zones = build_zones(src, zones_txt)
    with open(version_txt, "w", encoding="utf-8") as f:
        f.write(f"{version}\nsha256={digest}\n")
    log(f"compiled {len(zones)} zones into {zoneinfo}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
