#!/bin/sh
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
set -eu
cd "$(dirname "$0")/.."
sh scripts/build.sh
./out/test_core
./out/test_rgd
./out/test_cp
./out/test_cp_rg_rx
./out/test_cp_rg_tlv
./out/test_cp_rg_send
./out/test_cp_rg_wrapper
./out/test_settings
python3 tests/test_kit.py
python3 tests/test_handoff.py
python3 tests/test_aa_gate.py
python3 tests/test_run_wrapper.py
python3 tests/test_profile_to_header.py
python3 tests/test_port_check.py
sh tests/test_replay.sh
sh tests/test_installer.sh
sh tests/test_persist.sh
sh tests/test_premount.sh
sh tests/test_ld_preload.sh
sh tests/test_aa_nav.sh
rm -f /tmp/hudnav-cp-hook-test.sock
LD_PRELOAD=./out/cp_hook.so ./out/cp_hook_probe /tmp/hudnav-cp-hook-test.sock
