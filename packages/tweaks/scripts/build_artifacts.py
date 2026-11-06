#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Build the Tweaks app QML artifacts for Ioniq 5 Gen5W 260128.

Reads the stock / Hello / Battery / WiFi base QML from ../base and writes
patched variants into ../artifacts/qml/AppHomeScreen/qml_eco3.0/.

The patch adds one TweaksTile and one full-screen TweaksPage.  It is
deliberately additive so it can sit on top of the existing Hello, Battery
and WiFi payloads.
"""
from __future__ import annotations

import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BASE = os.path.join(ROOT, "base")
OUT = os.path.join(ROOT, "artifacts", "qml", "AppHomeScreen", "qml_eco3.0")
MARK = "// TWEAKS_PAYLOAD_260128\n"
ROOTFS = os.environ.get(
    "SCOURGE_ROOTFS",
    os.path.join(os.path.dirname(os.path.abspath(__file__)),
                 "..", "..", "..", "work", "rootfs"),
)
STANDBY_SRC = os.path.join(ROOTFS, "app", "share", "AppStandbyClock")
STANDBY_OUT = os.path.join(ROOT, "artifacts", "qml", "AppStandbyClock")
STANDBY_BASES = ("qml_eco3.0", "new_qml", "qml_genesis")
STANDBY_MARK = "// TWEAKS_DISCLAIMER_260128\n"

ADV = "HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
WIDE = "HomeScreen/QHMI_HomeScreenMain_Wide.qml"
ALL = "HomeScreen/Screen_HomeScreen_AllMenus.qml"
SETUP = "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
CUSTOM = "component/QCustomizedMenuListAllowEmpty.qml"


def read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def write(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def add_marker(text: str) -> str:
    if "TWEAKS_PAYLOAD_260128" in text:
        return text
    if text.startswith("//"):
        first, rest = text.split("\n", 1)
        return first + "\n" + MARK + rest
    return MARK + text


def find_block_end(text: str, start: int) -> int:
    """Return the index just after the matching closing brace."""
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


FIRST_EMPTY_INDEX_FN = """    function firstEmptyIndex(pageModel, skip) {
        if (skip === undefined) skip = 0
        var slots = emptySlots(pageModel)
        return skip < slots.length ? slots[skip] : -1
    }
"""


def patch_empty_fn(text: str) -> str:
    pat = re.compile(
        r"    function firstEmptyIndex\(pageModel(?:,\s*skip)?\) \{.*?\n    \}\n",
        re.S,
    )
    if "function firstEmptySlot(" in text:
        if re.search(r"    function firstEmptyIndex\(pageModel,\s*skip\)", text):
            return text
        if pat.search(text):
            return pat.sub(FIRST_EMPTY_INDEX_FN, text, count=1)
        return text
    if pat.search(text):
        return pat.sub(EMPTY_FN, text, count=1)
    for marker in ("    Component {\n", "    QListViewHomePage {\n"):
        if marker in text:
            return text.replace(marker, EMPTY_FN + "\n" + marker, 1)
    raise ValueError("cannot place firstEmptyIndex")


TWEAKS_BACK_CLOSE = """    // TWEAKS_BACK_CLOSE: Home/Back must dismiss the overlay.
    Connections {
        target: AppHomeModel
        onButtonBackClicked: {
            if (tweaksPage.visible)
                tweaksPage.visible = false
        }
    }

    Connections {
        target: AppHomeEngine
        onNotifyHomeMainChanged: {
            if (tweaksPage.visible)
                tweaksPage.visible = false
        }
    }

"""


def add_page(text: str) -> str:
    if "id: tweaksPage" not in text:
        page = """    TweaksPage {
        id: tweaksPage
        visible: false
    }

