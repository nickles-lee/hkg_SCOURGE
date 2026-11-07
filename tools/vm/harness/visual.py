# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Screenshot baselines and thresholded pixel diffs."""
from __future__ import annotations

import hashlib
import json
import shutil
from pathlib import Path


def _pil():
    try:
        from PIL import Image, ImageChops
    except ImportError as exc:
        raise RuntimeError("Pillow is required for visual regression") from exc
    return Image, ImageChops


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def baseline(source: Path, name: str, baselines: Path, run: str | None = None) -> dict:
    Image, _ = _pil()
    baselines.mkdir(parents=True, exist_ok=True)
    name = name if name.endswith(".png") else f"{name}.png"
    destination = baselines / name
    shutil.copy2(source, destination)
    with Image.open(destination) as image:
        width, height = image.size
    metadata = {
        "name": name,
        "source_run": run,
        "sha256": _sha256(destination),
        "size": destination.stat().st_size,
        "width": width,
        "height": height,
    }
    (baselines / f"{name}.json").write_text(json.dumps(metadata, indent=2) + "\n")
    return metadata


def compare(candidate: Path, name: str, baselines: Path,
            threshold: float = 0.01, tolerance: int = 8,
            diff_path: Path | None = None) -> dict:
    Image, ImageChops = _pil()
    name = name if name.endswith(".png") else f"{name}.png"
    base_path = baselines / name
    if not base_path.is_file():
        raise ValueError(f"baseline missing: {base_path}; run visual baseline first")
    if not 0 <= threshold <= 1:
        raise ValueError("threshold must be in [0, 1]")
    with Image.open(base_path) as image:
        base = image.convert("RGB")
    with Image.open(candidate) as image:
        candidate_image = image.convert("RGB")
    if base.size != candidate_image.size:
        return {"ok": False, "reason": "size-mismatch", "baseline": list(base.size),
                "candidate": list(candidate_image.size), "threshold": threshold}
    difference = ImageChops.difference(base, candidate_image)
    gray = difference.convert("L")
    # Count pixels above tolerance, not merely non-zero compression noise.
    mask = gray.point(lambda value: 255 if value > tolerance else 0)
    histogram = mask.histogram()
    changed = histogram[255]
    total = base.size[0] * base.size[1]
    ratio = changed / total if total else 0.0
    if diff_path is None:
        diff_path = baselines / f"{name}.diff.png"
    # Amplify the diff for human review while keeping the original geometry.
    difference.point(lambda value: min(255, value * 4)).save(diff_path)
    return {
        "ok": ratio <= threshold,
        "baseline": str(base_path),
        "candidate": str(candidate),
        "diff": str(diff_path),
        "width": base.size[0],
        "height": base.size[1],
        "changed_pixels": changed,
        "total_pixels": total,
        "ratio": round(ratio, 6),
        "threshold": threshold,
        "tolerance": tolerance,
        "baseline_sha256": _sha256(base_path),
        "candidate_sha256": _sha256(candidate),
    }


def list_baselines(baselines: Path) -> list[dict]:
    values = []
    if not baselines.is_dir():
        return values
    for metadata in sorted(baselines.glob("*.png.json")):
        try:
            values.append(json.loads(metadata.read_text()))
        except (OSError, json.JSONDecodeError):
            continue
    return values
