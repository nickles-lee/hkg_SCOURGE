#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Generate the Tweaks home-screen glyph (three blue sliders)."""
from PIL import Image, ImageDraw
import os

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "artifacts", "assets", "tweaks")


def make(color: str, path: str) -> None:
    im = Image.new("RGBA", (99, 99), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # Three horizontal slider tracks with knobs.
    rows = [(20, 22), (49, 64), (78, 38)]
    for y, knob_x in rows:
        d.rounded_rectangle((12, y - 3, 87, y + 3), radius=3, fill=color)
        d.ellipse((knob_x - 10, y - 10, knob_x + 10, y + 10), fill=color)
    im.save(path)


def make_power(color: str, path: str) -> None:
    im = Image.new("RGBA", (99, 99), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # Power symbol: a ring with a gap at the top and a vertical stem.
    d.arc((19, 24, 80, 85), start=300, end=240, fill=color, width=7)
    d.line((49, 17, 49, 54), fill=color, width=7)
    im.save(path)


os.makedirs(OUT, exist_ok=True)
make("#2F6FED", os.path.join(OUT, "tweaks_icon_n.png"))
make("#8A8A8A", os.path.join(OUT, "tweaks_icon_d.png"))
make_power("#2F6FED", os.path.join(OUT, "reboot_icon_n.png"))
make_power("#8A8A8A", os.path.join(OUT, "reboot_icon_d.png"))
print("wrote", OUT)
