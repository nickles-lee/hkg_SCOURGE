# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Tests for tools/firmware_audit.py (synthetic packages, no OEM bytes)."""

from __future__ import annotations

import hashlib
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))

from firmware_audit import audit  # noqa: E402

VERSION = "NE_E.EUR.S5W_L.001.001.260128"
PRODUCT = "IONIQ5_EU"


def crc32(data: bytes) -> int:
    value = zlib.crc32(data)
    return value - (1 << 32) if value >= (1 << 31) else value


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def md5(data: bytes) -> str:
    return hashlib.md5(data).hexdigest()


def write_package(root: Path, *, package_id: str = "1489",
                  header_product: str = PRODUCT) -> dict[str, str]:
    rootfs = b"rootfs-container"
    wrapper = b"wrapper-container"
    xml = b"signed-xml"
    (root / "HU/images").mkdir(parents=True, exist_ok=True)
    (root / "HU/images/mango-rootfs.tar.gz").write_bytes(rootfs)
    (root / "AppUpgrade").write_bytes(wrapper)
    (root / ".lge.upgrade.xml").write_bytes(xml)
    rows = [
        f"{header_product}|AppUpgrade|341142|{crc32(wrapper)}|{len(wrapper)}|1",
        f"{header_product}\\HU\\images|mango-rootfs.tar.gz|341174|"
        f"{crc32(rootfs)}|{len(rootfs)}|1",
        f"{header_product}|.lge.upgrade.xml|341175|{crc32(xml)}|{len(xml)}|1",
    ]
    ver = ("\n".join([f"+|21393|{VERSION}|HM|{header_product}|{package_id}|1"]
                     + rows) + "\n").encode()
    (root / f"{PRODUCT}.ver").write_bytes(ver)
    return {
        "SCOURGE_FIRMWARE_ID": VERSION,
        "SCOURGE_REGION": "EU",
        "SCOURGE_BRAND": "HM",
        "VER_FILE": f"{PRODUCT}.ver",
        "VER_STOCK_SIZE": str(len(ver)),
        "VER_STOCK_SHA256": sha256(ver),
        "VER_ROOTFS_ROW_ID": "341174",
        "VER_ROOTFS_ROW_STOCK_CRC": str(crc32(rootfs)),
        "GENUINE_ENVELOPE_SIZE": str(len(rootfs)),
        "GENUINE_MD5": md5(rootfs),
        "VER_WRAPPER_ROW_ID": "341142",
        "VER_WRAPPER_ROW_STOCK_CRC": str(crc32(wrapper)),
        "WRAPPER_SIZE": str(len(wrapper)),
        "WRAPPER_STOCK_SHA256": sha256(wrapper),
        "LGE_UPGRADE_XML_SHA256": sha256(xml),
    }


class FirmwareAuditTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="scourge-audit-")
        self.root = Path(self.tmp.name) / "pkg"
        self.locks = write_package(self.root)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_clean_package_passes(self) -> None:
        result = audit(self.root, self.locks)
        self.assertTrue(result["ok"], result["findings"])
        self.assertEqual(result["errors"], 0)

    def test_stale_rootfs_pin_is_named(self) -> None:
        # 1744474344 is the historical wrong pin (initial-root container CRC).
        self.locks["VER_ROOTFS_ROW_STOCK_CRC"] = "1744474344"
        result = audit(self.root, self.locks)
        self.assertFalse(result["ok"])
        text = "\n".join(f["detail"] for f in result["findings"])
        self.assertIn("lock pin is stale/wrong", text)
        self.assertIn(str(crc32(b"rootfs-container")), text)

    def test_new_package_id_is_a_warning(self) -> None:
        # The HMG server moves its package-id counter between repacks; only
        # the header (and therefore the file hash/size) changes.
        write_package(self.root, package_id="101489")
        result = audit(self.root, self.locks)
        self.assertTrue(result["ok"], result["findings"])
        self.assertGreaterEqual(result["warnings"], 1)
        checks = {f["check"] for f in result["findings"] if f["level"] == "warn"}
        self.assertIn("ver.sha", checks)

    def test_new_package_id_fails_strict(self) -> None:
        write_package(self.root, package_id="101489")
        result = audit(self.root, self.locks, strict=True)
        self.assertFalse(result["ok"])
        self.assertEqual(result["errors"], 0)

    def test_corrupt_file_fails(self) -> None:
        (self.root / "HU/images/mango-rootfs.tar.gz").write_bytes(b"changed")
        result = audit(self.root, self.locks)
        self.assertFalse(result["ok"])
        text = "\n".join(f["detail"] for f in result["findings"])
        self.assertIn("not self-consistent", text)

    def test_header_product_mismatch_fails(self) -> None:
        write_package(self.root, header_product="SOMETHING_ELSE")
        result = audit(self.root, self.locks)
        self.assertFalse(result["ok"])
        text = "\n".join(f["detail"] for f in result["findings"])
        self.assertIn("product", text)

    def test_truncated_package_map_fails(self) -> None:
        ver = self.root / f"{PRODUCT}.ver"
        ver.write_bytes(ver.read_bytes()[:20])
        result = audit(self.root, self.locks)
        self.assertFalse(result["ok"])
        text = "\n".join(f["detail"] for f in result["findings"])
        self.assertIn("shorter", text)

    def test_signed_xml_change_is_an_error(self) -> None:
        changed = b"different-signed-xml"
        (self.root / ".lge.upgrade.xml").write_bytes(changed)
        ver = self.root / f"{PRODUCT}.ver"
        lines = ver.read_text().splitlines()
        for index, line in enumerate(lines):
            fields = line.split("|")
            if len(fields) >= 6 and fields[1] == ".lge.upgrade.xml":
                fields[3] = str(crc32(changed))
                fields[4] = str(len(changed))
                lines[index] = "|".join(fields)
        ver.write_text("\n".join(lines) + "\n")
        result = audit(self.root, self.locks)
        self.assertFalse(result["ok"])
        self.assertTrue(any(f["check"] == "xml.lock-hash" and
                            f["level"] == "error" for f in result["findings"]))

    def test_all_rows_checks_extra_entries(self) -> None:
        firmware = self.root / "HU/firmware/update.tar.gz"
        firmware.parent.mkdir(parents=True, exist_ok=True)
        firmware.write_bytes(b"firmware-payload")
        ver = self.root / f"{PRODUCT}.ver"
        rows = ver.read_text().splitlines()
        rows.append(f"{PRODUCT}\\HU\\firmware|update.tar.gz|341146|"
                    f"{crc32(b'firmware-payload')}|{len(b'firmware-payload')}|1")
        ver.write_text("\n".join(rows) + "\n")
        result = audit(self.root, self.locks, all_rows=True)
        self.assertTrue(result["ok"], result["findings"])
        self.assertTrue(any(f["check"] == "all-rows.crc"
                            for f in result["findings"]))
        firmware.write_bytes(b"changed")
        result = audit(self.root, self.locks, all_rows=True)
        self.assertFalse(result["ok"])
        self.assertTrue(any(f["check"] == "all-rows.crc" and
                            f["level"] == "error" for f in result["findings"]))


if __name__ == "__main__":
    unittest.main()
