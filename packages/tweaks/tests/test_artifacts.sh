#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Static checks on the generated QML variants.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
Q="$ROOT/artifacts/qml/AppHomeScreen/qml_eco3.0"
ADV="$Q/HomeScreen"
COMP="$Q/component"

check_adv() {
    file="$1"
    skips="$2"
    for n in 1 2 3; do
        grep -q "id: tweaksTile$n" "$file" || { echo "missing tweaksTile$n in $file" >&2; exit 1; }
    done
    grep -q "function tweaksTargetPage()" "$file" || { echo "missing target fn in $file" >&2; exit 1; }
    grep -q "visible: allMenuIndex === tweaksTargetPage()" "$file" || { echo "missing page visibility in $file" >&2; exit 1; }
    grep -Fq "property var tweaksSkips: $skips" "$file" || {
        echo "unexpected skips in $file (expected $skips)" >&2
        grep 'property var tweaksSkips' "$file" >&2 || true
        exit 1
    }
}

check_adv "$ADV/QHMI_HomeScreenMain_Advanced.stock.qml" "[0, 0, 0]"
check_adv "$ADV/QHMI_HomeScreenMain_Advanced.battery.qml" "[0, 0, 0]"
check_adv "$ADV/QHMI_HomeScreenMain_Advanced.hello.qml" "[1, 1, 1]"
check_adv "$ADV/QHMI_HomeScreenMain_Advanced.battery_hello.qml" "[1, 1, 1]"
check_adv "$ADV/QHMI_HomeScreenMain_Advanced.wifi.qml" "[1, 0, 0]"
check_adv "$ADV/QHMI_HomeScreenMain_Advanced.battery_wifi.qml" "[1, 0, 0]"
check_adv "$ADV/QHMI_HomeScreenMain_Advanced.hello_wifi.qml" "[2, 1, 1]"
check_adv "$ADV/QHMI_HomeScreenMain_Advanced.hello_battery_wifi.qml" "[2, 1, 1]"

check_adv_global() {
    file="$1"
    skip="$2"
    for n in 1 2 3; do
        grep -q "id: tweaksTile$n" "$file" || { echo "missing tweaksTile$n in $file" >&2; exit 1; }
        grep -q "visible: slot !== null && slot.page === $((n - 1))" "$file" || {
            echo "missing global page visibility tweaksTile$n in $file" >&2; exit 1; }
    done
    [ "$(grep -c 'function firstEmptySlot' "$file")" = "1" ] || {
        echo "unexpected firstEmptySlot count in $file" >&2; exit 1; }
    grep -Fq "property var slot: firstEmptySlot($skip)" "$file" || {
        echo "unexpected global slot index in $file (expected $skip)" >&2; exit 1; }
    if grep -q 'tweaksTargetPage' "$file"; then
        echo "old per-page helper leaked into $file" >&2; exit 1
    fi
}

check_adv_global "$ADV/QHMI_HomeScreenMain_Advanced.battery_tile.qml" 1
check_adv_global "$ADV/QHMI_HomeScreenMain_Advanced.battery_tile_hello.qml" 2
check_adv_global "$ADV/QHMI_HomeScreenMain_Advanced.battery_tile_wifi.qml" 2
check_adv_global "$ADV/QHMI_HomeScreenMain_Advanced.hello_battery_tile_wifi.qml" 3

for f in "$ADV"/QHMI_HomeScreenMain_Wide.*.qml; do
    grep -q 'id: itemTweaks' "$f" || { echo "missing wide tile in $f" >&2; exit 1; }
done
for f in "$ADV"/Screen_HomeScreen_AllMenus.*.qml "$ADV"/Screen_HomeScreen_Submenu_Setup.*.qml; do
    grep -q 'onTweaksClicked:' "$f" || { echo "missing handler in $f" >&2; exit 1; }
    grep -q 'id: tweaksPage' "$f" || { echo "missing page in $f" >&2; exit 1; }
done
for f in "$COMP"/QCustomizedMenuListAllowEmpty.*.qml; do
    grep -q 'TweaksTile' "$f" || { echo "missing tile in $f" >&2; exit 1; }
    grep -q 'signal tweaksClicked()' "$f" || { echo "missing signal in $f" >&2; exit 1; }
done
test -f "$ADV/TweaksPage.qml"
test -f "$COMP/TweaksZonePicker.qml"

grep -q 'anchors.horizontalCenter: parent.horizontalCenter' "$COMP/TweaksTile.qml" || {
    echo "Tweaks tile icon is not horizontally centered" >&2; exit 1; }
