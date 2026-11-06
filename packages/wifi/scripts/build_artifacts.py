#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Build the WiFi app QML artifacts for Ioniq 5 Gen5W 260128.

Reads the stock/Hello/Battery base QML from ../base and writes patched
variants into ../artifacts/qml/AppHomeScreen/qml_eco3.0/.

The home-screen tile is placed with a single global allocator: it scans the
app pages in order and returns the Nth free cell.  The custom-tile ordinal is
fixed at install time by counting the custom tiles already present in the QML
(Hello, Battery, Tweaks).  This keeps WiFi, Hello and Battery from fighting
over page 1.
"""
from __future__ import annotations

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BASE = os.path.join(ROOT, "base")
OUT = os.path.join(ROOT, "artifacts", "qml", "AppHomeScreen", "qml_eco3.0")
MARK = "// WIFI_PAYLOAD_260128\n"

ADV = "HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
WIDE = "HomeScreen/QHMI_HomeScreenMain_Wide.qml"
ALL = "HomeScreen/Screen_HomeScreen_AllMenus.qml"
SETUP = "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
CUSTOM = "component/QCustomizedMenuListAllowEmpty.qml"

PAGE_MENUS = [
    (1, "menuPage", "helloTile1", 0),
    (2, "menuPage2", "helloTile2", 1),
    (3, "menuPage3", "helloTile3", 2),
]


def read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def write(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def add_marker(text: str) -> str:
    if "WIFI_PAYLOAD_260128" in text:
        return text
    if text.startswith("//"):
        first, rest = text.split("\n", 1)
        return first + "\n" + MARK + rest
    return MARK + text


def find_block_end(text: str, start: int) -> int:
    """Return index just after the matching closing brace of the block at start."""
    i = text.find("{", start)
    if i < 0:
        raise ValueError("no opening brace")
    depth = 0
    in_str = None
    escaped = False
    in_line_comment = False
    in_block_comment = False
    j = i
    while j < len(text):
        c = text[j]
        nxt = text[j + 1] if j + 1 < len(text) else ""
        if in_line_comment:
            if c == "\n":
                in_line_comment = False
            j += 1
            continue
        if in_block_comment:
            if c == "*" and nxt == "/":
                in_block_comment = False
                j += 2
            else:
                j += 1
            continue
        if in_str:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == in_str:
                in_str = None
            j += 1
            continue
        if c == "/" and nxt == "/":
            in_line_comment = True
            j += 2
            continue
        if c == "/" and nxt == "*":
            in_block_comment = True
            j += 2
            continue
        if c in ("'", '"'):
            in_str = c
            j += 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return j + 1
        j += 1
    raise ValueError("unbalanced block")


def insert_after_block(text: str, marker: str, insertion: str) -> str:
    pos = text.find(marker)
    if pos < 0:
        raise ValueError(f"marker not found: {marker!r}")
    end = find_block_end(text, pos)
    return text[:end] + "\n" + insertion + text[end:]


EMPTY_FN = """    function emptySlots(pageModel) {
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

    // Return the customIndex-th free cell across all normal app pages.
    // Page 0 is the widget page and has no app grid, so start at listPage1.
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

    function firstEmptyIndex(pageModel, skip) {
        if (skip === undefined) skip = 0
        var slots = emptySlots(pageModel)
        return skip < slots.length ? slots[skip] : -1
    }
"""


def patch_empty_fn(text: str) -> str:
    if "function firstEmptySlot(" in text:
        return text
    # Replace the old per-page helper with the global allocator plus the
    # compatibility helper used by All Menus / Setup.
    pat = re.compile(
        r"    function firstEmptyIndex\(pageModel(?:,\s*skip)?\) \{.*?\n    \}\n",
        re.S,
    )
    if pat.search(text):
        return pat.sub(EMPTY_FN, text, count=1)
    for marker in ("    Component {\n", "    QListViewHomePage {\n"):
        if marker in text:
            return text.replace(marker, EMPTY_FN + "\n" + marker, 1)
    raise ValueError("cannot place firstEmptyIndex")


def custom_tile_count(text: str) -> int:
    """Count custom home-screen tiles already present, in install order."""
    count = 0
    if re.search(r"id:\s*helloTile1\b", text):
        count += 1
    if re.search(r"id:\s*batteryTile1\b", text):
        count += 1
    if re.search(r"id:\s*tweaksTile1\b", text):
        count += 1
    return count


def slot_condition(page: int) -> str:
    return f"slot !== null && slot.page === {page}"


def slot_x(menu: str, page: int) -> str:
    return (f"{menu}.x + ({slot_condition(page)} ? "
            f"(slot.index % k_nbrOfColumns) * k_itemWidth : 0)")


def slot_y(menu: str, page: int) -> str:
    return (f"{menu}.y + ({slot_condition(page)} ? "
            f"Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)")


def hello_tile_block(menu: str, tile_id: str, page: int, onclick: str) -> str:
    cond = slot_condition(page)
    return f"""                HelloTile {{
                    id: {tile_id}
                    property var slot: firstEmptySlot(0)
                    visible: {cond}
                    x: {slot_x(menu, page)}
                    y: {slot_y(menu, page)}
                    width: k_itemWidth
                    height: k_itemHeight
                    z: 5
                    {onclick}
                }}
"""


def patch_hello_tiles(text: str) -> str:
    """Place Hello once, in the first free cell across all app pages."""
    for page, menu, tile_id, page_index in PAGE_MENUS:
        marker = f"                HelloTile {{\n                    id: {tile_id}"
        pos = text.find(marker)
        if pos < 0:
            continue
        end = find_block_end(text, pos)
        block = text[pos:end]
        m = re.search(r"^\s*(onClicked:.*)$", block, re.M)
        onclick = m.group(1) if m else "onClicked: helloPopup.visible = true"
        text = text[:pos] + hello_tile_block(menu, tile_id, page_index, onclick) + text[end:]
    return text


def wifi_tile_block(menu: str, page: int, custom_index: int) -> str:
    cond = slot_condition(page)
    return f"""                WifiTile {{
                    id: wifiTile{page + 1}
                    property var slot: firstEmptySlot({custom_index})
                    visible: {cond}
                    x: {slot_x(menu, page)}
                    y: {slot_y(menu, page)}
                    width: k_itemWidth
                    height: k_itemHeight
                    z: 6
                    onClicked: wifiPage.visible = true
                }}
"""


def insert_wifi_tiles(text: str, custom_index: int) -> str:
    for page, menu, hello_id, page_index in PAGE_MENUS:
        if f"id: wifiTile{page}" in text:
            continue
        hello_marker = f"                HelloTile {{\n                    id: {hello_id}"
        page_marker = f"                QMenuPageNoDrag{{\n                    id: {menu}"
        if hello_marker in text:
            marker = hello_marker
        elif page_marker in text:
            marker = page_marker
        else:
            continue
        text = insert_after_block(text, marker, wifi_tile_block(menu, page_index, custom_index))
    return text


def patch_advanced(src: str) -> str:
    text = add_marker(read(src))
    text = patch_empty_fn(text)
    has_hello = bool(re.search(r"id:\s*helloTile1\b", text))
    if has_hello:
        text = patch_hello_tiles(text)
    wifi_index = custom_tile_count(text)
    text = insert_wifi_tiles(text, wifi_index)
    if "id: wifiPage" not in text:
        page = """    WifiPage {
        id: wifiPage
        x: 0
        y: ConstantModel.MENU_BAR_Y
        width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
        height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y
        visible: false
        z: 2000
    }

    // Steering-wheel Home/Back key closes the page, like the Battery
    // overlay.  A switch to another app (top-bar home/launcher) closes it
    // too, so the page never stays on top of a different screen.
    Connections {
        target: AppHomeModel
        onButtonBackClicked: {
            if (wifiPage.visible)
                wifiPage.visible = false
        }
        onSwitchToOtherApp: {
            if (wifiPage.visible)
                wifiPage.visible = false
        }
    }

"""
        last = text.rstrip().rfind("}")
        text = text[:last] + page + text[last:]
    return text


def patch_wide(src: str) -> str:
    text = add_marker(read(src))
    has_hello = "id: itemHello" in text
    if has_hello:
        old_cell, new_cell = 200, 166
        marker = "        QHomeItem {\n            id: itemHello"
        xexpr = "itemHello.right"
        tile_width = 166
    else:
        old_cell, new_cell = 250, 200
        marker = "        GridView {\n            id: mainGridItemMenu"
        xexpr = "itemAllMenu.right + mainGridItemMenu.cellWidth * 4"
        tile_width = 200

    # Shrink the tile row enough for the new tile.  Stock Wide uses 250 px
    # cells; the Hello base uses 200.  Both need the same treatment.
    text = text.replace(
        f"cellWidth: AppHomeModel.isWideScr ? {old_cell} : 207",
        f"cellWidth: AppHomeModel.isWideScr ? {new_cell} : 207",
    )
    text = text.replace(
        f"width: AppHomeModel.isWideScr ? {old_cell} : 207",
        f"width: AppHomeModel.isWideScr ? {new_cell} : 207",
    )

    if "id: itemWifi" not in text:
        tile = f"""        QHomeItem {{
            id: itemWifi
            visible: AppHomeModel.isWideScr
            width: AppHomeModel.isWideScr ? {tile_width} : 207
            height: 183
            x: {xexpr}
            x_icon: AppHomeModel.isWideScr ? 50 : 55
            iconSource: ["file:///app/share/AppHomeScreen/wifi/wifi_icon_n.png", defineModel.home_bg_icon_p, "", "file:///app/share/AppHomeScreen/wifi/wifi_icon_d.png"]
            itemName: "WiFi"
            icoPressed: wifiMouse.pressed
            MouseArea {{
                id: wifiMouse
                anchors.fill: parent
                onClicked: wifiPage.visible = true
            }}
        }}

"""
        text = insert_after_block(text, marker, tile)
    if "id: wifiPage" not in text:
        page = """    WifiPage {
        id: wifiPage
        x: 0
        y: ConstantModel.MENU_BAR_Y
        width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
        height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y
        visible: false
        z: 2000
    }

    // Steering-wheel Home/Back key closes the page, like the Battery
    // overlay.  A switch to another app (top-bar home/launcher) closes it
    // too, so the page never stays on top of a different screen.
    Connections {
        target: AppHomeModel
        onButtonBackClicked: {
            if (wifiPage.visible)
                wifiPage.visible = false
        }
        onSwitchToOtherApp: {
            if (wifiPage.visible)
                wifiPage.visible = false
        }
    }

"""
        last = text.rstrip().rfind("}")
        text = text[:last] + page + text[last:]
    return text


def add_wifi_page_and_signal(text: str) -> str:
    # Signal wiring on the shared menu list.
    if "onWifiClicked:" not in text:
        if "onHelloClicked:" in text:
            pos = text.find("onHelloClicked:")
            end = find_block_end(text, pos)
            text = text[:end] + "\n            onWifiClicked: wifiPage.visible = true" + text[end:]
        else:
            pos = text.find("onMenuItemClicked:")
            end = find_block_end(text, pos)
            text = text[:end] + "\n            onWifiClicked: wifiPage.visible = true" + text[end:]

    if "id: wifiPage" not in text:
        page = """    WifiPage {
        id: wifiPage
        x: 0
        y: ConstantModel.MENU_BAR_Y
        width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
        height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y
        visible: false
        z: 2000
    }

    // Steering-wheel Home/Back key closes the page, like the Battery
    // overlay.  A switch to another app (top-bar home/launcher) closes it
    // too, so the page never stays on top of a different screen.
    Connections {
        target: AppHomeModel
        onButtonBackClicked: {
            if (wifiPage.visible)
                wifiPage.visible = false
        }
        onSwitchToOtherApp: {
            if (wifiPage.visible)
                wifiPage.visible = false
        }
    }

"""
        last = text.rstrip().rfind("}")
        text = text[:last] + page + text[last:]
    return text


def patch_all_or_setup(src: str) -> str:
    text = add_marker(read(src))
    return add_wifi_page_and_signal(text)


def patch_custom(src: str) -> str:
    text = add_marker(read(src))
    has_hello = "HelloTile" in text
    if "signal wifiClicked()" not in text:
        if "signal helloClicked()" in text:
            text = text.replace("    signal helloClicked()\n",
                                "    signal helloClicked()\n    signal wifiClicked()\n", 1)
        else:
            text = text.replace("    id: root\n",
                                "    id: root\n    signal wifiClicked()\n", 1)
    text = patch_empty_fn(text)
    skip = 1 if has_hello else 0
    tile = f"""            WifiTile {{
                id: wifiTile
                property int emptyIdx: root.firstEmptyIndex(menuPage.pageModel, {skip})
                visible: (isAllMenuScreen || listType === AppHome_Enum.E_LIST_SETUP_MAIN) && !customizeMode && index === menuList.currentIndex && emptyIdx >= 0
                x: menuPage.x + (emptyIdx % k_nbrOfColumns) * k_itemWidth
                y: menuPage.y + Math.floor(emptyIdx / k_nbrOfColumns) * k_itemHeight
                width: k_itemWidth
                height: k_itemHeight
                z: 6
                onClicked: root.wifiClicked()
            }}
"""
    if has_hello:
        marker = "            HelloTile {\n                id: helloTile"
    else:
        marker = "            QMenuPageAllowEmpty{\n                id: menuPage"
    text = insert_after_block(text, marker, tile)
    return text


def patch_theme_image() -> None:
    src = os.path.join(BASE, "stock", "component", "Q_ThemeImage.qml")
    if not os.path.exists(src):
        rootfs = os.environ.get(
            "SCOURGE_ROOTFS",
            os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "..", "..", "..", "work", "rootfs"))
        src = os.path.join(rootfs, "app", "share", "AppHomeScreen",
                           "qml_eco3.0", "component", "common",
                           "Q_ThemeImage.qml")
    text = read(src)
    if 'indexOf("file://")' not in text:
        old = 'source: themeSource !== "" ? (themeSource + AppHomeModel.dayLightSuffix) : themeSource'
        new = ('source: themeSource !== "" ? ((themeSource.indexOf("file://") === 0) ? '
               'themeSource : (themeSource + AppHomeModel.dayLightSuffix)) : themeSource')
        if old not in text:
            raise ValueError("Q_ThemeImage source line not found")
        text = text.replace(old, new, 1)
    write(os.path.join(OUT, "component", "common", "Q_ThemeImage.qml"), text)


def main() -> int:
    patch_theme_image()

    # Advanced variants.
    write(os.path.join(OUT, "HomeScreen", "QHMI_HomeScreenMain_Advanced.wifi.qml"),
          patch_advanced(os.path.join(BASE, "stock", "HomeScreen", "QHMI_HomeScreenMain_Advanced.qml")))
    write(os.path.join(OUT, "HomeScreen", "QHMI_HomeScreenMain_Advanced.hello_wifi.qml"),
          patch_advanced(os.path.join(BASE, "hello", "HomeScreen", "QHMI_HomeScreenMain_Advanced.qml")))
    write(os.path.join(OUT, "HomeScreen", "QHMI_HomeScreenMain_Advanced.battery_wifi.qml"),
          patch_advanced(os.path.join(BASE, "battery", "HomeScreen", "QHMI_HomeScreenMain_Advanced.qml")))
    write(os.path.join(OUT, "HomeScreen", "QHMI_HomeScreenMain_Advanced.hello_battery_wifi.qml"),
          patch_advanced(os.path.join(BASE, "battery_hello", "HomeScreen", "QHMI_HomeScreenMain_Advanced.hello.qml")))
    write(os.path.join(OUT, "HomeScreen", "QHMI_HomeScreenMain_Advanced.battery_tile_wifi.qml"),
          patch_advanced(os.path.join(BASE, "battery_tile", "HomeScreen", "QHMI_HomeScreenMain_Advanced.qml")))
    write(os.path.join(OUT, "HomeScreen", "QHMI_HomeScreenMain_Advanced.hello_battery_tile_wifi.qml"),
          patch_advanced(os.path.join(BASE, "battery_tile_hello", "HomeScreen", "QHMI_HomeScreenMain_Advanced.qml")))

    # Wide variants.
    write(os.path.join(OUT, "HomeScreen", "QHMI_HomeScreenMain_Wide.wifi.qml"),
          patch_wide(os.path.join(BASE, "stock", "HomeScreen", "QHMI_HomeScreenMain_Wide.qml")))
    write(os.path.join(OUT, "HomeScreen", "QHMI_HomeScreenMain_Wide.hello_wifi.qml"),
          patch_wide(os.path.join(BASE, "hello", "HomeScreen", "QHMI_HomeScreenMain_Wide.qml")))

    # All Menus + Setup variants.
    write(os.path.join(OUT, "HomeScreen", "Screen_HomeScreen_AllMenus.wifi.qml"),
          patch_all_or_setup(os.path.join(BASE, "stock", "HomeScreen", "Screen_HomeScreen_AllMenus.qml")))
    write(os.path.join(OUT, "HomeScreen", "Screen_HomeScreen_AllMenus.hello_wifi.qml"),
          patch_all_or_setup(os.path.join(BASE, "hello", "HomeScreen", "Screen_HomeScreen_AllMenus.qml")))
    write(os.path.join(OUT, "HomeScreen", "Screen_HomeScreen_Submenu_Setup.wifi.qml"),
          patch_all_or_setup(os.path.join(BASE, "stock", "HomeScreen", "Screen_HomeScreen_Submenu_Setup.qml")))
    write(os.path.join(OUT, "HomeScreen", "Screen_HomeScreen_Submenu_Setup.hello_wifi.qml"),
          patch_all_or_setup(os.path.join(BASE, "hello", "HomeScreen", "Screen_HomeScreen_Submenu_Setup.qml")))

    # Shared menu list variants.
    write(os.path.join(OUT, "component", "QCustomizedMenuListAllowEmpty.wifi.qml"),
          patch_custom(os.path.join(BASE, "stock", "component", "QCustomizedMenuListAllowEmpty.qml")))
    write(os.path.join(OUT, "component", "QCustomizedMenuListAllowEmpty.hello_wifi.qml"),
          patch_custom(os.path.join(BASE, "hello", "component", "QCustomizedMenuListAllowEmpty.qml")))

    print("artifacts written to", OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
