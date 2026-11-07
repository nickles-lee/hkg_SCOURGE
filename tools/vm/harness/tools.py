# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Tools-image provenance and staleness gate.

`toolsroot/manifest.json` is generated from the current tools tree and copied
into every newly built tools image. It maps each shipped file to its sha256 and
carries a deterministic `build_id` over that mapping. An image is *fresh* only
when its embedded manifest matches a manifest generated from the current
`toolsroot/` tree; an image without a valid manifest is *unmanifested* and is
treated as stale.

This catches the class of bug where `toolsroot/phase4.sh` or `scenarios.sh`
changes but an older image (for example the legacy `tools.dev.img`) is still
used. The gate is intentionally independent of the guest: no image is mounted
and no target code runs on the host.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

MANIFEST_NAME = "manifest.json"
SCHEMA_VERSION = 1
VM = Path(__file__).resolve().parent.parent
TOOLSROOT = VM / "toolsroot"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _manifest_files(root: Path) -> dict[str, dict[str, Any]]:
    files: dict[str, dict[str, Any]] = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        relative = path.relative_to(root).as_posix()
        if relative == MANIFEST_NAME:
            # Never hash the manifest into itself.
            continue
        files[relative] = {
            "sha256": sha256_file(path),
            "size": path.stat().st_size,
        }
    return files


def build_manifest(root: Path = TOOLSROOT) -> dict[str, Any]:
    """Return a deterministic manifest for a tools tree."""
    files = _manifest_files(root)
    canonical = json.dumps(
        {name: item["sha256"] for name, item in files.items()},
        sort_keys=True,
        separators=(",", ":"),
    ).encode()
    return {
        "schema": SCHEMA_VERSION,
        "build_id": hashlib.sha256(canonical).hexdigest(),
        "files": files,
    }


def write_manifest(root: Path = TOOLSROOT, output: Path | None = None) -> Path:
    output = output or (root / MANIFEST_NAME)
    output.write_text(json.dumps(build_manifest(root), indent=2, sort_keys=True) + "\n")
    return output


def _normalise_files(data: dict[str, Any]) -> dict[str, str]:
    result: dict[str, str] = {}
    files = data.get("files")
    if not isinstance(files, dict):
        return result
    for name, value in files.items():
        if isinstance(value, dict) and isinstance(value.get("sha256"), str):
            result[name] = value["sha256"]
        elif isinstance(value, str):
            result[name] = value
    return result


def read_image_manifest(image: Path) -> dict[str, Any] | None:
    """Read `/manifest.json` from an ext4 image without mounting it.

    `debugfs` is read-only here; a missing or malformed manifest returns None.
    """
    if not image.is_file():
        return None
    if shutil.which("debugfs") is None:
        return None
    result = subprocess.run(
        ["debugfs", "-R", f"cat /{MANIFEST_NAME}", str(image)],
        capture_output=True,
        text=True,
        check=False,
    )
    text = result.stdout.strip()
    if not text:
        return None
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        return None
    if not isinstance(data, dict) or data.get("schema") != SCHEMA_VERSION:
        return None
    if not isinstance(data.get("build_id"), str) or not isinstance(data.get("files"), dict):
        return None
    return data


def compare_manifests(expected: dict[str, Any], actual: dict[str, Any]) -> dict[str, Any]:
    expected_files = _normalise_files(expected)
    actual_files = _normalise_files(actual)
    changed = sorted(
        name
        for name in expected_files.keys() & actual_files.keys()
        if expected_files[name] != actual_files[name]
    )
    only_in_tree = sorted(expected_files.keys() - actual_files.keys())
    only_in_image = sorted(actual_files.keys() - expected_files.keys())
    stale = sorted(set(changed) | set(only_in_tree) | set(only_in_image))
    build_id_matches = expected.get("build_id") == actual.get("build_id")
    return {
        "state": "fresh" if build_id_matches and not stale else "stale",
        "build_id": actual.get("build_id"),
        "expected_build_id": expected.get("build_id"),
        "changed": changed,
        "only_in_tree": only_in_tree,
        "only_in_image": only_in_image,
        "stale": stale,
    }


def check_tools(image: Path, root: Path = TOOLSROOT) -> dict[str, Any]:
    """Compare an image's embedded manifest with the current tools tree."""
    expected = build_manifest(root)
    actual = read_image_manifest(image)
    if actual is None:
        return {
            "ok": False,
            "state": "unmanifested",
            "image": str(image),
            "expected_build_id": expected["build_id"],
            "detail": (
                "no valid embedded manifest; rebuild with "
                "sh build_tools.sh NEW-tools.img"
            ),
        }
    comparison = compare_manifests(expected, actual)
    comparison.update(
        {
            "ok": comparison["state"] == "fresh",
            "image": str(image),
            "detail": (
                "embedded manifest matches toolsroot"
                if comparison["state"] == "fresh"
                else "embedded manifest differs from toolsroot; rebuild with "
                "sh build_tools.sh NEW-tools.img"
            ),
        }
    )
    return comparison


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)

    manifest_parser = sub.add_parser("manifest", help="write toolsroot/manifest.json")
    manifest_parser.add_argument("--root", type=Path, default=TOOLSROOT)
    manifest_parser.add_argument("--output", type=Path, default=None)

    check_parser = sub.add_parser("check", help="check an image's embedded manifest")
    check_parser.add_argument("--image", type=Path, required=True)
    check_parser.add_argument("--root", type=Path, default=TOOLSROOT)
    check_parser.add_argument("--quiet", action="store_true")

    args = parser.parse_args(argv)
    if args.action == "manifest":
        output = write_manifest(args.root, args.output)
        print(output)
        return 0

    status = check_tools(args.image, args.root)
    if not args.quiet:
        print(json.dumps(status, indent=2, sort_keys=True))
    return 0 if status["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
