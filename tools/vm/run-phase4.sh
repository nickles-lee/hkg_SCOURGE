#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Direct AppDMClient -> AppUpgrade VM run. Base images are read-only
# (QEMU snapshot=on); all guest writes stay in this run's temporary overlay.
set -eu
cd "$(dirname "$0")"
TOOLS=${TOOLS_IMAGE:-tools.dev.img}
ALLOW=
if [ "${ALLOW_STALE_TOOLS:-0}" = 1 ]; then
    ALLOW=1
else
    if ! python3 -m harness.tools check --image "$TOOLS" --quiet; then
        echo "REFUSED: tools image '$TOOLS' is stale or unmanifested." >&2
        python3 -m harness.tools check --image "$TOOLS" >&2 || true
        echo "Rebuild with: sh build_tools.sh tools.$(date +%Y%m%d).img" >&2
        echo "Research override: ALLOW_STALE_TOOLS=1 sh run-phase4.sh" >&2
        exit 1
    fi
fi
GRAPHICS=patched-offscreen
[ "${USE_MESA:-0}" = 1 ] && GRAPHICS=mesa112
SCREENSHOT_ARG=--screenshot
if [ "${SCREENSHOT:-1}" = 0 ]; then SCREENSHOT_ARG=; fi
exec python3 launch.py run --profile research --scenario updater-launch \
    --usb "${UPDATE_IMAGE:-update-k.img}" --tools "$TOOLS" \
    --graphics "$GRAPHICS" --timeout "${TIMEOUT:-540}" \
    ${ALLOW:+--allow-stale-tools} ${SCREENSHOT_ARG} "$@"
