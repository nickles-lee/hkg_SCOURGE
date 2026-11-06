#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Apply the Battery tile + overlay patch to QHMI_HomeScreenMain_Advanced.qml.

Usage: patch_advanced.py <input.qml> <output.qml>

The Battery page is a full-screen overlay.  The Battery tile is placed with
the same global allocator as the other custom tiles: it finds the
batteryIndex-th free cell across all app pages, where batteryIndex is the
number of custom tiles already present (Hello, WiFi, Tweaks).  This lets the
Battery tile follow Hello/WiFi onto page 2 or page 3 instead of hard-coding
page 2.
"""
import re
import sys

MARKER = "// BATTERY_PAYLOAD_260128\n"

HELPER = """    function emptySlots(pageModel) {
        var slots = []
        if (!pageModel) return slots
        var n = pageModel.getCount ? pageModel.getCount() : pageModel.count
        for (var i = 0; i < n; i++) {
            var item = pageModel.getItem ? pageModel.getItem(i) : null
            var isEmpty = true
            if (item) {
                if (typeof item.isEditingVisible === "function") {
                    isEmpty = !item.isEditingVisible()
                } else if (typeof item.visible !== "undefined") {
                    isEmpty = item.visible !== true
                } else {
                    isEmpty = false
                }
            }
            if (isEmpty) slots.push(i)
        }
        for (var j = n; j < nbrItemsPerPage; j++) slots.push(j)
        return slots
    }

    // menuPageN already sits below the top menu bar; callers add row offsets.
    function firstEmptySlot(customIndex) {
        var remaining = customIndex
        for (var p = 0; p < totalPagesNormalMode; p++) {
            var slots = emptySlots(getPageDisplayListModel(p))
            if (remaining < slots.length) {
                return { page: p, index: slots[remaining] }
            }
            remaining -= slots.length
        }
        return null
    }

"""


def tile_block(page: int, menu: str, battery_index: int) -> str:
    cond = f"slot !== null && slot.page === {page - 1}"
    return f"""                BatteryTile {{
                    id: batteryTile{page}
                    property var slot: firstEmptySlot({battery_index})
                    visible: {cond}
                    x: {menu}.x + ({cond} ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)
                    y: {menu}.y + ({cond} ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)
                    width: k_itemWidth
                    height: k_itemHeight
                    z: 5
                    onClicked: batteryOverlay.open()
                }}
"""


def insert_battery_tiles(text: str, battery_index: int) -> str:
    pages = ((1, "menuPage", "helloTile1"), (2, "menuPage2", "helloTile2"), (3, "menuPage3", "helloTile3"))
    for page, menu, hello_id in pages:
        if f"id: batteryTile{page}" in text:
            continue
        hello_marker = f"                HelloTile {{\n                    id: {hello_id}"
        page_marker = f"                QMenuPageNoDrag{{\n                    id: {menu}"
        if hello_marker in text:
            marker = hello_marker
        elif page_marker in text:
            marker = page_marker
        else:
            continue
        pos = text.find(marker)
        # Find the end of the tile/menu block so the new tile becomes a sibling.
        i = text.find("{", pos)
        depth = 0
        j = i
        while j < len(text):
            c = text[j]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    j += 1
                    break
            j += 1
        text = text[:j] + "\n" + tile_block(page, menu, battery_index) + text[j:]
    return text


OVERLAY = """    BatteryPage {
        id: batteryOverlay
        x: 0
        y: ConstantModel.MENU_BAR_Y
        width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
        height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y
        z: 2000
        onGoHome: close()
    }

"""



def hello_tile_block(menu: str, tile_id: str, page: int, onclick: str) -> str:
    cond = f"slot !== null && slot.page === {page}"
    return f"""                HelloTile {{
                    id: {tile_id}
                    property var slot: firstEmptySlot(0)
                    visible: {cond}
                    x: {menu}.x + ({cond} ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)
                    y: {menu}.y + ({cond} ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)
                    width: k_itemWidth
                    height: k_itemHeight
                    z: 5
                    {onclick}
                }}
"""


def patch_hello_tiles(text: str) -> str:
    for page, menu, tile_id in ((1, "menuPage", "helloTile1"), (2, "menuPage2", "helloTile2"), (3, "menuPage3", "helloTile3")):
        marker = f"                HelloTile {{\n                    id: {tile_id}"
        pos = text.find(marker)
        if pos < 0:
            continue
        i = text.find("{", pos)
        depth = 0
        j = i
        while j < len(text):
            c = text[j]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    j += 1
                    break
            j += 1
        block = text[pos:j]
        m = re.search(r"^\s*(onClicked:.*)$", block, re.M)
        onclick = m.group(1) if m else "onClicked: helloPopup.visible = true"
        text = text[:pos] + hello_tile_block(menu, tile_id, page - 1, onclick) + text[j:]
    return text


def custom_tile_count(text: str) -> int:
    count = 0
    for pattern in (r"id:\s*helloTile1\b", r"id:\s*wifiTile1\b", r"id:\s*tweaksTile1\b"):
        if re.search(pattern, text):
            count += 1
    return count


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "r", encoding="utf-8") as f:
        text = f.read()

    if MARKER.strip() in text:
        print(f"{src}: already patched", file=sys.stderr)
        return 1

    battery_index = custom_tile_count(text)

    # 1. Add the global allocator if the input does not already have it
    #    (for example when WiFi was installed first).
    if "function firstEmptySlot(" not in text:
        anchor = "    function getPageDisplayListModel(index)\n"
        if text.count(anchor) != 1:
            print(f"helper anchor not found once in {src}", file=sys.stderr)
            return 1
        text = text.replace(anchor, HELPER + anchor)

    # 2. Keep Hello on the same global allocator so it does not duplicate
    #    across pages and leave a stale tile where Battery wants to go.
    text = patch_hello_tiles(text)

    # 3. Battery tile on all app pages.  Only the page returned by the
    #    allocator is visible.
    text = insert_battery_tiles(text, battery_index)

    # 3. Full-screen overlay + back-button close.
    anchor = ("    Component.onCompleted: {\n"
              "        AppHomeModel.screenNotSupportSS = true\n")
    if text.count(anchor) != 1:
        print(f"overlay anchor not found once in {src}", file=sys.stderr)
        return 1
    text = text.replace(anchor, OVERLAY + anchor)

    # 4. Marker.
    if not text.startswith("//"):
        text = MARKER + text
    else:
        first, rest = text.split("\n", 1)
        text = first + "\n" + MARKER + rest

    with open(dst, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"wrote {dst} (battery_index={battery_index})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
