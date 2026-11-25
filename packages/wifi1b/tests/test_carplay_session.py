#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Build and run cp_session integration tests, plus source-policy checks.

Artifacts are created under /tmp/opencode only. The normal build treats
warnings as errors; an ASan/UBSan build re-runs the same two-engine tests.
"""
from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "carplay" / "cp_session.c"
HDR = ROOT / "src" / "carplay" / "cp_session.h"
TEST = ROOT / "tests" / "test_carplay_session.c"
LINK = ROOT / "src" / "carplay" / "cp_link.c"
BOOT = ROOT / "src" / "carplay" / "cp_bootstrap.c"
WIRE = ROOT / "src" / "carplay" / "cp_wire.c"
BUILD_ROOT = Path("/tmp/opencode")

COMMON = [
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
    "-Wshadow",
    "-Wstrict-prototypes",
    "-Wmissing-prototypes",
]
INPUTS = [str(SRC), str(LINK), str(BOOT), str(WIRE), str(TEST)]


def compile_and_run(tmp: Path, name: str, extra: list[str],
                    env_extra: dict[str, str]) -> None:
    binary = tmp / name
    cmd = [os.environ.get("CC", "cc"), *COMMON, *extra, "-o", str(binary),
           *INPUTS]
    subprocess.run(cmd, check=True, timeout=120)
    env = dict(os.environ)
    env.update(env_extra)
    subprocess.run([str(binary)], check=True, timeout=120, env=env)


def assert_source_policy() -> None:
    source = SRC.read_text()
    header = HDR.read_text()
    test = TEST.read_text()

    for path, text in ((SRC, source), (HDR, header), (TEST, test),
                       (Path(__file__), Path(__file__).read_text())):
        assert "SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors" in text, path
        assert "SPDX-License-Identifier: AGPL-3.0-only" in text, path

    # Fixed storage, no allocation, no logging, no transport/hardware hooks.
    for forbidden in (
        r"\bmalloc\s*\(",
        r"\bcalloc\s*\(",
        r"\brealloc\s*\(",
        r"\bfree\s*\(",
        r"\bprintf\s*\(",
        r"\bfprintf\s*\(",
        r"\bsyslog\s*\(",
        r"\bsocket\s*\(",
        r"\bconnect\s*\(",
        r"\bpthread_",
        r"cp_spp_",
        r"cp_oem_",
        r"cp_native_",
    ):
        assert not re.search(forbidden, source), forbidden
        assert not re.search(forbidden, header), forbidden

    assert "#include <stdio.h>" not in source
    assert "#include <stdio.h>" not in header
    assert "cp_link_config_live_bluetooth(&config->link)" in source
    assert "CP_SESSION_RX_BUFFER" in header
    assert "(CP_LINK_MAX_REORDER + 1u) * CP_LINK_MAX_PAYLOAD" in header
    assert "CP_SESSION_POLICY_RECHECK_MS" in header
    assert "receiver_policy_deadline_ms" in header
    assert "if (tick_bootstrap && !session->receiver_active)" in source
    assert "receiver_policy_check" in source
    assert "cp_bootstrap_cancel" in source
    assert "session_link_config_acceptable" in source
    assert "receiver_policy_armed" in header
    assert "receiver_policy_armed" in source
    assert source.count("csm_rx_overflows++") == 1
    assert "All cp_boot_config fields, including the AirPlay port" in header
    assert "caller attestation" in header
    assert "This module never infers receiver activity internally" in header
    assert "about 486 KiB" in header
    assert "service_after_link(session, now_ms, false)" in source
    assert "cp_link_feed(&session->link, bytes + i, 1u, now_ms)" in source
    assert "Publishing AP readback is not a transport transition" in source

    # The active observation is a generation-bound bootstrap hold, not ad-hoc
    # wire gating: availability always reaches bootstrap and policy is checked
    # before a duplicate is returned.
    assert "active_availability_gated" not in source
    assert "csm_gated_availability" not in source
    assert "csm_gated_availability" not in header
    assert "cp_bootstrap_hold_offer" in source
    assert "cp_bootstrap_release_offer_hold" in source
    assert "cp_bootstrap_offer_held(&session->boot)" in source
    assert "cp_bootstrap_hold_offer" in header
    assert "cp_bootstrap_offer_held(&session->boot)" in source

    # The event record must stay scalar/sanitised: no payload pointer, SSID,
    # passphrase, key material or transport identifiers.
    event_block = header.split("typedef struct cp_session_event {", 1)[1]
    event_block = event_block.split("} cp_session_event_t;", 1)[0].lower()
    for forbidden in ("ssid", "passphrase", "public_key", "uint8_t *",
                      "const uint8_t", "peer_id", "data_len"):
        assert forbidden not in event_block, forbidden

    for needle in (
        "cp_session_feed",
        "cp_session_tick",
        "cp_session_set_receiver_active",
        "cp_session_pending_replies",
        "cp_session_set_ap_readback",
        "cp_session_cancel",
        "cp_session_transport_reset",
    ):
        assert needle in header, needle

    # Fixed two-engine facts exercised by the C test.
    for vector in ("404000061d00", "40400006aa00", "40400006aa05",
                   "404000065702"):
        assert vector in test, vector


def sanitizer_supported(compiler: str, tmp: Path) -> bool:
    probe = tmp / "sanprobe.c"
    probe.write_text("int main(void) { return 0; }\n")
    result = subprocess.run(
        [compiler, "-std=c11", "-fsanitize=address,undefined", "-o",
         str(tmp / "sanprobe"), str(probe)],
        capture_output=True,
        text=True,
        timeout=60,
    )
    return result.returncode == 0


def main() -> int:
    BUILD_ROOT.mkdir(parents=True, exist_ok=True)
    assert_source_policy()
    with tempfile.TemporaryDirectory(prefix="cp-session-", dir=str(BUILD_ROOT)) as d:
        tmp = Path(d)
        compile_and_run(tmp, "test_carplay_session", ["-O2"], {})
        print("PASS: cp_session normal strict build and two-engine tests")

        compiler = os.environ.get("CC", "cc")
        if sanitizer_supported(compiler, tmp):
            compile_and_run(
                tmp,
                "test_carplay_session_san",
                ["-O1", "-g", "-fno-omit-frame-pointer",
                 "-fsanitize=address,undefined"],
                {
                    "ASAN_OPTIONS": "detect_leaks=1:abort_on_error=1:strict_string_checks=1",
                    "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
                },
            )
            print("PASS: cp_session ASan/UBSan build and tests")
        else:
            print("SKIP: compiler sanitizer runtime unavailable")
    print("PASS: cp_session source policy and sanitised API checks")
    return 0


if __name__ == "__main__":
    sys.exit(main())
