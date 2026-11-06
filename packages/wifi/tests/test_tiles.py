#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Static checks for the home-screen tile allocator and theme fixes."""
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
QML = ROOT / "artifacts/qml/AppHomeScreen/qml_eco3.0"
COMP = QML / "component"


def text(rel: str) -> str:
    return (QML / rel).read_text(encoding="utf-8")


def require(cond: bool, msg: str) -> None:
    if not cond:
        raise AssertionError(msg)


def image_is_blue(path: Path) -> bool:
    im = Image.open(path).convert("RGBA")
    px = [p for p in im.getdata() if p[3] > 0]
    if not px:
        return False
    avg = tuple(sum(p[i] for p in px) // len(px) for i in range(3))
    return avg[2] > avg[0] + 80 and avg[2] > avg[1] + 40


def main() -> int:
    combined = text("HomeScreen/QHMI_HomeScreenMain_Advanced.hello_battery_wifi.qml")
    require(combined.count("WifiTile {") == 3, "WiFi tile must be present on all three pages")
    require(combined.count("HelloTile {") == 3, "Hello tile must be present on all three pages")
    require("BatteryTile {" not in combined,
            "current page-based Battery base must not reference BatteryTile")
    require("id: batteryPage" in combined, "Battery page is missing from the combined base")
    require("firstEmptySlot(1)" in combined, "WiFi should follow Hello when Battery is a page")
    require("firstEmptySlot(0)" in combined, "Hello should be first custom tile")
    require("slot !== null && slot.page === 0" in combined, "page 1 slot check missing")
    require("slot !== null && slot.page === 1" in combined, "page 2 slot check missing")
    require("slot !== null && slot.page === 2" in combined, "page 3 slot check missing")
    require("totalPageCount: totalPagesNormalMode + 2" in combined,
            "WiFi must not add a whole page (Battery page count is +2)")
    require("color: \"#FFFFFF\"" not in combined, "no hardcoded white text in generated QML")

    tile_combined = text("HomeScreen/QHMI_HomeScreenMain_Advanced.hello_battery_tile_wifi.qml")
    require(tile_combined.count("WifiTile {") == 3, "tile-based WiFi must be on all pages")
    require(tile_combined.count("BatteryTile {") == 3, "tile-based Battery must be on all pages")
    require(tile_combined.count("HelloTile {") == 3, "tile-based Hello must be on all pages")
    require("firstEmptySlot(2)" in tile_combined, "WiFi should follow Hello + Battery tile")
    require("firstEmptySlot(1)" in tile_combined, "Battery tile should follow Hello")
    require("id: batteryPage" not in tile_combined, "tile-based Battery must not add a page")

    tile_battery = text("HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile_wifi.qml")
    require("firstEmptySlot(1)" in tile_battery, "WiFi should follow Battery tile")
    require("firstEmptySlot(0)" in tile_battery, "Battery tile should be first without Hello")
    require("id: batteryPage" not in tile_battery, "tile-based Battery must not add a page")

    battery_wifi = text("HomeScreen/QHMI_HomeScreenMain_Advanced.battery_wifi.qml")
    require("firstEmptySlot(0)" in battery_wifi, "WiFi is the first custom tile without Hello")
    require("BatteryTile {" not in battery_wifi, "no Battery tile in a page-based Battery base")

    hello_wifi = text("HomeScreen/QHMI_HomeScreenMain_Advanced.hello_wifi.qml")
    require("firstEmptySlot(1)" in hello_wifi, "WiFi should follow Hello")
    require("BatteryTile {" not in hello_wifi, "no Battery tile in a Hello-only base")

    wide_stock = text("HomeScreen/QHMI_HomeScreenMain_Wide.wifi.qml")
    require("cellWidth: AppHomeModel.isWideScr ? 200 : 207" in wide_stock,
            "stock Wide cell width should be 200")
    require("id: itemWifi" in wide_stock, "Wide stock WiFi tile missing")

    wide_hello = text("HomeScreen/QHMI_HomeScreenMain_Wide.hello_wifi.qml")
    require("cellWidth: AppHomeModel.isWideScr ? 166 : 207" in wide_hello,
            "Hello Wide cell width should be 166")
    require("id: itemWifi" in wide_hello, "Wide Hello WiFi tile missing")

    for name in ("WifiTile.qml", "HelloTile.qml", "BatteryTile.qml"):
        path = COMP / name
        require(path.exists(), f"missing tile component: {name}")
        body = path.read_text(encoding="utf-8")
        require('color: "#FFFFFF"' not in body, f"{name}: hardcoded white text")
    wifi_tile = (COMP / "WifiTile.qml").read_text(encoding="utf-8")
    require("anchors.horizontalCenter: parent.horizontalCenter" in wifi_tile,
            "WiFi icon must be horizontally centered")

    require((ROOT / "docs/UI-TILES.md").exists(), "UI-TILES.md missing")
    require("firstEmptySlot" in (ROOT / "scripts/run_stuff_diag.sh").read_text(encoding="utf-8"),
            "diagnostic script should report firstEmptySlot")
    require('mode=client' not in (ROOT / "scripts/run_stuff.sh").read_text(encoding="utf-8"),
            "installer must not auto-enable client mode")

    # UX: full-screen page must sit below the 90px status bar, consume
    # clicks and close on the steering-wheel Home/Back key.
    page = (QML / "HomeScreen/WifiPage.qml").read_text(encoding="utf-8")
    require("height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y" in page,
            "WifiPage must be laid out for 630px below the bar")
    require("height: 720" not in page, "WifiPage must not use the full 720px")
    require("u2715  Exit" in page, "Exit button missing or not prominent")
    # Header clearance: the page starts at screen y=90, so y=26 puts the
    # exit button at 116 - comfortably below the status bar.
    require("id: closeButton" in page and "y: 26" in page,
            "Exit button must clear the 90px bar with margin")
    require("id: tabRow" in page and "y: 110" in page,
            "content tabs must clear the status bar")
    require("id: consolePanel" in page and "y: 20" in page,
            "console panel must clear the status bar")
    require("Start CarPlay AP" in page, "CarPlay AP button missing")
    require("Save logs" in page and "/logs/dump" in page,
            "log dump button missing from the Status tab")
    require("consolePanel" in page and "showConsole" in page,
            "live log console missing")
    require("root.showConsole ? 16" in page,
            "content is not shifted left for the console")
    require("Live log" in page, "console header missing")
    require("Stop pairing" in page, "CarPlay pairing stop button missing")
    require("/carplay/pair" in page, "CarPlay pairing endpoint not wired")
    require("btDiscoverable" in page and "pairMessage" in page,
            "CarPlay pairing status properties missing")

    require("property bool requested" in page,
            "overlay is missing the requested state")
    require("visible: requested && !atOtherApp" in page,
            "overlay visibility is not gated on request and foreground")
    require("function open()" in page and "function close()" in page,
            "overlay does not expose open()/close()")
    require("onButtonBackClicked: root.close()" in page,
            "overlay does not close on the back affordance")
    require("onIsAtOtherAppChanged" in page, "overlay does not close on app switch")
    require("onCurrentScreenChanged" in page, "overlay does not close on screen change")
    require("onCurrentHomeAdvancedPageIdxChanged" in page,
            "overlay does not close on page change")
    require("onRequestHomeByPPChanged" in page,
            "overlay does not close on requestHomeByPP")
    require("onHomeSeqChanged" in page and "homeSeq" in page,
            "overlay does not consume the daemon home_seq")
    require("preventStealing: true" in page, "input shield cannot prevent stealing")
    require("acceptedButtons: Qt.AllButtons" in page,
            "input shield does not accept all buttons")
    require("onWheel: wheel.accepted = true" in page,
            "input shield does not consume the wheel")
    require("z: 1" in page and "id: content" in page and "z: 2" in page,
            "content is not above the input shield")
    require("y: ConstantModel.MENU_BAR_Y" not in page, "page itself must not offset; hosts do")

    for rel in ("HomeScreen/QHMI_HomeScreenMain_Advanced.hello_battery_wifi.qml",
                "HomeScreen/QHMI_HomeScreenMain_Advanced.hello_wifi.qml",
                "HomeScreen/QHMI_HomeScreenMain_Wide.hello_wifi.qml",
                "HomeScreen/QHMI_HomeScreenMain_Wide.wifi.qml",
                "HomeScreen/Screen_HomeScreen_AllMenus.hello_wifi.qml",
                "HomeScreen/Screen_HomeScreen_Submenu_Setup.hello_wifi.qml"):
        host = text(rel)
        require("y: ConstantModel.MENU_BAR_Y" in host, f"{rel}: page must start below the status bar")
        require("height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y" in host,
                f"{rel}: page must stop above the bottom edge")
        require("z: 2000" in host, f"{rel}: page must be on top of the home screen")
        require("wifiPage.open()" in host,
                f"{rel}: tiles/menu must call the overlay's open()")
        require("wifiPage.visible" not in host,
                f"{rel}: hosts must not assign the overlay's visible state")
        require("visible: false\n        z: 2000" not in host,
                f"{rel}: host still overrides the overlay's visibility")

    require(image_is_blue(ROOT / "artifacts/assets/wifi/wifi_icon_n.png"), "WiFi icon is not blue")
    require(image_is_blue(ROOT / "artifacts/assets/hello/hello_icon_n.png"), "Hello icon is not blue")
    require(image_is_blue(ROOT / "artifacts/assets/battery/battery_icon_n.png"), "Battery icon is not blue")

    print("PASS: tile allocator, page placement and theme checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
