#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# TARGET: AU Linux Gen5W 260319, x86-64, glibc 2.25
set -eu
cd "$(dirname "$0")/.."
name=aa-diagnostic-260319
kit=out/$name
mkdir -p "$kit" build
CC=${CC:-gcc}
# aa_shim.c reads its mangled symbol names from the generated profile header.
PROFILE="${PROFILE:-profiles/NE_E.AUS.S5W_L.001.001.260319.json}"
python3 -I tools/profile_to_header.py --profile "$PROFILE" --output build/fw_profile.h
flags="-std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -Ibuild"

# The diagnostic daemon is static and links only the log backend.
$CC $flags -static -DHUDNAV_DIAGNOSTIC -o "$kit/hudnavd" \
    src/daemon.c src/convert.c src/motion.c src/output.c src/settings.c
$CC $flags -shared -fPIC -o "$kit/aa_shim.so" src/aa_shim.c src/convert.c -ldl
cp scripts/run-aa-diagnostic.sh "$kit/"
cp scripts/aa-diagnostic.README.md "$kit/README.md"
cat > "$kit/firmware.sha256" <<'PINS'
726c69ce627e9183771d11bdfac9acf9d92286a9b6c1a0d88bfbd6b5ac5a7d50  app/bin/AppProjection
59c31ae0359a8720e75afc3ee3037c3c18f15ffba71347fb46dd5d9691726865  app/lib/liblinux-head-unit.so
PINS
(
    cd "$kit"
    sha256sum hudnavd aa_shim.so run-aa-diagnostic.sh firmware.sha256 README.md > package.sha256
)
tar -czf "out/$name.tar.gz" -C out "$name"
printf '%s\n' "Built $kit and out/$name.tar.gz"
