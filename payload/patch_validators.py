#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Apply the updater/launcher validator stubs (build 4, four binaries).

TARGET: Hyundai Ioniq 5 Gen5W_L (Linux), EU,
        firmware NE_E.EUR.S5W_L.001.001.260128.

Four binaries are patched. Every application first verifies the target's
size AND sha256 against the pinned values (the ELF-hash rule — a same-size
binary from a different build is NOT the same binary), then verifies the
expected stock bytes at each offset. ANY mismatch aborts — never
best-effort a patch.

  /app/bin/AppUpgrade   (10,787,256 B) — installed updater engine
    FUN_002f5a00  2086500  final hash/signature verifier
    FUN_002f5b60  2086852  helper calling FUN_002f5a00
    FUN_002f5ba0  2086916  TE2 verifier wrapper
    FUN_002f5c40  2087076  second verifier wrapper

  /app/bin/AppScomoDM   (1,934,752 B) — UsbUpdateManager
    FUN_001e3430  930864   final verify
    FUN_001e3470  930928   shared verifier entry

  /app/bin/AppDMClient  (2,226,072 B) — checkUSBUpdate / the REAL button gate
    FUN_00221000  1183744  final verify
    FUN_00221040  1183808  shared verifier entry

  /app/bin/AppLauncher  (407,720 B) — .sign gate for preloaded binaries
    FUN_00115b50  88912    readSection gate
    FUN_00112100  73984    navi CheckSign

The updater/verifier gates are false-on-error: 31 C0 C3 = xor eax,eax; ret
(return 0 = success). The AppLauncher gates are true-on-success:
B8 01 00 00 00 C3 = mov eax,1; ret — note the different polarity.

Usage:
  patch_validators.py SRC DST [--kind appupgrade|scomo|dmclient|launcher]
  patch_validators.py verify SRC [--kind ...]     # report state only