"""
        last = text.rstrip().rfind("}")
        text = text[:last] + page + text[last:]
    if "TWEAKS_BACK_CLOSE" not in text:
        last = text.rstrip().rfind("}")
        text = text[:last] + TWEAKS_BACK_CLOSE + text[last:]
    return text


def tile_advanced(page: int, skip: int) -> str:
    menu = "menuPage" if page == 1 else f"menuPage{page}"
    index = page - 1
    cond = "allMenuIndex === tweaksTargetPage()"
    return f"""                TweaksTile {{
                    id: tweaksTile{page}
                    property int slot: firstEmptyIndex(getPageDisplayListModel({index}), {skip})
                    visible: {cond}
                    x: {menu}.x + ({cond} ? (slot % k_nbrOfColumns) * k_itemWidth : 0)
                    y: {menu}.y + ({cond} ? Math.floor(slot / k_nbrOfColumns) * k_itemHeight : 0)
                    width: k_itemWidth
                    height: k_itemHeight
                    z: 7
                    onClicked: tweaksPage.visible = true
                }}
"""


def add_tweaks_target_fn(text: str, skips: tuple[int, int, int]) -> str:
    if "function tweaksTargetPage()" in text:
        return text
    pos = text.find("    function firstEmptyIndex(")
    if pos < 0:
        raise ValueError("firstEmptyIndex not found")
    end = find_block_end(text, pos)
    fn = f"""    property var tweaksSkips: [{skips[0]}, {skips[1]}, {skips[2]}]

    function tweaksTargetPage() {{
        var maxPage = Math.min(totalPagesNormalMode, 3)
        for (var i = 0; i < maxPage; i++) {{
            if (firstEmptyIndex(getPageDisplayListModel(i), tweaksSkips[i]) >= 0)
                return i
        }}
        return -1
    }}
"""
    return text[:end] + "\n" + fn + text[end:]


def patch_advanced(src: str) -> str:
    text = add_marker(read(src))
    text = patch_empty_fn(text)
    has_hello = "helloTile1" in text
    has_hello2 = "helloTile2" in text
    has_hello3 = "helloTile3" in text
    has_wifi = "wifiTile1" in text

    # Battery tiles do not consume a Tweaks slot in the Advanced layout.
    skips = (
        int(has_hello) + int(has_wifi),
        int(has_hello2),
        int(has_hello3),
    )

    pages = (
        (1, "menuPage", "menuPage", "WifiTile", "wifiTile1", "HelloTile", "helloTile1"),
        (2, "menuPage2", "menuPage2", None, None, "HelloTile", "helloTile2"),
        (3, "menuPage3", "menuPage3", None, None, "HelloTile", "helloTile3"),
    )
    for page, menu_id, menu_marker_id, first_type, first_id, second_type, second_id in pages:
        if f"id: tweaksTile{page}" in text:
            continue
        if first_type and first_id and f"id: {first_id}" in text:
            marker = f"                {first_type} {{\n                    id: {first_id}"
        elif second_type and second_id and f"id: {second_id}" in text:
            marker = f"                {second_type} {{\n                    id: {second_id}"
        else:
            marker = f"                QMenuPageNoDrag{{\n                    id: {menu_marker_id}"
        text = insert_after_block(text, marker, tile_advanced(page, skips[page - 1]))
    text = add_tweaks_target_fn(text, skips)
    return add_page(text)


def tile_advanced_global(page: int, custom_index: int) -> str:
    menu = "menuPage" if page == 1 else f"menuPage{page}"
    page_index = page - 1
    cond = f"slot !== null && slot.page === {page_index}"
    return f"""                TweaksTile {{
                    id: tweaksTile{page}
                    property var slot: firstEmptySlot({custom_index})
                    visible: {cond}
                    x: {menu}.x + ({cond} ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)
                    y: {menu}.y + ({cond} ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)
                    width: k_itemWidth
                    height: k_itemHeight
                    z: 7
                    onClicked: tweaksPage.visible = true
                }}
