#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Gen5W_L EU 260128
# Build the Wifi1b USB payload tree.  Run from the wifi1b_app root.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
OUT=${1:-"$ROOT/out/usb"}
DROPBEAR="${SCOURGE_DROPBEAR:?set SCOURGE_DROPBEAR to the owner-supplied static dropbearmulti}"

cd "$ROOT"

echo "== icons =="
python3 scripts/make_icons1b.py

# First-party components only. Shared QML is additively patched after the
# canonical Battery/WiFi/Tweaks installers, never from divergent sibling trees.

echo "== daemon =="
# The embedded build identity hashes scripts/ and src/, so a script-only
# change must relink the daemon; an incremental make would keep the old ID.
make -C src clean
make -C src

echo "== assemble $OUT =="
rm -rf "$OUT"
mkdir -p "$OUT/payload/bin" "$OUT/payload/assets/wifi1b" "$OUT/payload/systemd" \
         "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen" \
         "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component" \
          "$OUT/backup"
mkdir -p "$OUT/payload/lib"
cp "$ROOT/../common/install_io.sh" "$OUT/payload/lib/"
cp "$ROOT/../common/runtime_cleanup.sh" "$OUT/payload/lib/"
python3 scripts/accepted_qml.py "$OUT/payload/accepted-qml.md5"

cp src/wifi1b_daemon "$OUT/payload/bin/wifi1b_daemon"
cp src/wifi1b_qmlpatch "$OUT/payload/bin/wifi1b_qmlpatch"
cp artifacts/assets/wifi1b/*.png "$OUT/payload/assets/wifi1b/"
cp artifacts/systemd/wifi1b-daemon.service "$OUT/payload/systemd/"
cp artifacts/systemd/60-wifi1b-foreground.conf "$OUT/payload/systemd/"
cp artifacts/systemd/60-connman-nodevice.conf "$OUT/payload/systemd/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/Wifi1bPage.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/component/Wifi1bTile.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/component/Wifi1bSwitch.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"
cp artifacts/qml/AppHomeScreen/qml_eco3.0/component/Wifi1bButton.qml "$OUT/payload/qml/AppHomeScreen/qml_eco3.0/component/"

cp "$DROPBEAR" "$OUT/payload/bin/dropbearmulti"
chmod 755 "$OUT/payload/bin/wifi1b_daemon" "$OUT/payload/bin/wifi1b_qmlpatch" "$OUT/payload/bin/dropbearmulti"

cp wifi1b.txt.sample "$OUT/payload/wifi1b.txt.sample"
cp wifi1b.txt.sample "$OUT/README-wifi1b.txt"
printf 'Wifi1b backups are written here by the installer.\n' > "$OUT/backup/README.txt"

cp scripts/run_stuff1b.sh "$OUT/run_stuff.sh"
cp scripts/run_stuff_restore1b.sh "$OUT/run_stuff_restore.sh"
chmod 755 "$OUT/run_stuff.sh" "$OUT/run_stuff_restore.sh"

cat > "$OUT/README.txt" <<'EOT'
Wifi1b V23 — static wpa_supplicant + own DHCP + SSH
===============================================

This is the standalone Wifi1b payload. It needs the canonical Battery/Tweaks
(or Battery/Tweaks/WiFi) shared home screen, but it does NOT install or
reinstall the WiFi app and has no migration stage. It works beside an
existing WiFi app and on the no-WiFi standalone layout.

WORK IN PROGRESS: offline-tested, not car-validated. Do not present it as a
verified fix and do not deploy it to a vehicle without explicit owner
authorization.

Install:
  1. Copy this USB tree to a FAT/exFAT stick.
  2. Optional: put iphone_wifi.txt (or wifi.txt) and authorized_keys at the root.
  3. Plug it in; check run_stuff_result.txt.
  4. Reboot if the result says REBOOT_REQUIRED.

The installer adds a "WiFi 1b" tile and a separate Wifi1b page. Wifi1b starts
idle: importing credentials does not enable WLAN, a mode, or SSH. Use the
Wifi1b page to start Client or CarPlay AP. The preflight stops only this
bundle's own radio daemons; no manual WiFi Off step is needed.

CarPlay AP defaults:
  SSID IONIQ5-CarPlay-B, 5 GHz channel 36, country GB
  static 10.42.0.1/24, own DHCP pool 10.42.0.10-200
  SSH user wifi1b, UID 0, key-only, configured port (default 2224), bind-all

V22+ BlueLink-safe client routing:
  While rmnet_usb0/rmnet_usb1 has an IPv4 address, client mode installs no
  WiFi default route. WiFi is SSH-only; the modem default and per-server host
  routes stay authoritative. Unverified legacy wlan0 defaults and ownership
  records are preserved; route changes fail closed. If no modem data IPv4 exists, a bounded
  high-metric proto-66 fallback is installed. A 30 s audit reconciles the two.
  Advanced: client_route_policy=fallback in /rw_data/wifi1b/config.conf.

V22+ Orange Pi USB-SSH:
  usb0 is configured as 10.55.0.2/24 with no default route. The generated
  runtime udev rule/helper uses the configured SSH port and remains available
  when WiFi is Off. ConnMan is kept off usb0. A missing Pi is a no-op.

The UID 0 wifi1b account is a deliberate product default, not a hidden
credential, and it is NOT car-validated. Owner keys in /rw_data/wifi1b are
preserved unless a replacement authorized_keys is supplied.

Restore:
  Copy run_stuff_restore.sh over run_stuff.sh and plug it in. This restores
  the shared QML, the service file and its enabled/active state from the
  original install transaction, and leaves the pre-existing WiFi app as it was.

Diagnostics:
  The Wifi1b page's Status/Logs tabs write wifi1b_logs_* bundles to the stick.
EOT

python3 scripts/package_release.py "$OUT" --metadata-only
# Deterministic content stamp instead of a timestamp/PID: the loader's md5
# gate re-runs the script when any installed payload byte changes, and
# identical inputs produce identical bytes.  Metadata files are excluded
# because package_release rewrites them after this digest is taken.
payload_digest=$(cd "$OUT" && find payload -type f \
    ! -name build.json ! -name build-id.txt -print0 | LC_ALL=C sort -z | \
    xargs -0 sha256sum | sha256sum | cut -d' ' -f1)
[ -n "$payload_digest" ] || { echo "cannot compute payload digest" >&2; exit 1; }
printf '\n# WIFI1B_PAYLOAD_DIGEST=%s\n' "$payload_digest" >> "$OUT/run_stuff.sh"
printf '\n# WIFI1B_PAYLOAD_DIGEST=%s\n' "$payload_digest" >> "$OUT/run_stuff_restore.sh"
# Record the final script bytes in metadata before hashing the manifest; the
# final package step then recomputes identical metadata.
python3 scripts/package_release.py "$OUT" --metadata-only
( cd "$OUT" && find payload -type f -print0 | LC_ALL=C sort -z | xargs -0 md5sum > manifest.md5 )
python3 scripts/package_release.py "$OUT"

echo "== done =="
echo "USB tree: $OUT"
echo "manifest entries: $(wc -l < "$OUT/manifest.md5")"
