#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Stage 3 (host-level): vtable-reloc interposition of aa_shim.so over a fake vendor lib.
set -eu
cd "$(dirname "$0")/.."
sh scripts/build.sh >/dev/null
W=/tmp/vmt3; mkdir -p $W
gcc -shared -fPIC -o $W/libfake_aa.so tests/vm/fake_aa_lib.c
gcc -o $W/driver tests/vm/aa_driver.c -L$W -lfake_aa -Wl,-rpath,$W
S=$W/h.sock
./out/hudnavd --socket $S 2>$W/d.log & D=$!; sleep 0.4
echo "--- without shim"; $W/driver
echo "--- with shim";    HUDNAV_SOCK=$S LD_PRELOAD=$PWD/out/aa_shim.so $W/driver
sleep 0.6; kill $D 2>/dev/null || true
echo "--- daemon saw:"; cat $W/d.log
echo "--- fail-open (no daemon):"; rm -f $S; HUDNAV_SOCK=$S LD_PRELOAD=$PWD/out/aa_shim.so $W/driver | grep -c ORIG | sed 's/^/orig calls: /'
