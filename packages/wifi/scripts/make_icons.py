#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Generate theme-visible WiFi glyphs for the home-screen tile.

The normal icon is a medium blue that reads on both the light and dark
home-screen themes.  The disabled icon stays grey.  Text colour is handled by
QText/HomeThemeHelper in WifiTile.qml, not by the icon.
"""
from PIL import Image, ImageDraw
import os

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   "artifacts", "assets", "wifi")


def make(color: str, path: str) -> None:
    im = Image.new("RGBA", (99, 99), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # dot
    d.ellipse((44, 66, 55, 77), fill=color)
    # arcs (bottom to top)
    d.arc((27, 48, 72, 93), 225, 315, fill=color, width=7)
    d.arc((17, 33, 82, 98), 225, 315, fill=color, width=7)
    d.arc((7, 18, 92, 103), 225, 315, fill=color, width=7)
    im.save(path)


os.makedirs(OUT, exist_ok=True)
make("#2F6FED", os.path.join(OUT, "wifi_icon_n.png"))
make("#8A8A8A", os.path.join(OUT, "wifi_icon_d.png"))
print("wrote", OUT)
