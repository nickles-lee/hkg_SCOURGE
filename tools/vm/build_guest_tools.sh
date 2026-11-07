#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

# Build the static guest helpers used by build_rootfs.sh/phase3.sh.
# One shared builder compiles every helper from guest/*.c; this wrapper keeps
# the historical entry point without a second, divergent compile recipe.
set -eu
cd "$(dirname "$0")"
sh build_research_tools.sh
file guest/micom-dbus-sim guest/usb-mount-signal
