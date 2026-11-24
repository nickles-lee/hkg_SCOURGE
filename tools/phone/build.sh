#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Build the static arm64 on-device rig binary. No toolchain is committed: use an
# installed Go toolchain (>= 1.21) and install one locally if absent.
set -eu
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
cd "$HERE"

if ! command -v go >/dev/null 2>&1; then
    echo "Go >= 1.21 is required. Install a toolchain locally (do not commit it)" >&2
    echo "and re-run. See docs/21 for the build-host prerequisites." >&2
    exit 1
fi

GOOS=linux GOARCH=arm64 CGO_ENABLED=0 \
    go build -trimpath -o phone-rig-arm64 .

sha256sum phone-rig-arm64
echo "wrote $HERE/phone-rig-arm64"
