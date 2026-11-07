# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Host-side PNG sanity checks for screenshots."""
from __future__ import annotations

from pathlib import Path


def nonblack_bbox(path: Path) -> dict | None:
    """Return the non-black bounding box; raise if Pillow is unavailable."""
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError(
            "Pillow is required for screenshot pixel checks; install python-pillow"
        ) from exc
    image = Image.open(path).convert("RGB")
    width, height = image.size
    pixels = image.load()
    xs: list[int] = []
    ys: list[int] = []
    for y in range(height):
        for x in range(width):
            red, green, blue = pixels[x, y]
            if red + green + blue > 30:
                xs.append(x)
                ys.append(y)
    if not xs:
        return {"count": 0, "bbox": None, "width": width, "height": height}
    return {
        "count": len(xs),
        "bbox": [min(xs), min(ys), max(xs), max(ys)],
        "width": width,
        "height": height,
    }
