#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: AppHomeScreen eco3.0, Gen5W_L EU 260128
"""Reject lost placement bindings in generated/staged/tarred Advanced QML."""
import re
from pathlib import Path
import sys


def check(text):
    ordinals = {}
    # The packed combo Advanced QML must carry every custom tile; a missing
    # block is exactly the V4 regression (icons stacked at the page origin).
    REQUIRED = ("battery", "wifi", "tweaks", "reboot")
    for tile in ("hello", "battery", "wifi", "tweaks", "reboot"):
        for page in (1, 2, 3):
            match = re.search(r"\b(?:Hello|Battery|Wifi|Tweaks|Reboot)Tile\s*\{\s*id:\s*"
                              + tile + "Tile" + str(page) + r"\b([^{}]*)", text)
            if not match:
                assert not re.search(r"\bid:\s*" + tile + "Tile" + str(page) + r"\b", text), f"unrecognized {tile}Tile{page} block"
                continue
            body = match[1]
            slot = re.search(r"property var slot:\s*firstEmptySlot\((\d+)\)", body)
            assert slot, f"{tile}Tile{page}: missing placement binding"
            ordinal = int(slot[1])
            assert ordinals.setdefault(tile, ordinal) == ordinal, f"{tile}: ordinal differs across pages"
            guard = f"slot !== null && slot.page === {page - 1}"
            assert f"visible: {guard}" in body, f"{tile}Tile{page}: missing visibility guard"
            for axis in ("x", "y"):
                assert re.search(r"\b" + axis + r":.*slot.index", body), f"{tile}Tile{page}: missing {axis}"
            assert "width: k_itemWidth" in body and "height: k_itemHeight" in body
            # A custom tile must be a sibling of QMenuPageNoDrag, not its child.
            preceding = text[:match.start()]
            menu_start = preceding.rfind("QMenuPageNoDrag{")
            if menu_start >= 0:
                menu_text = preceding[menu_start:]
                assert menu_text.count("{") == menu_text.count("}"), f"{tile}Tile{page}: nested inside stock grid"
    assert len(set(ordinals.values())) == len(ordinals), f"duplicate custom slots: {ordinals}"
    missing = [tile for tile in REQUIRED if tile not in ordinals]
    assert not missing, f"missing custom tiles: {missing}"
    assert sorted(ordinals.values()) == list(range(len(ordinals))), f"non-contiguous custom slots: {ordinals}"
    for tile in ordinals:
        assert all(f"id: {tile}Tile{page}" in text for page in (1, 2, 3)), f"{tile}: missing page instance"
    return ordinals


if __name__ == "__main__":
    for name in sys.argv[1:]:
        ordinals = check(Path(name).read_text())
        print(f"PASS: {Path(name).name}: {ordinals}")