"""


def custom_tile_count_global(text: str) -> int:
    count = 0
    for pattern in (r"id:\s*helloTile1\b", r"id:\s*batteryTile1\b",
                    r"id:\s*wifiTile1\b", r"id:\s*tweaksTile1\b"):
        if re.search(pattern, text):
            count += 1
    return count


def patch_advanced_global(src: str) -> str:
    text = add_marker(read(src))
    text = patch_empty_fn(text)
    custom_index = custom_tile_count_global(text)
    pages = (
        (1, "menuPage", "wifiTile1", "batteryTile1", "helloTile1"),
        (2, "menuPage2", "wifiTile2", "batteryTile2", "helloTile2"),
        (3, "menuPage3", "wifiTile3", "batteryTile3", "helloTile3"),
    )
    for page, menu, wifi_id, battery_id, hello_id in pages:
        if f"id: tweaksTile{page}" in text:
            continue
        if f"id: {wifi_id}" in text:
            marker = f"                WifiTile {{\n                    id: {wifi_id}"
        elif f"id: {battery_id}" in text:
            marker = f"                BatteryTile {{\n                    id: {battery_id}"
        elif f"id: {hello_id}" in text:
            marker = f"                HelloTile {{\n                    id: {hello_id}"
        else:
            marker = f"                QMenuPageNoDrag{{\n                    id: {menu}"
        text = insert_after_block(text, marker, tile_advanced_global(page, custom_index))
    return add_page(text)


def shrink_wide(text: str) -> str:
    # Make room for up to 8 items on the wide home screen.
    text = text.replace("AppHomeModel.isWideScr ? 166 : 207",
                        "AppHomeModel.isWideScr ? 150 : 207")
    text = text.replace("AppHomeModel.isWideScr ? 250 : 207",
                        "AppHomeModel.isWideScr ? 150 : 207")
    text = text.replace("AppHomeModel.isWideScr ? 50 : 55",
                        "AppHomeModel.isWideScr ? 42 : 55")
    text = text.replace("AppHomeModel.isWideScr ? 76 : 55",
                        "AppHomeModel.isWideScr ? 42 : 55")
    text = text.replace("cellWidth: AppHomeModel.isWideScr ? 166 : 207",
                        "cellWidth: AppHomeModel.isWideScr ? 150 : 207")
    text = text.replace("cellWidth: AppHomeModel.isWideScr ? 250 : 207",
                        "cellWidth: AppHomeModel.isWideScr ? 150 : 207")
    return text


def tile_wide(xexpr: str) -> str:
    return f"""        QHomeItem {{
            id: itemTweaks
            visible: AppHomeModel.isWideScr
            width: AppHomeModel.isWideScr ? 150 : 207
            height: 183
            x: {xexpr}
            x_icon: AppHomeModel.isWideScr ? 42 : 55
            iconSource: ["file:///app/share/AppHomeScreen/tweaks/tweaks_icon_n.png", defineModel.home_bg_icon_p, "", "file:///app/share/AppHomeScreen/tweaks/tweaks_icon_d.png"]
            itemName: "Tweaks"
            icoPressed: tweaksMouse.pressed
            MouseArea {{
                id: tweaksMouse
                anchors.fill: parent
                onClicked: tweaksPage.visible = true
            }}
        }}