python3 - "$ADV/TweaksPage.qml" <<'PY'
import re, sys
text = open(sys.argv[1]).read()

def pos(name):
    m = re.search(r"id:\s*" + name + r"\b", text)
    assert m, "missing " + name
    seg = text[m.start():m.start() + 600]
    y = int(re.search(r"\by:\s*(\d+)", seg).group(1))
    h = re.search(r"\bheight:\s*(\d+)", seg)
    return y, (int(h.group(1)) if h else None)

dy, dh = pos("deepRedCard")
ay, _ = pos("actions")
dgy, _ = pos("diag")
assert dh is not None and dy + dh <= ay, ("deep red overlaps actions", dy, dh, ay)
assert ay + 44 <= dgy, ("actions overlap diagnostics", ay, dgy)
assert dgy <= 610, ("diagnostics below content", dgy)
assert "Identify CRTC 2" not in text, "cluster is not a CRTC; remove that button"
assert "Identify front" in text and '"/identify?crtc=" + root.mainCrtc' in text, "missing front identify"
PY
grep -q 'y: ConstantModel.MENU_BAR_Y' "$ADV/TweaksPage.qml" || {
    echo "Tweaks page does not respect the status bar" >&2; exit 1; }
grep -q 'height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y' "$ADV/TweaksPage.qml" || {
    echo "Tweaks page height does not fit below the status bar" >&2; exit 1; }
[ "$(grep -c 'anchors.fill: parent' "$ADV/TweaksPage.qml")" -ge 2 ] || {
    echo "Tweaks page lacks a full-page click guard" >&2; exit 1; }
[ "$(grep -c 'anchors.fill: parent' "$COMP/TweaksZonePicker.qml")" -ge 2 ] || {
    echo "Zone picker lacks a full-page click guard" >&2; exit 1; }

