#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Build the complete USB payload tree for the Tweaks app.
# Usage: sh scripts/build_payload.sh [output_dir]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
OUT=${1:-"$ROOT/out/usb"}
ROOTFS="${SCOURGE_ROOTFS:-$(dirname "$0")/../../../work/rootfs}"
export IONIQ_ROOTFS="$ROOTFS"
QML_SRC="$ROOT/artifacts/qml/AppHomeScreen/qml_eco3.0"

cd "$ROOT"

echo "== build daemon / wrapper =="
make -C src

echo "== build persistence helper =="
if [ "${SKIP_PERSIST:-0}" = "1" ]; then
    echo "skipping tweaks_persist (SKIP_PERSIST=1)"
else
    sh scripts/build_target_helper.sh
fi

echo "== icons =="
python3 scripts/make_icons.py

echo "== QML artifacts =="
python3 scripts/build_artifacts.py

echo "== tzdata =="
python3 scripts/fetch_tzdata.py

echo "== assemble $OUT =="
rm -rf "$OUT"
mkdir -p "$OUT/payload/bin" "$OUT/payload/assets/tweaks" "$OUT/payload/systemd" \
         "$OUT/payload/ref" "$OUT/payload/tzdata" \
         "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen" \
         "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/common" \
         "$OUT/payload/qml/AppStandbyClock" \
         "$OUT/backup/AppHomeScreen/qml_eco3.0/HomeScreen" \
         "$OUT/backup/AppHomeScreen/qml_eco3.0/component/common"

cp src/tweaks_daemon "$OUT/payload/bin/tweaks_daemon"
cp src/wrandr_wrapper "$OUT/payload/bin/wrandr"
if [ -f src/tweaks_persist ]; then
    cp src/tweaks_persist "$OUT/payload/bin/tweaks_persist"
