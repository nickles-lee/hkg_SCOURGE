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
