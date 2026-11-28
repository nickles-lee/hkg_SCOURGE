#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# Host build (offline). Static-friendly C99, no external deps. NOT car-validated.
set -eu
cd "$(dirname "$0")/.."
mkdir -p out build
CC="${CC:-gcc}"
# All firmware-specific literals used by cp_rg.c and aa_shim.c come from this
# profile at build time. Override with: PROFILE=profiles/<other>.json sh scripts/build.sh
PROFILE="${PROFILE:-profiles/NE_E.AUS.S5W_L.001.001.260319.json}"
python3 -I tools/profile_to_header.py --profile "$PROFILE" --output build/fw_profile.h
CFLAGS="-std=c99 -O2 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -Ibuild"
$CC $CFLAGS -o out/hudnavd src/daemon.c src/convert.c src/motion.c src/output.c src/output_can.c src/settings.c
# Diagnostic daemon: log backend only, CAN TX is not compiled in (no output_can.c).
$CC $CFLAGS -DHUDNAV_DIAGNOSTIC -o out/hudnavd-diag src/daemon.c src/convert.c src/motion.c src/output.c src/settings.c
$CC $CFLAGS -o out/test_core tests/test_core.c src/convert.c src/motion.c src/settings.c
$CC $CFLAGS -o out/test_settings tests/test_settings.c src/settings.c
$CC $CFLAGS -o out/test_rgd tests/test_rgd.c src/rgd_tlv.c src/convert.c
$CC $CFLAGS -shared -fPIC -o out/aa_shim.so src/aa_shim.c src/convert.c
$CC $CFLAGS -o out/aa_gate_probe tests/aa_gate_probe.c -ldl
# AA navigation-enable interposers: fake head-unit library + probe (tests/test_aa_nav.sh)
$CC $CFLAGS -shared -fPIC -o out/libaafake.so tests/aa_nav_fake.c
$CC $CFLAGS -o out/aa_nav_probe tests/aa_nav_probe.c -Lout -laafake -Wl,-rpath,'$ORIGIN' 
# CarPlay iAP2 RouteGuidance interceptor model (host-only; no firmware/USB/network).
$CC $CFLAGS -o out/test_cp tests/test_cp.c src/cp_shim.c src/rgd_tlv.c src/convert.c src/motion.c
$CC $CFLAGS -o out/test_cp_rg_rx tests/test_cp_rg_rx.c src/cp_rg_rx.c src/rgd_tlv.c src/convert.c
$CC $CFLAGS -o out/test_cp_rg_tlv tests/test_cp_rg_tlv.c src/cp_rg_tlv.c
$CC $CFLAGS -o out/test_cp_rg_send tests/test_cp_rg_send.c src/cp_rg_send.c
$CC $CFLAGS -shared -fPIC -o out/cp_shim.so src/cp_shim.c src/rgd_tlv.c src/convert.c
# CarPlay production interposer (thin read/write/open hooks + the tested logic).
$CC $CFLAGS -shared -fPIC -o out/cp_hook.so src/cp_hook.c src/cp_shim.c src/rgd_tlv.c src/convert.c -ldl
$CC $CFLAGS -o out/cp_hook_probe tests/cp_hook_probe.c
# CP-A2: CarPlay Identification route-guidance wrapper + runtime patcher.
# Host test links the same wrapper with a fake real P (-DCP_RG_UNIT_TEST).
$CC $CFLAGS -DCP_RG_UNIT_TEST -o out/test_cp_rg_wrapper tests/test_cp_rg_wrapper.c src/cp_rg.c src/cp_rg_tlv.c
$CC $CFLAGS -shared -fPIC -o out/cp_rg.so src/cp_rg.c src/cp_rg_tlv.c src/cp_rg_send.c src/cp_rg_rx.c src/rgd_tlv.c src/convert.c
# iAP2Service-2.0 loads no libdl: dlsym / extra NEEDED entries would abort it.
if readelf --dyn-syms -W out/cp_rg.so | grep -q ' dlsym'; then
    echo "ERROR: cp_rg.so references dlsym (iAP2Service has no libdl)" >&2
    exit 1
fi
if readelf -d out/cp_rg.so | grep NEEDED | grep -vqE 'libc\.so\.6|ld-linux-x86-64\.so\.2'; then
    echo "ERROR: cp_rg.so has an unexpected NEEDED library" >&2
    readelf -d out/cp_rg.so | grep NEEDED >&2
    exit 1
fi
echo "build ok: out/hudnavd out/test_core out/test_rgd out/aa_shim.so out/test_cp out/cp_shim.so out/cp_hook.so"
