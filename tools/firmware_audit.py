#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""firmware_audit.py — explain package-map / hash-pin disagreements.

Read-only and stdlib-only. Given an official package tree (or the four files
staged by 01_stage_inputs.sh), compare:

  * the ``.ver`` header and rows,
  * the files the rows describe,
  * the pins in ``config/firmware_locks.env``.

The point is to say *which side* is wrong when a setup step warns, instead of
only printing "hash mismatch":

  row == file, row != lock  -> the lock pin is stale/wrong
  row == lock, row != file  -> the package does not match its own map
  row != file               -> corrupt or mixed package

The audit never decrypts, patches or executes anything. A package-map
revision that differs only in header bookkeeping/padding (the HMG server's
package-id counter moves between repacks) is a warning, not an error, as long
as the header identity and every checked row agree with the files.
"""
from __future__ import annotations

import hashlib
import json
import shlex
import sys
import zlib
from pathlib import Path

# The .ver grammar is shared with the VM update-media checker. Importing it
# keeps one parser instead of two subtly different ones.
sys.path.insert(0, str(Path(__file__).resolve().parent / "vm" / "harness"))
from update_media import parse_ver  # noqa: E402

LAYOUTS = ("package", "staged")

# layout -> (rootfs, wrapper, xml) relative paths
LAYOUT_PATHS = {
    "package": ("HU/images/mango-rootfs.tar.gz", "AppUpgrade", ".lge.upgrade.xml"),
    "staged": ("mango-rootfs.tar.gz", "AppUpgrade.usb", ".lge.upgrade.xml"),
}


def load_locks(path: Path) -> dict[str, str]:
    """Read KEY=VALUE pairs from firmware_locks.env (same rules as the shell)."""
    locks: dict[str, str] = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        fields = shlex.split(line, comments=True)
        if fields and "=" in fields[0]:
            key, value = fields[0].split("=", 1)
            locks[key] = value
    return locks


def signed32(value: int) -> int:
    return value - (1 << 32) if value >= (1 << 31) else value


def digests(path: Path, want_md5: bool = False) -> dict:
    """One pass over a file: size, signed CRC32, sha256 and optional md5."""
    crc = 0
    sha = hashlib.sha256()
    md5 = hashlib.md5() if want_md5 else None
    size = 0
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(8 * 1024 * 1024), b""):
            size += len(chunk)
            crc = zlib.crc32(chunk, crc)
            sha.update(chunk)
            if md5 is not None:
                md5.update(chunk)
    return {
        "size": size,
        "crc32": signed32(crc),
        "sha256": sha.hexdigest(),
        "md5": md5.hexdigest() if md5 is not None else None,
    }


def _row_by_id(entries: list[dict], row_id: str | None) -> tuple[dict | None, int]:
    if row_id is None:
        return None, 0
    matches = [entry for entry in entries if str(entry.get("id")) == str(row_id)]
    return (matches[0] if len(matches) == 1 else None), len(matches)


def _row_by_name(entries: list[dict], filename: str) -> tuple[dict | None, int]:
    matches = [entry for entry in entries if entry.get("filename") == filename]
    return (matches[0] if len(matches) == 1 else None), len(matches)


def audit(root, locks: dict[str, str], layout: str = "package",
          ver_file: str | None = None, strict: bool = False,
          all_rows: bool = False) -> dict:
    """Audit one package/staging tree. Return findings plus counts.

    ``ok`` is False when any error exists (or, with ``strict``, any warning).
    ``all_rows`` additionally verifies every non-consumed row against its
    file (slow: the map covers the whole ~24 GB package).
    """
    root = Path(root)
    if layout not in LAYOUTS:
        raise ValueError(f"unknown layout: {layout}")
    ver_name = ver_file or locks.get("VER_FILE", "IONIQ5_EU.ver")
    findings: list[dict] = []

    def add(level: str, check: str, detail: str) -> None:
        findings.append({"level": level, "check": check, "detail": detail})

    def fail(check: str, detail: str) -> None:
        add("error", check, detail)

    result = {
        "root": str(root),
        "layout": layout,
        "ver": str(root / ver_name),
        "header": {},
        "rows": {},
        "findings": findings,
        "errors": 0,
        "warnings": 0,
        "ok": False,
    }

    ver_path = root / ver_name
    if not ver_path.is_file():
        fail("ver.present", f"missing package map: {ver_path}")
        result["errors"] = 1
        return result

    raw = ver_path.read_bytes()
    ver_sha = hashlib.sha256(raw).hexdigest()
    parsed = parse_ver(raw.decode("utf-8", errors="replace"))
    result["header"] = parsed["header"]
    for problem in parsed["problems"]:
        fail("ver.grammar", problem)

    header = parsed["header"]
    expected_product = Path(ver_name).stem
    if header:
        if header.get("version") != locks.get("SCOURGE_FIRMWARE_ID"):
            fail("ver.version",
                 f"package map version {header.get('version')!r} != pinned "
                 f"{locks.get('SCOURGE_FIRMWARE_ID')!r}")
        if locks.get("SCOURGE_BRAND") and \
                header.get("brand") != locks.get("SCOURGE_BRAND"):
            fail("ver.brand",
                 f"package map brand {header.get('brand')!r} != pinned "
                 f"{locks.get('SCOURGE_BRAND')!r}")
        if header.get("product") != expected_product:
            fail("ver.product",
                 f"package map product {header.get('product')!r} != file-name "
                 f"product {expected_product!r}")

    entries = parsed["entries"]
    rootfs_path = root / LAYOUT_PATHS[layout][0]
    wrapper_path = root / LAYOUT_PATHS[layout][1]
    xml_path = root / LAYOUT_PATHS[layout][2]

    checks = (
        ("rootfs", rootfs_path,
         _row_by_id(entries, locks.get("VER_ROOTFS_ROW_ID"))[0],
         _row_by_id(entries, locks.get("VER_ROOTFS_ROW_ID"))[1],
         locks.get("VER_ROOTFS_ROW_STOCK_CRC"),
         locks.get("GENUINE_ENVELOPE_SIZE"),
         locks.get("GENUINE_MD5"),
         locks.get("VER_ROOTFS_ROW_ID"), "VER_ROOTFS_ROW_STOCK_CRC",
         "GENUINE_ENVELOPE_SIZE", True),
        ("wrapper", wrapper_path,
         _row_by_id(entries, locks.get("VER_WRAPPER_ROW_ID"))[0],
         _row_by_id(entries, locks.get("VER_WRAPPER_ROW_ID"))[1],
         locks.get("VER_WRAPPER_ROW_STOCK_CRC"),
         locks.get("WRAPPER_SIZE"),
         locks.get("WRAPPER_STOCK_SHA256"),
         locks.get("VER_WRAPPER_ROW_ID"), "VER_WRAPPER_ROW_STOCK_CRC",
         "WRAPPER_SIZE", False),
        ("xml", xml_path,
         _row_by_name(entries, ".lge.upgrade.xml")[0],
         _row_by_name(entries, ".lge.upgrade.xml")[1],
         None, None,
         locks.get("LGE_UPGRADE_XML_SHA256"),
         ".lge.upgrade.xml", None, None, False),
    )

    table_complete = True
    for label, path, row, row_count, lock_crc, lock_size, lock_hash, \
            row_label, crc_label, size_label, want_md5 in checks:
        if row_count != 1:
            fail(f"{label}.row",
                 f"expected exactly one package-map row for {label}, "
                 f"found {row_count} ({row_label})")
            table_complete = False
            continue
        if not path.is_file():
            fail(f"{label}.present", f"missing {label} file: {path}")
            table_complete = False
            continue
        got = digests(path, want_md5=want_md5)
        result["rows"][label] = got | {"declared_crc": row.get("crc"),
                                       "declared_size": row.get("declared_size")}
        consistent = True
        try:
            declared_crc = int(row["crc"])
            declared_size = int(row["declared_size"])
        except (KeyError, TypeError, ValueError):
            fail(f"{label}.row", f"malformed {label} row: {row}")
            table_complete = False
            continue
        if declared_size != got["size"]:
            fail(f"{label}.size",
                 f"package map says {declared_size} B, file is {got['size']} B")
            consistent = False
        if declared_crc != got["crc32"]:
            fail(f"{label}.crc",
                 f"package map CRC32 is {declared_crc}, file CRC32 is "
                 f"{got['crc32']} -> the package is not self-consistent")
            consistent = False
        if not consistent:
            table_complete = False
            continue
        if lock_crc is not None and int(lock_crc) != declared_crc:
            fail(f"{label}.lock",
                 f"package map and file agree on CRC32 {declared_crc}, but "
                 f"{crc_label}={lock_crc} -> the lock pin is stale/wrong; "
                 f"update it to {declared_crc}")
            table_complete = False
        else:
            add("ok", f"{label}.crc",
                f"row {row.get('id')} CRC32 {declared_crc} / {got['size']} B")
        if lock_size is not None and int(lock_size) != got["size"]:
            fail(f"{label}.lock-size",
                 f"file size {got['size']} B != {size_label}={lock_size}; "
                 f"this is not the pinned payload")
            table_complete = False
        if lock_hash:
            if want_md5:
                actual_hash, kind = got["md5"], "md5"
            else:
                actual_hash, kind = got["sha256"], "sha256"
            if actual_hash != lock_hash:
                if label == "xml":
                    fail(f"{label}.lock-hash",
                         f"{kind} {actual_hash} != pinned {lock_hash}; the "
                         f"signed XML must stay stock (the stage script "
                         f"hard-stops; do not treat it as a repack)")
                else:
                    add("warn", f"{label}.lock-hash",
                        f"{kind} {actual_hash} != pinned {lock_hash}; rows are "
                        f"consistent (package-map variant) - recording it")

    if all_rows:
        consumed = {(row.get("path"), row.get("filename"))
                    for _label, _path, row, *_rest in checks if row}
        for entry in entries:
            if (entry.get("path"), entry.get("filename")) in consumed:
                continue
            relative = entry.get("relative")
            if not relative:
                continue
            path = root / relative
            if not path.is_file():
                add("warn", "all-rows.present",
                    f"missing {relative} (not consumed by SCOURGE builds)")
                continue
            got = digests(path)
            try:
                declared_crc = int(entry["crc"])
                declared_size = int(entry["declared_size"])
            except (KeyError, TypeError, ValueError):
                fail("all-rows.row", f"malformed row: {entry}")
                continue
            if declared_size != got["size"] or declared_crc != got["crc32"]:
                fail("all-rows.crc",
                     f"{relative}: map says {declared_crc}/{declared_size}, "
                     f"file is {got['crc32']}/{got['size']}")
            else:
                add("ok", "all-rows.crc",
                    f"{relative}: {got['crc32']}/{got['size']}")

    # .ver file itself: exact hash is a fast path, not the identity check.
    pinned_ver_sha = locks.get("VER_STOCK_SHA256")
    if pinned_ver_sha and ver_sha == pinned_ver_sha:
        add("ok", "ver.sha", f"package map sha256 {ver_sha[:16]}... matches pin")
    elif pinned_ver_sha:
        if table_complete and not any(f["level"] == "error" for f in findings):
            add("warn", "ver.sha",
                f"package map sha256 {ver_sha[:16]}... != pin "
                f"{pinned_ver_sha[:16]}... but header identity and rows verify "
                f"(server-side package-id/padding revision)")
        else:
            fail("ver.sha",
                 f"package map sha256 {ver_sha} != pin {pinned_ver_sha}, and "
                 f"row checks did not verify")

    pinned_size = locks.get("VER_STOCK_SIZE")
    if pinned_size:
        pinned_size = int(pinned_size)
        if len(raw) < pinned_size:
            fail("ver.size",
                 f"package map is {len(raw)} B, shorter than the engine's "
                 f"pinned record area {pinned_size} B")
        elif len(raw) != pinned_size:
            add("warn", "ver.size",
                f"package map is {len(raw)} B, pin {pinned_size} B; longer is "
                f"safe (the engine reads the fixed record area)")

    result["errors"] = sum(1 for f in findings if f["level"] == "error")
    result["warnings"] = sum(1 for f in findings if f["level"] == "warn")
    result["ok"] = result["errors"] == 0 and not (strict and result["warnings"])
    return result


def format_report(result: dict) -> str:
    lines = [f"Firmware audit: {result['root']} ({result['layout']})"]
    for finding in result["findings"]:
        lines.append(f"  [{finding['level'].upper():5}] "
                     f"{finding['check']}: {finding['detail']}")
    lines.append(f"Result: {result['errors']} error(s), "
                 f"{result['warnings']} warning(s)")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default="input",
                        help="package tree or staged input directory")
    parser.add_argument("--locks", default="config/firmware_locks.env")
    parser.add_argument("--layout", choices=LAYOUTS, default="package")
    parser.add_argument("--ver-file", default=None)
    parser.add_argument("--strict", action="store_true",
                        help="treat warnings as errors")
    parser.add_argument("--all-rows", action="store_true",
                        help="also verify every non-consumed .ver row (slow)")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    locks = load_locks(Path(args.locks))
    result = audit(Path(args.root), locks, layout=args.layout,
                   ver_file=args.ver_file, strict=args.strict,
                   all_rows=args.all_rows)
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        print(format_report(result))
    return 0 if result["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
