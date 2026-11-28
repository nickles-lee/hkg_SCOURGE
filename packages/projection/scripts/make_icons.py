#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Generate the Projection home-screen glyph: a phone outline with a turn arrow.

Same canvas (99x99), blue (#2F6FED), 7 px line weight and transparent
background as the Tweaks/Reboot/Wi-Fi glyphs, so the tiles read as one set.
Drawn at 4x and downsampled for clean curves. No OEM assets.
"""
from PIL import Image, ImageDraw
import os

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "artifacts", "assets", "projection")
S = 4          # supersampling factor
LW = 7 * S     # line weight, matches the sibling glyphs


def p(*xy):
    return tuple(v * S for v in xy)


def make(color: str, path: str) -> None:
    im = Image.new("RGBA", (99 * S, 99 * S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    # Phone body (outline) and a short speaker slot at the top.
    d.rounded_rectangle(p(21, 6, 78, 93), radius=12 * S, outline=color, width=LW)
    d.rounded_rectangle(p(42, 15, 57, 19), radius=2 * S, fill=color)

    # Right-turn arrow: stem up, quarter-circle bend, short run, arrowhead.
    # Round caps/joins are drawn as discs so the stroke ends look finished.
    r = 11                      # bend radius (centre line)
    sx, top, bottom = 39, 51, 79
    run_end = 54
    d.line(p(sx, bottom, sx, top), fill=color, width=LW)
    # PIL strokes arcs inward from the box, so grow the box by half a line
    # to keep the arc centred on the same line as the straight segments.
    h = LW / 2
    cx, cy = (sx + r) * S, top * S
    d.arc((cx - r * S - h, cy - r * S - h, cx + r * S + h, cy + r * S + h),
          start=180, end=270, fill=color, width=LW)
    d.line(p(sx + r, top - r, run_end, top - r), fill=color, width=LW)
    half = LW / 2
    for cx, cy in ((sx, bottom), (sx, top), (sx + r, top - r)):
        d.ellipse((cx * S - half, cy * S - half, cx * S + half, cy * S + half), fill=color)
    ay = top - r
    d.polygon([p(run_end - 2, ay - 12), p(run_end + 14, ay), p(run_end - 2, ay + 12)],
              fill=color)

    im.resize((99, 99), Image.LANCZOS).save(path)


os.makedirs(OUT, exist_ok=True)
make("#2F6FED", os.path.join(OUT, "projection_icon_n.png"))
make("#8A8A8A", os.path.join(OUT, "projection_icon_d.png"))
print("wrote", OUT)
