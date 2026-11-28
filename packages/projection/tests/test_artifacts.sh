#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Projection+ home-screen patch: idempotency, markers, tile insertions,
# manifest, and loud refusal on a missing anchor.  Needs the git-ignored
# car base (packages/projection/base/car_2026-11-27).  Run: sh tests/test_artifacts.sh
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
PY=${PYTHON:-python3}
B="$HERE/build"
Q="$B/qml_eco3.0"
fails=0
ok()   { echo "ok   $1"; }
bad()  { echo "FAIL $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
count() { grep -c -- "$2" "$1" 2>/dev/null || true; }

snap() { (cd "$B" && find . -type f | sort | xargs sha256sum); }

$PY "$HERE/scripts/build_artifacts.py" --self-check >/dev/null || { bad "first build"; exit 1; }
ok "first build (+ patch(patch(x)) == patch(x) self-check)"
s1=$(snap)
$PY "$HERE/scripts/build_artifacts.py" >/dev/null || bad "second build"
s2=$(snap)
check "second build is byte-identical" '[ "$s1" = "$s2" ]'

ADV="$Q/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
ALL="$Q/HomeScreen/Screen_HomeScreen_AllMenus.qml"
SET="$Q/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml"
CUS="$Q/component/QCustomizedMenuListAllowEmpty.qml"
for f in "$ADV" "$ALL" "$SET" "$CUS"; do
  n=$(basename "$f")
  check "$n: one marker" '[ "$(count "$f" "PROJECTION_PAYLOAD_260319")" = 1 ]'
  check "$n: older markers kept" '[ "$(count "$f" "TWEAKS_PAYLOAD_260128")" = 1 ]'
done
check "Advanced: 3 ProjectionTile" '[ "$(count "$ADV" "^                ProjectionTile {")" = 3 ]'
for p in 1 2 3; do
  check "Advanced: projectionTile$p uses slot 3" \
    'grep -A2 "id: projectionTile$p\$" "$ADV" | grep -q "root.firstEmptySlot(3)"'
done
check "Advanced: tiles open the page" '[ "$(count "$ADV" "onClicked: projectionPage.open()")" = 3 ]'
check "Advanced: existing tiles still 0/1/2" \
  '[ "$(count "$ADV" "firstEmptySlot(0)")" = 3 ] && [ "$(count "$ADV" "firstEmptySlot(1)")" = 3 ] && [ "$(count "$ADV" "firstEmptySlot(2)")" = 3 ]'
for f in "$ADV" "$ALL" "$SET"; do
  check "$(basename "$f"): one ProjectionPage" '[ "$(count "$f" "id: projectionPage")" = 1 ]'
done
for f in "$ALL" "$SET"; do
  check "$(basename "$f"): signal handler" '[ "$(count "$f" "onProjectionClicked: projectionPage.open()")" = 1 ]'
done
check "custom: signal" '[ "$(count "$CUS" "signal projectionClicked()")" = 1 ]'
check "custom: tile skip 3" '[ "$(count "$CUS" "firstEmptyIndex(menuPage.pageModel, 3)")" = 1 ]'
check "custom: tile emits signal" '[ "$(count "$CUS" "onClicked: root.projectionClicked()")" = 1 ]'
check "Wide not patched" '[ ! -e "$Q/HomeScreen/QHMI_HomeScreenMain_Wide.qml" ]'

# Purely additive: every base line survives in order (diff shows only '+' lines).
BASE="$HERE/base/car_2026-11-27/qml_eco3.0"
for f in "$ADV" "$ALL" "$SET" "$CUS"; do
  rel=${f#$Q/}
  check "$rel: additive only" '! diff "$BASE/$rel" "$f" | grep -q "^<"'
done

M="$B/manifest.txt"
check "manifest: 4 patch rows" '[ "$(grep -c "^patch	" "$M")" = 4 ]'
check "manifest: base hashes match car_sha256" \
  'grep "^patch	" "$M" | while IFS="	" read k d s h b; do r=${d#/app/share/AppHomeScreen/qml_eco3.0/}; grep -q "^$b  ./$r\$" "$HERE/base/car_2026-11-27/car_sha256.txt" || exit 1; done'
check "manifest: page, tile, 2 icons" \
  'grep -q "HomeScreen/ProjectionPage.qml" "$M" && grep -q "component/ProjectionTile.qml" "$M" && [ "$(grep -c "projection_icon_.\.png" "$M")" = 2 ]'

# Negative: a base whose anchor is missing must be refused (exit 2), not guessed.
T=$(mktemp -d)
mkdir -p "$T/qml_eco3.0"
cp -r "$BASE/." "$T/qml_eco3.0/"
sed -i 's/id: rebootTile2$/id: rebootTileX/' "$T/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
(cd "$T/qml_eco3.0" && find . -type f | sort | xargs sha256sum) > "$T/car_sha256.txt"
out=$(PROJECTION_BASE="$T" $PY "$HERE/scripts/build_artifacts.py" 2>&1); rc=$?
check "missing anchor refused (rc=2)" '[ $rc = 2 ] && echo "$out" | grep -q "REFUSED: .*rebootTile2"'
# Negative: base differing from car_sha256.txt must be refused.
cp "$BASE/HomeScreen/QHMI_HomeScreenMain_Advanced.qml" "$T/qml_eco3.0/HomeScreen/"
(cd "$T/qml_eco3.0" && find . -type f | sort | xargs sha256sum) > "$T/car_sha256.txt"
echo "// drift" >> "$T/qml_eco3.0/HomeScreen/Screen_HomeScreen_AllMenus.qml"
out=$(PROJECTION_BASE="$T" $PY "$HERE/scripts/build_artifacts.py" 2>&1); rc=$?
check "hash drift refused (rc=2)" '[ $rc = 2 ] && echo "$out" | grep -q "sha256"'
rm -rf "$T"
# The refused runs may have written partial output; rebuild the real one.
$PY "$HERE/scripts/build_artifacts.py" >/dev/null
check "rebuild matches first build" '[ "$(snap)" = "$s1" ]'

echo "failures: $fails"
[ $fails = 0 ]