for f in "$ADV"/*.qml; do
    if grep -q 'id: tweaksPage' "$f"; then
        grep -q 'TWEAKS_BACK_CLOSE' "$f" || {
            echo "missing Home/Back close handler in $f" >&2; exit 1; }
    fi
done

# ---- Reboot fourth app -------------------------------------------------
for f in "$ADV"/QHMI_HomeScreenMain_Advanced.*.qml; do
    [ "$(grep -c 'RebootTile {' "$f")" = 3 ] || {
        echo "Reboot tile missing from all pages in $f" >&2; exit 1; }
    for n in 1 2 3; do
        grep -q "id: rebootTile$n" "$f" || {
            echo "missing rebootTile$n in $f" >&2; exit 1; }
        grep -q "visible: slot !== null && slot.page === $((n - 1))" "$f" || {
            echo "missing Reboot slot guard $n in $f" >&2; exit 1; }
    done
    grep -q 'onClicked: rebootPage.open()' "$f" || {
        echo "Reboot tile does not open the modal in $f" >&2; exit 1; }
    grep -q 'id: rebootPage' "$f" || {
        echo "missing RebootPage declaration in $f" >&2; exit 1; }
    grep -q 'onGoHome: close()' "$f" || {
        echo "missing Reboot goHome close in $f" >&2; exit 1; }
    if grep -q 'rebootPage.visible' "$f"; then
        echo "host assigns rebootPage.visible in $f" >&2; exit 1
    fi
    grep -q 'REBOOT_PAYLOAD_260128' "$f" || {
        echo "missing Reboot marker in $f" >&2; exit 1; }
done

for f in "$ADV"/QHMI_HomeScreenMain_Wide.*.qml; do
    grep -q 'id: itemReboot' "$f" || {
        echo "missing Reboot wide tile in $f" >&2; exit 1; }
    grep -q 'rebootPage.open()' "$f" || {
        echo "wide Reboot tile does not open the modal in $f" >&2; exit 1; }
    grep -q 'id: rebootPage' "$f" || {
        echo "missing RebootPage in $f" >&2; exit 1; }
done
for f in "$ADV"/Screen_HomeScreen_AllMenus.*.qml "$ADV"/Screen_HomeScreen_Submenu_Setup.*.qml; do
    grep -q 'onRebootClicked:' "$f" || {
        echo "missing Reboot handler in $f" >&2; exit 1; }
    grep -q 'id: rebootPage' "$f" || {
        echo "missing RebootPage in $f" >&2; exit 1; }
done
for f in "$COMP"/QCustomizedMenuListAllowEmpty.*.qml; do
    grep -q 'RebootTile' "$f" || {
        echo "missing RebootTile in $f" >&2; exit 1; }
    grep -q 'signal rebootClicked()' "$f" || {
        echo "missing rebootClicked signal in $f" >&2; exit 1; }
done

test -f "$ADV/RebootPage.qml"
test -f "$COMP/RebootTile.qml"
python3 - "$ADV/RebootPage.qml" "$COMP/RebootTile.qml" <<'PY'
import re, sys
page = open(sys.argv[1]).read()
tile = open(sys.argv[2]).read()

def need(cond, msg):
    if not cond:
        raise AssertionError(msg)

need("height: DEVICE_HEIGHT - ConstantModel.MENU_BAR_Y" in page,
     "RebootPage does not fit under the status bar")
need("height: 720" not in page, "RebootPage uses full screen height")
need("visible: requested && !atOtherApp" in page,
     "RebootPage is not foreground-gated")
need("function open()" in page and "function close()" in page,
     "RebootPage lacks open()/close()")
need("onButtonBackClicked: root.close()" in page,
     "RebootPage does not close on Back")
need("onIsAtOtherAppChanged" in page, "RebootPage does not close on app switch")
need("onCurrentScreenChanged" in page, "RebootPage does not close on screen change")
need("onCurrentHomeAdvancedPageIdxChanged" in page,
     "RebootPage does not close on page change")
need("onRequestHomeByPPChanged" in page,
     "RebootPage does not close on requestHomeByPP")
need("onNotifyHomeMainChanged: root.close()" in page,
     "RebootPage does not close on the C++ Home signal")
need("homeSeq" in page and "home_source" in page,
     "RebootPage does not consume the daemon home_seq")
need("preventStealing: true" in page, "RebootPage shield cannot prevent stealing")
need("acceptedButtons: Qt.AllButtons" in page, "shield does not accept all buttons")
need("onWheel: wheel.accepted = true" in page, "shield does not consume the wheel")
need("QPopupAnimationItem" in page, "RebootPage is not a stock popup")
need("QVerticalButton" in page, "RebootPage panel is not stock")
need("QHMI_ToggleButton" in page, "RebootPage buttons are not stock")
need("ConstantModel.APP_HOME_POPUP_WIDTH_MID" in page, "popup width token missing")
need("ConstantModel.APP_HOME_POPUP_1LINE_HEIGHT" in page, "popup height token missing")
need("ConstantModel.APP_HOME_POPUP_BUTTON_HEIGHT" in page, "popup button token missing")
need("GENERAL_OPACITY_PERCENT_70" in page, "popup dim missing")
need("X-Reboot-App" in page, "QML does not send the reboot header")
need("AppHomeModel.drivingRestriction" in page, "driving lockout missing")
need("update_in_progress" in page, "update guard message missing")
need("TranslationsModel.STR_HOME_YES" in page, "Yes label missing")
need("TranslationsModel.STR_HOME_NO" in page, "No label missing")

need("EVT_HOME_SOUND_BEEP_PLAY" in tile, "Reboot tile does not beep")
need("onPositionChanged" in tile, "Reboot tile press does not cancel on slide-off")
need("reboot_icon_n.png" in tile, "Reboot tile icon path missing")
need('text: "Reboot"' in tile, "Reboot tile label missing")
PY

python3 - "$ROOT/artifacts/assets/tweaks/reboot_icon_n.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGBA")
px = [p for p in im.getdata() if p[3] > 0]
assert px, "Reboot icon is empty"
avg = tuple(sum(p[i] for p in px) // len(px) for i in range(3))
assert avg[2] > avg[0] + 80 and avg[2] > avg[1] + 40, avg
PY

SB="$ROOT/artifacts/qml/AppStandbyClock"
for base in qml_eco3.0 new_qml qml_genesis; do
    grep -q 'TWEAKS_DISCLAIMER_260128' "$SB/$base/main.qml" || {
        echo "missing disclaimer marker in $base/main.qml" >&2; exit 1; }
    grep -q 'TWEAKS_DISCLAIMER_260128' "$SB/$base/Screen_AppStandbyClock_Confirm.qml" || {
        echo "missing disclaimer marker in $base/Confirm.qml" >&2; exit 1; }
    grep -q 'autoConfirmTimer' "$SB/$base/Screen_AppStandbyClock_Confirm_Profile.qml" || {
        echo "missing autoConfirmTimer in $base/Confirm_Profile.qml" >&2; exit 1; }
done

echo "PASS: generated QML structure"