"""


def patch_wide(src: str) -> str:
    text = add_marker(read(src))
    text = shrink_wide(text)
    has_hello = "itemHello" in text
    has_wifi = "itemWifi" in text
    if "itemTweaks" not in text:
        if has_wifi:
            marker = "        QHomeItem {\n            id: itemWifi"
            xexpr = "itemWifi.right"
        elif has_hello:
            marker = "        QHomeItem {\n            id: itemHello"
            xexpr = "itemHello.right"
        else:
            marker = "        GridView {\n            id: mainGridItemMenu"
            xexpr = "itemAllMenu.right + mainGridItemMenu.cellWidth * 4"
        text = insert_after_block(text, marker, tile_wide(xexpr))
    return add_page(text)


def add_signal_handler(text: str) -> str:
    if "onTweaksClicked:" in text:
        return text
    if "onWifiClicked:" in text:
        pos = text.find("onWifiClicked:")
    elif "onHelloClicked:" in text:
        pos = text.find("onHelloClicked:")
    else:
        pos = text.find("onMenuItemClicked:")
    if pos < 0:
        raise ValueError("no menu click handler found")
    end = find_block_end(text, pos)
    return text[:end] + "\n            onTweaksClicked: tweaksPage.visible = true" + text[end:]


def patch_all_or_setup(src: str) -> str:
    text = add_marker(read(src))
    text = add_signal_handler(text)
    return add_page(text)


def patch_custom(src: str) -> str:
    text = add_marker(read(src))
    if "signal tweaksClicked()" not in text:
        if "signal wifiClicked()" in text:
            text = text.replace("    signal wifiClicked()\n",
                                "    signal wifiClicked()\n    signal tweaksClicked()\n", 1)
        elif "signal helloClicked()" in text:
            text = text.replace("    signal helloClicked()\n",
                                "    signal helloClicked()\n    signal tweaksClicked()\n", 1)
        else:
            text = text.replace("    id: root\n",
                                "    id: root\n    signal tweaksClicked()\n", 1)
    text = patch_empty_fn(text)
    has_hello = "HelloTile" in text
    has_battery = "BatteryTile" in text
    has_wifi = "WifiTile" in text
    skip = int(has_hello) + int(has_battery) + int(has_wifi)
    if "TweaksTile" not in text:
        tile = f"""            TweaksTile {{
                id: tweaksTile
                property int emptyIdx: root.firstEmptyIndex(menuPage.pageModel, {skip})
                visible: (isAllMenuScreen || listType === AppHome_Enum.E_LIST_SETUP_MAIN) && !customizeMode && index === menuList.currentIndex && emptyIdx >= 0
                x: menuPage.x + (emptyIdx % k_nbrOfColumns) * k_itemWidth
                y: menuPage.y + Math.floor(emptyIdx / k_nbrOfColumns) * k_itemHeight
                width: k_itemWidth
                height: k_itemHeight
                z: 7
                onClicked: root.tweaksClicked()
            }}
"""
        if has_wifi:
            marker = "            WifiTile {\n                id: wifiTile"
        elif has_hello:
            marker = "            HelloTile {\n                id: helloTile"
        else:
            marker = "            QMenuPageAllowEmpty{\n                id: menuPage"
        text = insert_after_block(text, marker, tile)
    return text


def patch_theme_image() -> None:
    src = os.path.join(BASE, "stock", "component", "common", "Q_ThemeImage.qml")
    text = read(src)
    if 'indexOf("file://")' not in text:
        old = 'source: themeSource !== "" ? (themeSource + AppHomeModel.dayLightSuffix) : themeSource'
        new = ('source: themeSource !== "" ? ((themeSource.indexOf("file://") === 0) ? '
               'themeSource : (themeSource + AppHomeModel.dayLightSuffix)) : themeSource')
        if old not in text:
            raise ValueError("Q_ThemeImage source line not found")
        text = text.replace(old, new, 1)
    write(os.path.join(OUT, "component", "common", "Q_ThemeImage.qml"), text)


def variant_path(variant: str, rel: str) -> str:
    return os.path.join(BASE, variant, rel)


def standby_marker(text: str) -> str:
    if "TWEAKS_DISCLAIMER_260128" in text:
        return text
    if text.startswith("//"):
        first, rest = text.split("\n", 1)
        return first + "\n" + STANDBY_MARK + rest
    return STANDBY_MARK + text


def insert_after_first_id_root(text: str, code: str) -> str:
    marker = "    id: root\n"
    pos = text.find(marker)
    if pos < 0:
        raise ValueError("id: root not found")
    end = pos + len(marker)
    return text[:end] + code + text[end:]


def insert_before_last_brace(text: str, code: str) -> str:
    pos = text.rstrip().rfind("}")
    if pos < 0:
        raise ValueError("no closing brace")
    return text[:pos] + "\n" + code + text[pos:]


STANDBY_MAIN_CODE = """    objectName: "AppStandbyClockRoot"
    property bool disclaimerAutoCancelled: false
    property bool disclaimerAutoDone: false