fi
cp artifacts/assets/tweaks/*.png "$OUT/payload/assets/tweaks/"
cp artifacts/systemd/tweaks-daemon.service "$OUT/payload/systemd/"
cp "$QML_SRC/HomeScreen/TweaksPage.qml" "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp "$QML_SRC/HomeScreen/RebootPage.qml" "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp "$QML_SRC/component/TweaksTile.qml" "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"
cp "$QML_SRC/component/RebootTile.qml" "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"
cp "$QML_SRC/component/TweaksSwitch.qml" "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"
cp "$QML_SRC/component/TweaksButton.qml" "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"
cp "$QML_SRC/component/TweaksZonePicker.qml" "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"
cp -a "$ROOT/artifacts/qml/AppStandbyClock/." "$OUT/payload/qml/AppStandbyClock/"
cp "$QML_SRC/component/common/Q_ThemeImage.qml" "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/common/"
cp "$QML_SRC/HomeScreen/QHMI_HomeScreenMain_Advanced."*.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp "$QML_SRC/HomeScreen/QHMI_HomeScreenMain_Wide."*.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp "$QML_SRC/HomeScreen/Screen_HomeScreen_AllMenus."*.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp "$QML_SRC/HomeScreen/Screen_HomeScreen_Submenu_Setup."*.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp "$QML_SRC/component/QCustomizedMenuListAllowEmpty."*.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"

# zic creates hardlinks for aliases (US/Eastern -> America/New_York). Keep
# them in out/usb on ext4, but when deploying to FAT/exFAT materialize them
# with cp -R/rsync -r; cp -a fails with "cannot create hard link".
cp -a "$ROOT/build/tzdata/zoneinfo" "$OUT/payload/tzdata/"
cp "$ROOT/build/tzdata/zones.txt" "$ROOT/build/tzdata/version.txt" "$OUT/payload/tzdata/"

chmod 755 "$OUT/payload/bin/tweaks_daemon" "$OUT/payload/bin/wrandr"
[ -f "$OUT/payload/bin/tweaks_persist" ] && chmod 755 "$OUT/payload/bin/tweaks_persist"

# Reference hashes used by the installer to recognise stock bases.
md5_of() { md5sum "$1" | cut -d' ' -f1; }
echo "$(md5_of base/stock/HomeScreen/QHMI_HomeScreenMain_Advanced.qml)" > "$OUT/payload/ref/stock_advanced.md5"
echo "$(md5_of base/hello/HomeScreen/QHMI_HomeScreenMain_Advanced.qml)" > "$OUT/payload/ref/hello_advanced.md5"
echo "$(md5_of base/stock/HomeScreen/QHMI_HomeScreenMain_Wide.qml)" > "$OUT/payload/ref/stock_wide.md5"
echo "$(md5_of base/hello/HomeScreen/QHMI_HomeScreenMain_Wide.qml)" > "$OUT/payload/ref/hello_wide.md5"
echo "$(md5_of base/stock/HomeScreen/Screen_HomeScreen_AllMenus.qml)" > "$OUT/payload/ref/stock_allmenus.md5"
echo "$(md5_of base/hello/HomeScreen/Screen_HomeScreen_AllMenus.qml)" > "$OUT/payload/ref/hello_allmenus.md5"
echo "$(md5_of base/stock/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml)" > "$OUT/payload/ref/stock_setup.md5"
echo "$(md5_of base/hello/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml)" > "$OUT/payload/ref/hello_setup.md5"
echo "$(md5_of base/stock/component/QCustomizedMenuListAllowEmpty.qml)" > "$OUT/payload/ref/stock_custom.md5"
echo "$(md5_of base/hello/component/QCustomizedMenuListAllowEmpty.qml)" > "$OUT/payload/ref/hello_custom.md5"
echo "$(md5_of "$ROOTFS/app/share/AppHomeScreen/qml_eco3.0/component/common/Q_ThemeImage.qml")" > "$OUT/payload/ref/stock_theme.md5"

# Reference originals for manual recovery/diffing.
cp base/stock/HomeScreen/QHMI_HomeScreenMain_Advanced.qml "$OUT/backup/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp base/stock/HomeScreen/QHMI_HomeScreenMain_Wide.qml "$OUT/backup/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp base/stock/HomeScreen/Screen_HomeScreen_AllMenus.qml "$OUT/backup/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp base/stock/HomeScreen/Screen_HomeScreen_Submenu_Setup.qml "$OUT/backup/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp base/stock/component/QCustomizedMenuListAllowEmpty.qml "$OUT/backup/AppHomeScreen/qml_eco3.0/component/"
cp "$ROOTFS/app/share/AppHomeScreen/qml_eco3.0/component/common/Q_ThemeImage.qml" "$OUT/backup/AppHomeScreen/qml_eco3.0/component/common/"

cp scripts/run_stuff.sh "$OUT/run_stuff.sh"
cp scripts/run_stuff_restore.sh "$OUT/run_stuff_restore.sh"
cp scripts/run_stuff_diag.sh "$OUT/run_stuff_diag.sh"
cp scripts/run_stuff.sh "$OUT/do_stuff.sh"
cp scripts/run_stuff_restore.sh "$OUT/do_stuff_restore.sh"
cp scripts/run_stuff_diag.sh "$OUT/do_stuff_diag.sh"
chmod 755 "$OUT"/run_stuff*.sh "$OUT"/do_stuff*.sh

cat > "$OUT/README.txt" <<'EOF'
Ioniq 5 Gen5W Tweaks app - USB payload

Install:
  1. Copy the whole contents of this USB tree to a FAT/exFAT stick.
     Use cp -R or rsync -r, not cp -a: the compiled tzdata tree contains
     hardlinks and exFAT cannot store them.
  2. Plug the stick into the head unit; the initial_root failsafe runs
     run_stuff.sh (or do_stuff.sh, if that is your loader name) as root
     within ~8 seconds.
  3. Check run_stuff_result.txt on the stick.

The package adds a Tweaks tile and a fourth Reboot tile to the home screen /
All Menus / Setup.

Tweaks page:
  - Instrument Cluster Night Mode Fix
  - Infotainment Night Mode Fix (CarPlay and Android Auto)
  - Safety disclaimer auto-accept (1-10s, cancels on touch)
  - Automatic DST using a selectable IANA timezone
  - Hyundai OTA block (local USB updates remain available)

Reboot app:
  - Yes/No confirmation modal; No/outside/Home closes it
  - Yes asks the daemon for a clean systemctl reboot
  - if the clean reboot has not taken effect after 20 s, the daemon
    syncs and sends the vendor micom watchdog reset as a hard fallback
  - blocked while driving or during an update

The night-mode fixes follow the vehicle's existing Setup > Display >
Blue Light Filter warmth setting.  The disclaimer and DST settings are
stored in /rw_data/tweaks/ and survive reboots.  The AppStandbyClock
disclaimer patch takes effect on the next boot.

Restore:
  Copy run_stuff_restore.sh (or do_stuff_restore.sh) over the loader filename
  on the stick and plug it in.

Diagnostics:
  Copy run_stuff_diag.sh (or do_stuff_diag.sh) over the loader filename and
  plug it in; it writes tweaks_diag.txt to the stick root (read-only).

This does not flash firmware and does not touch the updater, bootloader or
partitions.
EOF

# Manifest covers payload/ only; the loader md5-gates run_stuff.sh itself.
( cd "$OUT" && find payload -type f -print0 | sort -z | xargs -0 md5sum > manifest.md5 )

# Bake a payload digest into the loader scripts so a same-selection rebuild
# with different package bytes changes the loader's md5 gate and re-runs.
PAYLOAD_ID=$(md5sum "$OUT/manifest.md5" | cut -d' ' -f1 | cut -c1-16)
for f in "$OUT/run_stuff.sh" "$OUT/do_stuff.sh" \
         "$OUT/run_stuff_restore.sh" "$OUT/do_stuff_restore.sh"; do
    [ -f "$f" ] || continue
    sed -i "s/^# TWEAKS_PAYLOAD_ID=.*/# TWEAKS_PAYLOAD_ID=$PAYLOAD_ID/" "$f"
done
echo "payload id: $PAYLOAD_ID"

if command -v zip >/dev/null 2>&1; then
    rm -f "$ROOT/out/tweaks_app_usb.zip"
    ( cd "$OUT" && zip -qr "$ROOT/out/tweaks_app_usb.zip" . )
fi

echo "== done =="
echo "USB tree: $OUT"
echo "manifest entries: $(wc -l < "$OUT/manifest.md5")"
