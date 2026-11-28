#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Build the Projection+ home-screen patch for AppHomeScreen/qml_eco3.0.

Status: WIP; car-run once on one AU 260319 car (install, tile, status); restore VM-only (see docs/STATUS.md).

Input is the owner's own pulled home-screen QML (git-ignored OEM content):
    $PROJECTION_BASE/qml_eco3.0/...       (default: ../base/car_2026-11-27)
    $PROJECTION_BASE/car_sha256.txt       (sha256 of every pulled file)

That base already carries the WiFi, Tweaks and Reboot payloads.  This script
adds, strictly additively and following the same patterns:

  * one ProjectionTile per place a TweaksTile/WifiTile/RebootTile appears,
    in the next free slot after the existing custom tiles;
  * one ProjectionPage instance per host screen, opened by the tile.

Only changed layout files are written, to ../build/qml_eco3.0/ (git-ignored),
plus ../build/manifest.txt for the installer.  Every anchor must be found
exactly where expected; otherwise the build stops instead of guessing.

usage: build_artifacts.py            build + manifest
       build_artifacts.py --self-check   also assert patch(patch(x)) == patch(x)
"""
from __future__ import annotations

import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BASE = os.environ.get("PROJECTION_BASE",
                      os.path.join(ROOT, "base", "car_2026-11-27"))
BASE_QML = os.path.join(BASE, "qml_eco3.0")
BASE_HASHES = os.path.join(BASE, "car_sha256.txt")
BUILD = os.path.join(ROOT, "build")
OUT = os.path.join(BUILD, "qml_eco3.0")
ART_QML = os.path.join(ROOT, "artifacts", "qml", "AppHomeScreen", "qml_eco3.0")
ART_ICONS = os.path.join(ROOT, "artifacts", "assets", "projection")

DEST_QML = "/app/share/AppHomeScreen/qml_eco3.0"
DEST_ICONS = "/app/share/AppHomeScreen/projection"

MARK_NAME = "PROJECTION_PAYLOAD_260319"
MARK = "// " + MARK_NAME

ADV = "HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
WIDE = "HomeScreen/QHMI_HomeScreenMain_Wide.qml"
ALL = "HomeScreen/Screen_HomeScreen_AllMenus.qml"
SETUP = "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
CUSTOM = "component/QCustomizedMenuListAllowEmpty.qml"


class AnchorError(RuntimeError):
    """An expected anchor is missing or ambiguous: stop, do not guess."""


# ---------------------------------------------------------------- helpers --

def read(path: str) -> str:
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def load_base_hashes() -> dict[str, str]:
    if not os.path.isfile(BASE_HASHES):
        raise AnchorError(f"base hash list missing: {BASE_HASHES}")
    out = {}
    for line in read(BASE_HASHES).splitlines():
        parts = line.split(None, 1)
        if len(parts) == 2:
            out[parts[1].strip().lstrip("./")] = parts[0]
    return out


def find_block_end(text: str, start: int) -> int:
    """Index just after the brace matching the first '{' at/after start."""
    i = text.find("{", start)
    if i < 0:
        raise AnchorError("no opening brace after anchor")
    depth = 0
    in_str = None
    escaped = False
    line_c = block_c = False
    j = i
    while j < len(text):
        c = text[j]
        nxt = text[j + 1] if j + 1 < len(text) else ""
        if line_c:
            line_c = c != "\n"
        elif block_c:
            if c == "*" and nxt == "/":
                block_c = False
                j += 1
        elif in_str:
            if escaped:
                escaped = False
            elif c == "\\":
                escaped = True
            elif c == in_str:
                in_str = None
        elif c == "/" and nxt == "/":
            line_c = True
            j += 1
        elif c == "/" and nxt == "*":
            block_c = True
            j += 1
        elif c in ("'", '"'):
            in_str = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return j + 1
        j += 1
    raise AnchorError("unbalanced block after anchor")


def unique(text: str, anchor: str, what: str) -> int:
    n = text.count(anchor)
    if n != 1:
        raise AnchorError(f"{what}: expected exactly 1 anchor {anchor!r}, found {n}")
    return text.find(anchor)


def insert_after_block(text: str, anchor: str, insertion: str, what: str) -> str:
    end = find_block_end(text, unique(text, anchor, what))
    return text[:end] + "\n" + insertion.rstrip("\n") + text[end:]


def insert_after_line(text: str, anchor_line: str, insertion: str, what: str) -> str:
    pos = unique(text, anchor_line, what) + len(anchor_line)
    return text[:pos] + insertion + text[pos:]


def add_marker(text: str) -> str:
    """Append our marker after the leading block of '// *_PAYLOAD_*' lines."""
    if MARK_NAME in text:
        return text
    lines = text.split("\n")
    k = 0
    while k < len(lines) and re.match(r"^// [A-Z]+_PAYLOAD_\d+\s*$", lines[k]):
        k += 1
    if k == 0:
        raise AnchorError("no existing *_PAYLOAD_* marker block at top of file")
    lines.insert(k, MARK)
    return "\n".join(lines)


# -------------------------------------------------------------- snippets --

def page_instance() -> str:
    # Same host wiring as WifiPage / RebootPage in the car base.
    return """
    ProjectionPage {
        id: projectionPage
        x: 0
        y: ConstantModel.MENU_BAR_Y
        width: AppHomeModel.isWideScr ? DEVICE_WIDTH : ConstantModel.SCREEN_WIDTH
        height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y
        z: 2000
    }