"""

STANDBY_MAIN_GUARD = """    // TWEAKS_DISCLAIMER_260128: any touch before auto-accept cancels it for this boot.
    MouseArea {
        id: disclaimerTouchGuard
        anchors.fill: parent
        z: 1001
        enabled: !root.disclaimerAutoDone
        propagateComposedEvents: true
        onPressed: {
            root.disclaimerAutoCancelled = true
            root.disclaimerAutoDone = true
            if (current_loader.item && current_loader.item.cancelAutoConfirm)
                current_loader.item.cancelAutoConfirm()
            mouse.accepted = false
        }
    }
"""

STANDBY_CONFIRM_CODE = r"""    property bool autoConfirmCancelled: false
    property int autoConfirmTimeout: 2000
    property int autoConfirmRetries: 0

    function autoAppRoot() {
        var p = root.parent
        while (p) {
            if (p.objectName === "AppStandbyClockRoot")
                return p
            p = p.parent
        }
        return null
    }

    function cancelAutoConfirm() {
        autoConfirmTimer.stop()
        root.autoConfirmCancelled = true
        var r = root.autoAppRoot()
        if (r) {
            r.disclaimerAutoCancelled = true
            r.disclaimerAutoDone = true
        }
    }

    function startAutoConfirm() {
        var r = root.autoAppRoot()
        if (r && (r.disclaimerAutoCancelled || r.disclaimerAutoDone))
            return
        var x = new XMLHttpRequest()
        x.onreadystatechange = function() {
            if (x.readyState !== XMLHttpRequest.DONE)
                return
            var enabled = false
            if (x.status === 200 || x.status === 0)
                enabled = String(x.responseText).replace(/\s+/g, "") === "1"
            if (!enabled)
                return
            var y = new XMLHttpRequest()
            y.onreadystatechange = function() {
                if (y.readyState !== XMLHttpRequest.DONE)
                    return
                var timeout = 2000
                if (y.status === 200 || y.status === 0) {
                    var n = parseInt(String(y.responseText).replace(/\s+/g, ""), 10)
                    if (!isNaN(n))
                        timeout = Math.max(1000, Math.min(10000, n))
                }
                root.autoConfirmTimeout = timeout
                root.autoConfirmRetries = 0
                autoConfirmTimer.interval = timeout
                autoConfirmTimer.start()
            }
            y.open("GET", "file:///rw_data/tweaks/disclaimer_timeout_ms", true)
            y.send()
        }
        x.open("GET", "file:///rw_data/tweaks/disclaimer_auto", true)
        x.send()
    }

    function autoConfirmAllowed() {
        if (root.autoConfirmCancelled)
            return false
        var r = root.autoAppRoot()
        if (r && (r.disclaimerAutoCancelled || r.disclaimerAutoDone))
            return false
        if (typeof AppStandbyClockModel.drivingRestriction !== "undefined" &&
            AppStandbyClockModel.drivingRestriction)
            return false
        if (typeof AppStandbyClockModel.isCalling !== "undefined" &&
            AppStandbyClockModel.isCalling)
            return false
        return true
    }

    function confirmAutoAccept() {
        if (!root.autoConfirmAllowed())
            return
        if (typeof AppStandbyClockModel.enableBtn !== "undefined" && !AppStandbyClockModel.enableBtn) {
            if (root.autoConfirmRetries < 20) {
                root.autoConfirmRetries++
                autoConfirmTimer.interval = 250
                autoConfirmTimer.start()
            }
            return
        }
        var r = root.autoAppRoot()
        if (r)
            r.disclaimerAutoDone = true
        if (typeof root.isPressedConfirm !== "undefined")
            root.isPressedConfirm = true
        QML_Handler.qmlSendEvtKey(HMIEVENTS.KEY_EVT_CONFIRMSCREEN_BUTTON_CONFIRM_CLICKED)
    }

    Timer {
        id: autoConfirmTimer
        interval: root.autoConfirmTimeout
        running: false
        repeat: false
        onTriggered: root.confirmAutoAccept()
    }

    Timer {
        interval: 1
        running: true
        repeat: false
        onTriggered: root.startAutoConfirm()
    }
