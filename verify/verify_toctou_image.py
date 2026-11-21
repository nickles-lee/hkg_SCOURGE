#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""verify_toctou_image.py — the swap invariants, checked offline.

    python3 verify/verify_toctou_image.py \
        --image out/usb.img --regions out/regions.json \
        --genuine input/mango-rootfs.tar.gz --envelope out/stage1.te2

What it proves, without a rig and without a car:

* the region in `regions.json` lands exactly on the genuine rootfs container
  inside the served image — same offset, same length, same bytes (sha256);
* the envelope it swaps in has the same length but different bytes, so pass #1
  serves genuine and pass #2 does not;
* the plugin's pass arithmetic says `benign_then_evil` with
  `switch_after_pass = 2`, which is what the engine's two reads need.

A failure here is a wasted drive to the car, so it runs in 09_preflight.sh.
The `.ver` row CRC deliberately is not part of the check: docs/01 establishes
from the engine's own command templates that no install pipeline re-hashes the
rootfs, and a CRC error at install is not a mechanism this flow can produce.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys


def sha256_range(path: str, offset: int, length: int) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        handle.seek(offset)
        left = length
        while left > 0:
            chunk = handle.read(min(left, 1 << 22))
            if not chunk:
                raise SystemExit(f"ERROR: {path} ended {length - left} bytes into the region")
            digest.update(chunk)
            left -= len(chunk)
    return digest.hexdigest()


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 22), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True)
    parser.add_argument("--regions", required=True)
    parser.add_argument("--genuine", required=True)
    parser.add_argument("--envelope", required=True)
    args = parser.parse_args()

    for path in (args.image, args.regions, args.genuine, args.envelope):
        if not os.path.isfile(path):
            print(f"ERROR: missing {path}")
            return 2

    failed = False

    def check(ok: bool, label: str) -> None:
        nonlocal failed
        failed = failed or not ok
        print("  [%s] %s" % ("PASS" if ok else "FAIL", label))

    regions = json.load(open(args.regions))
    check(isinstance(regions, list) and len(regions) == 1,
          "exactly one swap region (got %d)" % (len(regions) if isinstance(regions, list) else -1))
    if not regions:
        print("  row: %s" % json.dumps(regions))
        return 1
    region = regions[0]

    genuine_size = os.path.getsize(args.genuine)
    envelope_size = os.path.getsize(args.envelope)
    image_size = os.path.getsize(args.image)
    offset, length = int(region.get("off", -1)), int(region.get("len", -1))

    check(region.get("mode") == "benign_then_evil",
          "mode is benign_then_evil (got %r)" % region.get("mode"))
    check(int(region.get("switch_after_pass", -1)) == 2,
          "switch_after_pass is 2 (got %r)" % region.get("switch_after_pass"))
    check(length == genuine_size,
          "region length %d == genuine envelope %d" % (length, genuine_size))
    check(envelope_size == genuine_size,
          "swapped envelope is the same length (%d) — a different length cannot land here"
          % envelope_size)
    check(0 <= offset and offset + length <= image_size,
          "region [%d,%d) fits the image (%d)" % (offset, offset + length, image_size))
    check(region.get("evil") is not None, "region names the envelope it swaps in")

    genuine_digest = sha256_file(args.genuine)
    envelope_digest = sha256_file(args.envelope)
    check(genuine_digest != envelope_digest,
          "swapped envelope differs from the genuine container (otherwise the swap is a no-op)")

    if 0 <= offset and offset + length <= image_size:
        region_digest = sha256_range(args.image, offset, length)
        check(region_digest == genuine_digest,
              "served region is the genuine container (%s)" % region_digest[:16])
    else:
        check(False, "region could not be read from the image")

    print("       region: %s" % json.dumps(region))
    print("       genuine sha256: %s" % genuine_digest[:32])
    print("       envelope sha256: %s" % envelope_digest[:32])
    if failed:
        print("\nTOCTOU IMAGE: FAIL")
        return 1
    print("\nTOCTOU IMAGE: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
