#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Generate the Battery home-screen icons (blue line art + dim variant)."""
import os
from PIL import Image, ImageDraw

SIZE = 99
OUT = os.path.join(os.path.dirname(__file__), "..", "artifacts", "assets", "battery")


def draw_icon(color):
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    w = 5

    # Battery body: rounded rectangle, leaving room for the terminal.
    left, top, right, bottom = 12, 26, 78, 78
    d.rounded_rectangle([left, top, right, bottom], radius=8, outline=color, width=w)

    # Terminal on the right.
    d.rounded_rectangle([right + 3, 40, right + 12, 64], radius=3,
                        outline=color, width=w)

    # Lightning bolt inside the body.
    bolt = [
        (46, 36), (34, 55), (44, 55), (38, 70), (56, 48), (45, 48), (51, 36),
    ]
    d.polygon(bolt, fill=color)

    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    draw_icon((47, 111, 237, 255)).save(os.path.join(OUT, "battery_icon_n.png"))
    draw_icon((138, 138, 138, 255)).save(os.path.join(OUT, "battery_icon_d.png"))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
