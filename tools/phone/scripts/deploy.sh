#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host-side: push the built binary, config, scripts and a device-side env file.
# Fill in tools/phone/phone.env first (see phone.env.example). Run from here.
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
[ -f "$HERE/phone.env" ] || { echo "phone.env missing - copy phone.env.example" >&2; exit 1; }
# shellcheck disable=SC1091
. "$HERE/phone.env"
S=${PHONE_SERIAL:-CHANGEME}
[ "$S" != CHANGEME ] || { echo "set PHONE_SERIAL in phone.env" >&2; exit 1; }
R=${RIG_DIR:-/data/local/tmp/scourge}

[ -f "$HERE/phone-rig-arm64" ] || { echo "run 'sh build.sh' first" >&2; exit 1; }
[ -f "$HERE/config.json" ] || { echo "config.json missing - copy config.json.example" >&2; exit 1; }

# Create the directory as shell: ADB push runs as shell and cannot write into a
# root-owned directory. Root is only needed to execute the binary.
adb -s "$S" shell "mkdir -p $R"

TMP=$(mktemp)
# Generate the device-side env from host values (strip CR), keeping the host
# ADB serial out of the file.
{
    echo "RIG_DIR=$R"
    echo "GEN_IMAGE=${GEN_IMAGE:-$R/usb.img}"
    echo "EVIL_IMAGE=${EVIL_IMAGE:-$R/evil.img}"
    echo "GADGET_DIR=${GADGET_DIR:-/config/usb_gadget/g1}"
    echo "UDC=${UDC:-CHANGEME}"
    echo "MASS_STORAGE_FN=${MASS_STORAGE_FN:-mass_storage.0}"
    echo "CONFIG_NAME=${CONFIG_NAME:-c.1}"
    echo "LUN_NAME=${LUN_NAME:-lun.0}"
    echo "DM_NAME=${DM_NAME:-phone-rig}"
} | tr -d '\r' > "$TMP"
adb -s "$S" push "$TMP" "$R/rig.env"

adb -s "$S" push "$HERE/phone-rig-arm64" "$R/phone-rig"
adb -s "$S" push "$HERE/config.json" "$R/config.json"

# Strip CR defensively: Android's sh rejects CRLF scripts after a checkout on
# any platform, even though .gitattributes forces LF in the repository.
for f in "$HERE"/scripts/*.sh "$HERE"/tests/*.sh; do
    tr -d '\r' < "$f" > "$TMP"
    adb -s "$S" push "$TMP" "$R/$(basename "$f")"
done
rm -f "$TMP"

adb -s "$S" shell "su -c 'chmod 755 $R/phone-rig $R/*.sh; sha256sum $R/phone-rig'"
echo "deployed to $S:$R - next: 'su -c \"sh $R/provision.sh\"' on the phone"
