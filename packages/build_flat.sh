#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: Ioniq 5 Gen5W_L EU 260128
# Build locally only; never format a drive or touch a car.
# Usage: sh packages/build_flat.sh [output directory]
# Owner input: SCOURGE_WIFI_TXT=/private/path (never a repository default).
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
OUT=${1:-"$HERE/../out/appcombo-flat"}
mkdir -p "$OUT"
OUT=$(CDPATH= cd -- "$OUT" && pwd)
STAGE=$(mktemp -d "${TMPDIR:-/tmp}/scourge-flat.XXXXXX")
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/tree"
sh "$HERE/compose.sh" "$STAGE/tree" battery,wifi,tweaks
if [ -n "${SCOURGE_WIFI_TXT:-}" ]; then
    cp -f "$SCOURGE_WIFI_TXT" "$STAGE/tree/wifi.txt"
    chmod 600 "$STAGE/tree/wifi.txt"
fi
if [ -n "${SCOURGE_AUTHORIZED_KEYS:-}" ]; then
    cp -f "$SCOURGE_AUTHORIZED_KEYS" "$STAGE/tree/authorized_keys"
fi
# Check the exact bytes that will be installed LAST, not an upstream base.
python3 "$HERE/common/check_layout.py" \
    "$STAGE/tree/combo/tweaks/payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile_wifi.qml"
(cd "$STAGE/tree" && tar -czf "$STAGE/appcombo.tar.gz" .)
digest=$(sha256sum "$STAGE/appcombo.tar.gz" | cut -d' ' -f1)
sed "s/@TARBALL_SHA256@/$digest/g" "$HERE/flat_wrapper.sh" > "$STAGE/run_stuff.sh"
sh -n "$STAGE/run_stuff.sh"
cp -f "$STAGE/appcombo.tar.gz" "$STAGE/run_stuff.sh" "$OUT/"
cat > "$OUT/README.txt" <<'EOF'
SCOURGE flat combo — Battery, WiFi, Tweaks and Reboot
OFFLINE-TESTED; on-car validation pending. No firmware/update stick present.
Copy BOTH run_stuff.sh and appcombo.tar.gz to the stick root; don't extract.
The wrapper verifies the archive SHA256 before running any stage.
SENSITIVE: when built with SCOURGE_WIFI_TXT/SCOURGE_AUTHORIZED_KEYS the
appcombo.tar.gz itself contains those credentials — store and dispose of the
pair accordingly, and keep it out of any shared location.
Icons are best-effort; critical files and readback checks still fail closed.
Beeps: 1 start, 2 per failed stage, 5 success, 3 finished with failures.
Read wrapper_result.txt, run_stuff_result.txt and *_result.txt.
combo.log and *_install.log contain the current run's detailed errors.
Create MODE containing battery / wifi / tweaks / all / restore to select.
Changing only MODE does NOT change the failsafe's run_stuff.sh MD5 gate:
also change a comment in run_stuff.sh when rerunning the same pair.
Keep owner credentials private. Optional wifi.txt/authorized_keys at the
stick root override packed inputs. SSH activation remains manual.
The experimental MDNSD_ROOT_ARM marker is NOT included; leave it disarmed.
Restore uses saved app backups; do not erase backups after a failed install.
EOF
echo "Built $OUT (tarball SHA256=$digest); car validation pending"