"""


def add_page(text: str, what: str) -> str:
    if "id: projectionPage" in text:
        return text
    return insert_after_block(text, "    RebootPage {\n        id: rebootPage\n",
                              page_instance(), what + " page")


def adv_tile(page: int, custom_index: int) -> str:
    menu = "menuPage" if page == 1 else f"menuPage{page}"
    p = page - 1
    cond = f"slot !== null && slot !== undefined && slot.page === {p}"
    return f"""                ProjectionTile {{
                    id: projectionTile{page}
                    property var slot: root.firstEmptySlot({custom_index})
                    visible: {cond}
                    x: {menu}.x + ({cond} ? (slot.index % k_nbrOfColumns) * k_itemWidth : 0)
                    y: {menu}.y + ({cond} ? Math.floor(slot.index / k_nbrOfColumns) * k_itemHeight : 0)
                    width: k_itemWidth
                    height: k_itemHeight
                    z: 7
                    onClicked: projectionPage.open()
                }}
"""


# ---------------------------------------------------------------- patches --

def patch_advanced(text: str) -> str:
    text = add_marker(text)
    if "function firstEmptySlot(" not in text:
        raise AnchorError("Advanced: global allocator firstEmptySlot() missing")
    for page in (1, 2, 3):
        if f"id: projectionTile{page}\n" in text:
            continue
        anchor = f"                RebootTile {{\n                    id: rebootTile{page}\n"
        start = unique(text, anchor, f"Advanced rebootTile{page}")
        # Next global custom index = one past every index already taken on this page.
        page_start = text.rfind("QMenuPageNoDrag{", 0, start)
        used = [int(n) for n in re.findall(r"root\.firstEmptySlot\((\d+)\)",
                                            text[page_start:find_block_end(text, start)])]
        if sorted(used) != list(range(len(used))) or not used:
            raise AnchorError(f"Advanced page {page}: custom indices not contiguous: {used}")
        text = insert_after_block(text, anchor, adv_tile(page, len(used)),
                                  f"Advanced rebootTile{page}")
    return add_page(text, "Advanced")


def patch_all_or_setup(text: str, what: str) -> str:
    text = add_marker(text)
    if "onProjectionClicked:" not in text:
        text = insert_after_line(text, "            onRebootClicked: rebootPage.open()\n",
                                 "            onProjectionClicked: projectionPage.open()\n",
                                 what + " onRebootClicked")
    return add_page(text, what)


def patch_custom(text: str) -> str:
    text = add_marker(text)
    if "signal projectionClicked()" not in text:
        text = insert_after_line(text, "    signal rebootClicked()\n",
                                 "    signal projectionClicked()\n", "custom signal")
    if "id: projectionTile\n" not in text:
        anchor = "            RebootTile {\n                id: rebootTile\n"
        start = unique(text, anchor, "custom rebootTile")
        used = [int(n) for n in re.findall(
            r"root\.firstEmptyIndex\(menuPage\.pageModel, (\d+)\)",
            text[:find_block_end(text, start)])]
        if sorted(used) != list(range(len(used))) or not used:
            raise AnchorError(f"custom list: skip indices not contiguous: {used}")
        tile = f"""            ProjectionTile {{
                id: projectionTile
                property int emptyIdx: root.firstEmptyIndex(menuPage.pageModel, {len(used)})
                visible: (isAllMenuScreen || listType === AppHome_Enum.E_LIST_SETUP_MAIN) && !customizeMode && index === menuList.currentIndex && emptyIdx >= 0
                x: menuPage.x + (emptyIdx % k_nbrOfColumns) * k_itemWidth
                y: menuPage.y + Math.floor(emptyIdx / k_nbrOfColumns) * k_itemHeight
                width: k_itemWidth
                height: k_itemHeight
                z: 7
                onClicked: root.projectionClicked()
            }}