"""
import argparse
import hashlib
import sys

STUB = b"\x31\xc0\xc3"
LAUNCHER_STUB = b"\xb8\x01\x00\x00\x00\xc3"

# name, (size, patches, stub, label, stock_sha256, patched_sha256)
KINDS = {
    "appupgrade": (
        10787256,
        [
            ("FUN_002f5a00", 2086500, bytes.fromhex("415455534883ec50")),
            ("FUN_002f5b60", 2086852, bytes.fromhex("4883ec1864488b04")),
            ("FUN_002f5ba0", 2086916, bytes.fromhex("554889f531f65348")),
            ("FUN_002f5c40", 2087076, bytes.fromhex("554889f5534883ec")),
        ],
        STUB, "AppUpgrade",
        "5295c6e4c41fb9c03760cf006c58b66fcd11aeb4e6e889af6e31b062549b6df0",
        "5d8422126e5df937dbd7e69aab4652ff8188640a229a6c6f9b674a9da008dc01",
    ),
    "scomo": (
        1934752,
        [
            ("FUN_001e3430", 930864, bytes.fromhex("4883ec1864488b0425")),
            ("FUN_001e3470", 930928, bytes.fromhex("554889f531f65348")),
        ],
        STUB, "AppScomoDM",
        "c44da21eb39987e88d71c648f625338722b8458f0ff8ecfe29e3321cf6d62046",
        "b2467b2b7cdf18b4e470b36083867411cae3b9d047739959141b333e5fa1e596",
    ),
    "dmclient": (
        2226072,
        [
            ("FUN_00221000", 0x121000, bytes.fromhex("4883ec1864488b0425")),
            ("FUN_00221040", 0x121040, bytes.fromhex("554889f531f65348")),
        ],
        STUB, "AppDMClient",
        "9a1fb796d7ac24fcce71c16e3dbee9306c888f43e705ee081f41b7d473978bd0",
        "e59259a16d3025d07a9f790a07e5eb4837df5cf69b05fcb2b3a64b281221fba5",
    ),
    "launcher": (
        407720,
        [
            ("FUN_00115b50", 88912, bytes.fromhex("4157be1000000041")),
            ("FUN_00112100", 73984, bytes.fromhex("4157415641554154")),
        ],
        LAUNCHER_STUB, "AppLauncher",
        "c8d6d56bc254ad0731e81f40bd83aa9b3be6b49bcbd8efb291157a06c4a84268",
        "d00efd1363dcaef2c3c5b3c5aefc01d0eea99dce84d60647ab036ce65113e2e0",
    ),
}

# Convenience aliases used by the build scripts.
EXPECTED_SIZE, PATCHES = KINDS["appupgrade"][0], KINDS["appupgrade"][1]
SCDM_SIZE, SCDM_PATCHES = KINDS["scomo"][0], KINDS["scomo"][1]
DMCLIENT_SIZE, DMCLIENT_PATCHES = KINDS["dmclient"][0], KINDS["dmclient"][1]
LAUNCHER_SIZE, LAUNCHER_PATCHES = KINDS["launcher"][0], KINDS["launcher"][1]


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def _apply(data, size, patches, stub, label, stock_sha=None, patched_sha=None):
    """Patch a bytearray in place. Idempotent; hash-gated."""
    if len(data) != size:
        raise SystemExit("unexpected %s size: %d (want %d)" %
                         (label, len(data), size))
    if stock_sha:
        h = sha256_bytes(data)
        if h != stock_sha:
            # Accept a fully-patched binary (idempotent re-run) but nothing
            # else: an ELF that matches neither known hash is not the one
            # these offsets were derived for (docs/09, the ELF-hash rule).
            if patched_sha and h == patched_sha:
                pass  # already fully patched; the loop below no-ops
            else:
                raise SystemExit(
                    "%s sha256 %s matches neither the pinned stock hash nor "
                    "the known patched build; refusing to patch (see "
                    "config/firmware_locks.env and docs/09-porting.md)" %
                    (label, h))
    for name, off, expect in patches:
        cur = bytes(data[off:off + len(expect)])
        if cur[:len(stub)] == stub:
            print("%s: already patched at %d" % (name, off))
            continue
        if cur != expect:
            raise SystemExit("%s: unexpected bytes at %d: %s (want %s)" %
                             (name, off, cur.hex(), expect.hex()))
        data[off:off + len(stub)] = stub
        print("%s: patched at %d: %s -> %s" %
              (name, off, cur.hex(), bytes(data[off:off + len(expect)]).hex()))
    if patched_sha:
        got = sha256_bytes(data)
        if got != patched_sha:
            raise SystemExit("patched %s sha256 mismatch: %s (want %s)" %
                            (label, got, patched_sha))
    return data


def apply(data):
    return _apply(data, *KINDS["appupgrade"][:4], *KINDS["appupgrade"][4:])


def apply_scomo(data):
    return _apply(data, *KINDS["scomo"][:4], *KINDS["scomo"][4:])


def apply_dmclient(data):
    return _apply(data, *KINDS["dmclient"][:4], *KINDS["dmclient"][4:])


def apply_launcher(data):
    return _apply(data, *KINDS["launcher"][:4], *KINDS["launcher"][4:])


def state_of(data, patches, stub):
    seen = set()
    for _name, off, expect in patches:
        cur = bytes(data[off:off + len(expect)])
        if cur[:len(stub)] == stub:
            seen.add("patched")
        elif cur == expect:
            seen.add("stock")
        else:
            return "unknown-bytes@%d" % off
    return seen.pop() if len(seen) == 1 else "mixed"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst", nargs="?")
    ap.add_argument("--kind", choices=sorted(KINDS), default="appupgrade")
    ap.add_argument("--verify", action="store_true",
                    help="report stock/patched state only")
    a = ap.parse_args()

    size, patches, stub, label, stock_sha, patched_sha = KINDS[a.kind]
    data = bytearray(open(a.src, "rb").read())

    if a.verify:
        if len(data) != size:
            print("%s: wrong size" % label)
            return 1
        h = sha256_bytes(data)
        known = "stock" if h == stock_sha else \
                ("patched" if h == patched_sha else "unknown-hash")
        print("%s: %s (%s)" % (label, known, state_of(data, patches, stub)))
        return 0

    if not a.dst:
        raise SystemExit("SRC DST required (or --verify SRC)")
    _apply(data, size, patches, stub, label, stock_sha, patched_sha)
    open(a.dst, "wb").write(data)
    print("%s: wrote %s" % (label, a.dst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