"""


def patch_standby_main(src: str) -> str:
    text = standby_marker(read(src))
    if "AppStandbyClockRoot" not in text:
        text = insert_after_first_id_root(text, STANDBY_MAIN_CODE)
    if "disclaimerTouchGuard" not in text:
        text = insert_before_last_brace(text, STANDBY_MAIN_GUARD)
    return text


def patch_standby_confirm(src: str) -> str:
    text = standby_marker(read(src))
    if "autoConfirmTimer" not in text:
        text = insert_before_last_brace(text, STANDBY_CONFIRM_CODE)
    return text


def build_standbyclock() -> None:
    if not os.path.isdir(STANDBY_SRC):
        print(f"standbyclock source not found, skipping: {STANDBY_SRC}", file=sys.stderr)
        return
    shutil.rmtree(STANDBY_OUT, ignore_errors=True)
    for base in STANDBY_BASES:
        src_dir = os.path.join(STANDBY_SRC, base)
        if not os.path.isdir(src_dir):
            continue
        out_dir = os.path.join(STANDBY_OUT, base)
        for name, patch in (
            ("main.qml", patch_standby_main),
            ("Screen_AppStandbyClock_Confirm.qml", patch_standby_confirm),
            ("Screen_AppStandbyClock_Confirm_Profile.qml", patch_standby_confirm),
        ):
            src = os.path.join(src_dir, name)
            if not os.path.exists(src):
                continue
            write(os.path.join(out_dir, name), patch(src))
    print("standbyclock artifacts written to", STANDBY_OUT)


def main() -> int:
    patch_theme_image()

    adv_variants = ["stock", "hello", "battery", "battery_hello",
                    "wifi", "hello_wifi", "battery_wifi", "hello_battery_wifi"]
    for v in adv_variants:
        write(os.path.join(OUT, "HomeScreen", f"QHMI_HomeScreenMain_Advanced.{v}.qml"),
              patch_advanced(variant_path(v, ADV)))

    # New Battery-tile layout: use the global allocator used by the current
    # Battery/WiFi payloads, not the per-page helper.
    global_adv_variants = ["battery_tile", "battery_tile_hello",
                           "battery_tile_wifi", "hello_battery_tile_wifi"]
    for v in global_adv_variants:
        write(os.path.join(OUT, "HomeScreen", f"QHMI_HomeScreenMain_Advanced.{v}.qml"),
              patch_advanced_global(variant_path(v, ADV)))

    wide_variants = ["stock", "hello", "wifi", "hello_wifi"]
    for v in wide_variants:
        write(os.path.join(OUT, "HomeScreen", f"QHMI_HomeScreenMain_Wide.{v}.qml"),
              patch_wide(variant_path(v, WIDE)))

    all_variants = ["stock", "hello", "wifi", "hello_wifi"]
    for v in all_variants:
        write(os.path.join(OUT, "HomeScreen", f"Screen_HomeScreen_AllMenus.{v}.qml"),
              patch_all_or_setup(variant_path(v, ALL)))
        write(os.path.join(OUT, "HomeScreen", f"Screen_HomeScreen_Submenu_Setup.{v}.qml"),
              patch_all_or_setup(variant_path(v, SETUP)))
        write(os.path.join(OUT, "component", f"QCustomizedMenuListAllowEmpty.{v}.qml"),
              patch_custom(variant_path(v, CUSTOM)))

    build_standbyclock()
    print("artifacts written to", OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