"""
        text = insert_after_block(text, anchor, tile, "custom rebootTile")
    return text


def check_wide(text: str) -> None:
    """Basic home: refuse to place a tile; report the layout question.

    The strip is HOME_1920_MENU_W (=627+627+12 = 1266 px per docs/ui).  The
    car base already has All Menu (200) + 4 grid cells (800) + WiFi (200) =
    1204, Tweaks 1204..1354 and Reboot 1354..1504, i.e. Tweaks overruns the
    strip and Reboot is past it.  There is no free cell, so Wide is NOT
    patched; Projection+ stays reachable there via All Menus."""
    unique(text, "        QHomeItem {\n            id: itemReboot\n", "Wide itemReboot")
    unique(text, "            x: itemTweaks.right\n", "Wide itemReboot x")
    if "itemProjection" in text:
        raise AnchorError("Wide base already has itemProjection: unexpected base")


PATCHES = (
    (ADV, patch_advanced),
    (ALL, lambda t: patch_all_or_setup(t, "AllMenus")),
    (SETUP, lambda t: patch_all_or_setup(t, "Setup")),
    (CUSTOM, patch_custom),
)


# ------------------------------------------------------------------ main --

def own_files() -> list[tuple[str, str]]:
    """(source path, install destination) for our own, unpatched files."""
    files = []
    for sub in ("HomeScreen", "component"):
        d = os.path.join(ART_QML, sub)
        for name in sorted(os.listdir(d)):
            if name.startswith("Projection") and name.endswith(".qml"):
                files.append((os.path.join(d, name), f"{DEST_QML}/{sub}/{name}"))
    for name in ("projection_icon_n.png", "projection_icon_d.png"):
        p = os.path.join(ART_ICONS, name)
        if not os.path.isfile(p):
            raise AnchorError(f"icon missing: {p}")
        files.append((p, f"{DEST_ICONS}/{name}"))
    return files


def main(argv: list[str]) -> int:
    self_check = "--self-check" in argv
    hashes = load_base_hashes()
    rows = []
    for rel, fn in PATCHES:
        src = os.path.join(BASE_QML, rel)
        want = hashes.get(rel)
        got = sha256_file(src)
        if want is None or want != got:
            raise AnchorError(f"base file {rel} sha256 {got} != car_sha256.txt {want}")
        base = read(src)
        new = fn(base)
        if new == base:
            raise AnchorError(f"{rel}: patch produced no change")
        if self_check and fn(new) != new:
            raise AnchorError(f"{rel}: patch is not idempotent")
        out = os.path.join(OUT, rel)
        write(out, new)
        rows.append(("patch", f"{DEST_QML}/{rel}", os.path.relpath(out, BUILD),
                     sha256_file(out), want))
    check_wide(read(os.path.join(BASE_QML, WIDE)))
    for src, dest in own_files():
        rows.append(("file", dest, os.path.relpath(src, BUILD), sha256_file(src), "-"))

    lines = [
        "# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors",
        "# SPDX-License-Identifier: AGPL-3.0-only",
        f"# Projection+ install manifest ({MARK_NAME}). Status: offline-rendered, NOT car-validated.",
        "# kind  dest  source(relative to build/)  sha256(source)  sha256(car base file it replaces, '-' = new file)",
        "# Installer: refuse a 'patch' row unless the car's current dest sha256 == the last column.",
        "# Not patched: " + WIDE + " (Basic home: no free cell; see BUILD_NOTES).",
    ]
    lines += ["\t".join(r) for r in rows]
    write(os.path.join(BUILD, "manifest.txt"), "\n".join(lines) + "\n")
    for r in rows:
        print(f"{r[0]:5} {r[1]}")
    print("written to", BUILD)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except AnchorError as e:
        print("REFUSED:", e, file=sys.stderr)
        sys.exit(2)
