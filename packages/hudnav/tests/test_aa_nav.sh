#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# aa_shim navigation-enable interposers against a fake head-unit library (host only, nothing touches /rw_data).
set -eu
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/hudnav-aanav.XXXXXX)
trap 'rm -rf "$T"' EXIT
export HUDNAV_AA_NAV_ENABLE_FILE="$T/enable" HUDNAV_AA_TRACE_FILE="$T/trace.on" HUDNAV_AA_TRACE_LOG="$T/trace.log"
: > "$T/trace.on"
# active (gate forced on, as in AppProjection)
LD_PRELOAD=./out/aa_shim.so LD_LIBRARY_PATH=./out HUDNAV_FORCE=1 ./out/aa_nav_probe
grep -q 'NAV setInstanceINavigationStatusServiceCB called' "$T/trace.log"
grep -q 'NAV NavigationStatusEndpoint::start() called' "$T/trace.log"
# inert: shim loaded but not in AppProjection (no force, exe is not AppProjection): nothing may be added
rm -f "$T/trace.log"
LD_PRELOAD=./out/aa_shim.so LD_LIBRARY_PATH=./out ./out/aa_nav_probe inert
[ ! -s "$T/trace.log" ] || { echo "inert run wrote trace lines"; exit 1; }
# without the shim at all the probe's expectations about added calls must NOT hold (sanity of the test itself)
! LD_LIBRARY_PATH=./out HUDNAV_FORCE=1 ./out/aa_nav_probe > /dev/null 2>&1
echo "aa nav tests passed"
