#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: NE_E.EUR.S5W_L.001.001.260128, stock vbtd-51
# Build a private, manual-install candidate. Never deploy or enable anything.
set -eu
umask 077
[ "$#" = 1 ] || { echo "Usage: sh $0 OWNER_ROOTFS" >&2; exit 2; }
ROOTFS=$(cd "$1" && pwd -P)
HERE=$(cd "$(dirname "$0")" && pwd -P)
WIFI1B=$(dirname "$HERE")
REPO=$(cd "$WIFI1B/../.." && pwd -P)
mkdir -p /tmp/opencode
OUT=$(mktemp -d /tmp/opencode/carplay-candidate.XXXXXXXX)
trap 'rc=$?; if [ "$rc" -ne 0 ]; then printf "Incomplete candidate retained at %s\n" "$OUT" >&2; fi' EXIT

python3 "$HERE/build_carplay_spp_patch.py" \
    --input "$ROOTFS/app/bin/vbtd-51" --target 51 --out "$OUT/vbtd"
python3 "$HERE/build_carplay_broker.py" --rootfs "$ROOTFS" --out "$OUT/broker"
python3 "$HERE/build_carplay_native.py" --rootfs "$ROOTFS" --out "$OUT/native"

# Ship only the selected outputs; objects and path-bearing build receipts
# remain in the private build directory, not in the transferable payload.
mkdir "$OUT/payload"
cp "$OUT/vbtd/vbtd-51.spp-rx.patched" "$OUT/broker/carplay-broker" \
    "$OUT/native/carplay-native-auth" "$OUT/payload/"
cp "$WIFI1B/artifacts/systemd/carplay-broker.service" \
    "$WIFI1B/artifacts/systemd/scourge-carplay-auth.service" "$OUT/payload/"
cp "$WIFI1B/artifacts/carplay/carplay-broker.conf.example" "$OUT/payload/"
cp "$REPO/docs/wireless-carplay/TEST-PLAN.md" \
    "$REPO/docs/wireless-carplay/VALIDATION.md" \
    "$REPO/docs/wireless-carplay/REPRODUCTION.md" \
    "$REPO/docs/wireless-carplay/VBT-SPP-HOOK-PORTING.md" \
    "$REPO/docs/wireless-carplay/DEFERRED-CAR-PROBES.md" "$OUT/payload/"
cp "$WIFI1B/artifacts/carplay/README.md" "$OUT/payload/INSTALL.md"
(
    cd "$OUT/payload"
    sha256sum vbtd-51.spp-rx.patched carplay-broker carplay-native-auth \
        carplay-broker.service scourge-carplay-auth.service \
        carplay-broker.conf.example INSTALL.md TEST-PLAN.md VALIDATION.md \
        REPRODUCTION.md VBT-SPP-HOOK-PORTING.md DEFERRED-CAR-PROBES.md > SHA256SUMS
    sha256sum -c SHA256SUMS
)
printf 'Private WIP candidate (not car-validated): %s/payload\n' "$OUT"
