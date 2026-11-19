#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Independent oracle and build/runner for the C1 CarPlay bootstrap tests.

The wire frames below are built and parsed by this file, independently of the
C encoder.  The C test binary is compiled into /tmp/opencode only and must
agree with these fixtures.  Nothing here touches a vehicle, phone, radio,
socket or OEM artifact.
"""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src" / "carplay"
TEST_C = ROOT / "tests" / "test_carplay_bootstrap.c"
PY_TEST = Path(__file__).resolve()

CSM = 0x4040
SENT = (0x1D01, 0xAA01, 0xAA03, 0x5703, 0x4301)
RECEIVED = (0x1D00, 0x1D02, 0x1D03, 0xAA00, 0xAA02, 0xAA04, 0xAA05,
            0x5702, 0x4300, 0x4E0D, 0x4E0E)
LANGUAGES = ("en", "de")


def tlv(param_id: int, payload: bytes) -> bytes:
    total = len(payload) + 4
    assert 4 <= total <= 0xFFFF
    return total.to_bytes(2, "big") + param_id.to_bytes(2, "big") + payload


def frame(msg_id: int, params: bytes) -> bytes:
    total = len(params) + 6
    assert 6 <= total <= 4096
    return (CSM.to_bytes(2, "big") + total.to_bytes(2, "big") +
            msg_id.to_bytes(2, "big") + params)


def text(value: str) -> bytes:
    return value.encode("utf-8") + b"\0"


def str_param(param_id: int, value: str) -> bytes:
    return tlv(param_id, text(value))


def u8_param(param_id: int, value: int) -> bytes:
    return tlv(param_id, bytes([value]))


def u16_param(param_id: int, value: int) -> bytes:
    return tlv(param_id, value.to_bytes(2, "big"))


def u32_param(param_id: int, value: int) -> bytes:
    return tlv(param_id, value.to_bytes(4, "big"))


def flag(param_id: int) -> bytes:
    return tlv(param_id, b"")


def u16_list(param_id: int, values: tuple[int, ...]) -> bytes:
    return tlv(param_id,
               b"".join(value.to_bytes(2, "big") for value in values))


def ident_frame(team: bool = True) -> bytes:
    eap = b"".join((
        u8_param(0, 1),
        str_param(1, "com.scourge.carplay"),
        u8_param(2, 4),
    ))
    bluetooth = b"".join((
        u16_param(0, 0),
        str_param(1, "bt"),
        flag(2),
        tlv(3, bytes.fromhex("aabbccddeeff")),
    ))
    wireless = b"".join((
        u16_param(0, 1),
        str_param(1, "SCOURGE-AP"),
        flag(2),
        flag(4),
    ))
    params = b"".join((
        str_param(0, "SCOURGE"),
        str_param(1, "GEN5W"),
        str_param(2, "SCOURGE"),
        str_param(3, "SN-0001"),
        str_param(4, "1.0.0"),
        str_param(5, "1.0"),
        u16_list(6, SENT),
        u16_list(7, RECEIVED),
        u8_param(8, 0),
        u16_param(9, 0),
        tlv(10, eap),
        str_param(11, "SCOURGETEAM") if team else b"",
        str_param(12, "en"),
        tlv(13, b"".join(text(language) for language in LANGUAGES)),
        tlv(17, bluetooth),
        tlv(24, wireless),
    ))
    return frame(0x1D01, params)


def wifi_frame(ssid: str = "SCOURGE-AP", channel: int = 36) -> bytes:
    params = b"".join((
        str_param(1, ssid),
        str_param(2, "Wireless-Test-1"),
        u8_param(3, 2),
        u8_param(4, channel),
    ))
    return frame(0x5703, params)


def start_frame(security: int, ssid: str = "SCOURGE-AP",
                channel: int = 36, link_local: str = "fe80::1") -> bytes:
    security_wire = {0: None, 1: 3, 2: 4}[security]
    wireless = [
        str_param(0, ssid),
        str_param(1, "Wireless-Test-1"),
        u8_param(2, channel),
        tlv(3, text(link_local)),
    ]
    if security_wire is not None:
        wireless.append(u8_param(4, security_wire))
    params = b"".join((
        tlv(1, b"".join(wireless)),
        u32_param(2, 7000),
        str_param(3, "SCOURGE-DEV"),
        str_param(4, "test-public-key"),
        str_param(5, "1.0.0"),
    ))
    return frame(0x4301, params)


EXPECTED = {
    "ident": ident_frame(True),
    "ident_no_team": ident_frame(False),
    "wifi": wifi_frame(),
    "start_wpa2": start_frame(0),
    "start_transition": start_frame(1),
    "start_wpa3": start_frame(2),
    "start_readback": start_frame(0, "CarPlay-AP", 149),
    "aa01": frame(0xAA01, tlv(0, bytes([1, 2, 255]))),
    "aa03": frame(0xAA03, tlv(0, bytes.fromhex("deadbeef"))),
}

# The same frames, written from the protocol layout and pinned as hex so a
# change in the Python builder cannot silently move the oracle.
PINNED_HEX = {
    "ident": (
        "404001001d01000c000053434f5552474500000a000147454e355700000c0002"
        "53434f5552474500000c0003534e2d3030303100000a0004312e302e30000008"
        "0005312e3000000e00061d01aa01aa0357034301001a00071d001d021d03aa00"
        "aa02aa04aa05570243004e0d4e0e00050008000006000900000026000a000500"
        "000100180001636f6d2e73636f757267652e636172706c617900000500020400"
        "10000b53434f555247455445414d000007000c656e00000a000d656e00646500"
        "001f00110006000000000007000162740000040002000a0003aabbccddeeff00"
        "210018000600000001000f000153434f555247452d4150000004000200040004"
    ),
    "ident_no_team": (
        "404000f01d01000c000053434f5552474500000a000147454e355700000c0002"
        "53434f5552474500000c0003534e2d3030303100000a0004312e302e30000008"
        "0005312e3000000e00061d01aa01aa0357034301001a00071d001d021d03aa00"
        "aa02aa04aa05570243004e0d4e0e00050008000006000900000026000a000500"
        "000100180001636f6d2e73636f757267652e636172706c617900000500020400"
        "07000c656e00000a000d656e00646500001f0011000600000000000700016274"
        "0000040002000a0003aabbccddeeff00210018000600000001000f000153434f"
        "555247452d4150000004000200040004"
    ),
    "wifi": (
        "404000335703000f000153434f555247452d41500000140002576972656c6573"
        "732d546573742d310000050003020005000424"
    ),
    "start_wpa2": (
        "40400074430100380001000f000053434f555247452d41500000140001576972"
        "656c6573732d546573742d31000005000224000c0003666538303a3a31000008"
        "000200001b580010000353434f555247452d4445560000140004746573742d70"
        "75626c69632d6b657900000a0005312e302e3000"
    ),
    "start_transition": (
        "404000794301003d0001000f000053434f555247452d41500000140001576972"
        "656c6573732d546573742d31000005000224000c0003666538303a3a31000005"
        "0004030008000200001b580010000353434f555247452d444556000014000474"
        "6573742d7075626c69632d6b657900000a0005312e302e3000"
    ),
    "start_wpa3": (
        "404000794301003d0001000f000053434f555247452d41500000140001576972"
        "656c6573732d546573742d31000005000224000c0003666538303a3a31000005"
        "0004040008000200001b580010000353434f555247452d444556000014000474"
        "6573742d7075626c69632d6b657900000a0005312e302e3000"
    ),
    "start_readback": (
        "40400074430100380001000f0000436172506c61792d41500000140001576972"
        "656c6573732d546573742d31000005000295000c0003666538303a3a31000008"
        "000200001b580010000353434f555247452d4445560000140004746573742d70"
        "75626c69632d6b657900000a0005312e302e3000"
    ),
    "aa01": "4040000daa01000700000102ff",
    "aa03": "4040000eaa0300080000deadbeef",
}


def parse_frame(data: bytes) -> tuple[int, bytes]:
    assert len(data) >= 6
    assert int.from_bytes(data[0:2], "big") == CSM
    total = int.from_bytes(data[2:4], "big")
    assert total == len(data), (total, len(data))
    msg_id = int.from_bytes(data[4:6], "big")
    return msg_id, data[6:]


def parse_tlvs(params: bytes) -> list[tuple[int, bytes]]:
    result = []
    offset = 0
    while offset < len(params):
        assert len(params) - offset >= 4
        total = int.from_bytes(params[offset:offset + 2], "big")
        assert 4 <= total <= len(params) - offset
        param_id = int.from_bytes(params[offset + 2:offset + 4], "big")
        result.append((param_id,
                       params[offset + 4:offset + total]))
        offset += total
    return result


def unique(params: list[tuple[int, bytes]], param_id: int) -> bytes:
    matches = [payload for current, payload in params if current == param_id]
    assert len(matches) == 1, (param_id, matches)
    return matches[0]


def repeated(params: list[tuple[int, bytes]], param_id: int) -> list[bytes]:
    return [payload for current, payload in params if current == param_id]


def parse_text(payload: bytes) -> str:
    assert payload.endswith(b"\0")
    assert b"\0" not in payload[:-1]
    return payload[:-1].decode("utf-8")


def nested(payload: bytes) -> dict[int, bytes]:
    params = parse_tlvs(payload)
    ids = [param_id for param_id, _ in params]
    assert len(ids) == len(set(ids)), ids
    return dict(params)


def verify_identification(data: bytes, with_team: bool) -> None:
    msg_id, params = parse_frame(data)
    assert msg_id == 0x1D01
    values = parse_tlvs(params)
    ids = [param_id for param_id, _ in values]
    assert ids == ([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10] +
                   ([11] if with_team else []) + [12, 13, 17, 24])
    assert parse_text(unique(values, 0)) == "SCOURGE"
    assert parse_text(unique(values, 1)) == "GEN5W"
    sent = unique(values, 6)
    assert tuple(int.from_bytes(sent[i:i + 2], "big")
                 for i in range(0, len(sent), 2)) == SENT
    received = unique(values, 7)
    assert tuple(int.from_bytes(received[i:i + 2], "big")
                 for i in range(0, len(received), 2)) == RECEIVED
    assert unique(values, 8) == b"\0"
    assert unique(values, 9) == b"\0\0"
    if with_team:
        assert parse_text(unique(values, 11)) == "SCOURGETEAM"
    eap = nested(unique(values, 10))
    assert eap[0] == b"\x01"
    assert parse_text(eap[1]) == "com.scourge.carplay"
    assert eap[2] == b"\x04"
    assert 4 not in eap
    assert len(data) == (256 if with_team else 240)
    bluetooth = nested(unique(values, 17))
    assert int.from_bytes(bluetooth[0], "big") == 0
    assert parse_text(bluetooth[1]) == "bt"
    assert bluetooth[2] == b""
    assert bluetooth[3] == bytes.fromhex("aabbccddeeff")
    wireless = nested(unique(values, 24))
    assert int.from_bytes(wireless[0], "big") == 1
    assert parse_text(wireless[1]) == "SCOURGE-AP"
    assert wireless[2] == b"" and wireless[4] == b""
    for forbidden in (14, 15, 16, 20, 21, 22, 30):
        assert forbidden not in ids


def verify_wifi(data: bytes, ssid: str = "SCOURGE-AP",
                channel: int = 36) -> None:
    msg_id, params = parse_frame(data)
    assert msg_id == 0x5703
    values = parse_tlvs(params)
    assert [param_id for param_id, _ in values] == [1, 2, 3, 4]
    assert parse_text(unique(values, 1)) == ssid
    assert parse_text(unique(values, 2)) == "Wireless-Test-1"
    assert unique(values, 3) == b"\x02"
    assert unique(values, 4) == bytes([channel])


def verify_start(data: bytes, security_wire: int | None,
                 ssid: str = "SCOURGE-AP", channel: int = 36) -> None:
    msg_id, params = parse_frame(data)
    assert msg_id == 0x4301
    values = parse_tlvs(params)
    assert [param_id for param_id, _ in values] == [1, 2, 3, 4, 5]
    wireless = parse_tlvs(unique(values, 1))
    ids = [param_id for param_id, _ in wireless]
    expected_ids = [0, 1, 2, 3] + ([4] if security_wire is not None else [])
    assert ids == expected_ids
    assert parse_text(unique(wireless, 0)) == ssid
    assert parse_text(unique(wireless, 1)) == "Wireless-Test-1"
    assert unique(wireless, 2) == bytes([channel])
    assert parse_text(unique(wireless, 3)) == "fe80::1"
    if security_wire is not None:
        assert unique(wireless, 4) == bytes([security_wire])
    assert int.from_bytes(unique(values, 2), "big") == 7000
    assert parse_text(unique(values, 3)) == "SCOURGE-DEV"
    assert parse_text(unique(values, 4)) == "test-public-key"
    assert parse_text(unique(values, 5)) == "1.0.0"


def verify_downlink_fixtures() -> None:
    """Exercise the parser on independently built phone-to-accessory frames."""
    update = frame(0x4E0D, u8_param(0, 1))
    assert parse_frame(update)[0] == 0x4E0D
    assert unique(parse_tlvs(parse_frame(update)[1]), 0) == b"\x01"

    transport = frame(0x4E0E, b"".join((
        str_param(0, "AA:BB:CC:DD:EE:FF"),
        str_param(1, "usb-1"),
    )))
    values = parse_tlvs(parse_frame(transport)[1])
    assert parse_text(unique(values, 0)) == "AA:BB:CC:DD:EE:FF"
    assert parse_text(unique(values, 1)) == "usb-1"

    wired = tlv(0, b"".join((u8_param(0, 1), str_param(1, "usb-1"))))
    wireless = tlv(1, b"".join((u8_param(0, 1),
                                str_param(1, "AA:BB:CC:DD:EE:FF"))))
    availability = frame(0x4300, wired + wireless)
    values = parse_tlvs(parse_frame(availability)[1])
    wired_values = nested(unique(values, 0))
    wireless_values = nested(unique(values, 1))
    assert wired_values[0] == b"\x01"
    assert parse_text(wired_values[1]) == "usb-1"
    assert wireless_values[0] == b"\x01"
    assert parse_text(wireless_values[1]) == "AA:BB:CC:DD:EE:FF"

    rejected = frame(0x1D03, flag(0) + flag(17))
    values = parse_tlvs(parse_frame(rejected)[1])
    assert unique(values, 0) == b"" and unique(values, 17) == b""

    challenge = frame(0xAA02, tlv(0, bytes(range(1, 21))))
    values = parse_tlvs(parse_frame(challenge)[1])
    assert unique(values, 0) == bytes(range(1, 21))


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=True, **kwargs)


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


def build_binary(tmp: Path, name: str, extra: list[str]) -> Path:
    binary = tmp / name
    command = [
        os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
        "-Werror", "-pedantic", "-Wshadow", "-Wstrict-prototypes",
        "-Wmissing-prototypes", *extra, "-I", str(SRC), "-o", str(binary),
        str(TEST_C), str(SRC / "cp_wire.c"), str(SRC / "cp_bootstrap.c"),
    ]
    run(command, timeout=120)
    return binary


def check_sources() -> None:
    required = [
        SRC / "cp_wire.h",
        SRC / "cp_wire.c",
        SRC / "cp_bootstrap.h",
        SRC / "cp_bootstrap.c",
        TEST_C,
        PY_TEST,
    ]
    spdx = "SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors"
    license_line = "SPDX-License-Identifier: AGPL-3.0-only"
    for path in required:
        assert path.is_file(), path
        head = path.read_text(encoding="utf-8")[:400]
        assert spdx in head and license_line in head, path

    forbidden = (
        "malloc(", "calloc(", "realloc(", "free(", "strdup(", "asprintf(",
        "printf(", "fprintf(", "syslog(", "perror(", "fopen(", "socket(",
        "connect(", "system(", "popen(",
    )
    for name in ("cp_wire.c", "cp_bootstrap.c", "cp_wire.h",
                 "cp_bootstrap.h"):
        source = (SRC / name).read_text(encoding="utf-8")
        for token in forbidden:
            assert token not in source, (name, token)
        assert "#include <stdio.h>" not in source, name

    bootstrap = (SRC / "cp_bootstrap.c").read_text(encoding="utf-8")
    # Authentication success is assigned in exactly one place: the AA05 path.
    assignment_text = "session->auth_state = CP_BOOT_AUTH_SUCCEEDED;"
    assert bootstrap.count(assignment_text) == 1
    succeeded = bootstrap.index("handle_auth_succeeded")
    assignment = bootstrap.index(assignment_text)
    assert assignment > succeeded
    assert "cp_bootstrap_projection_active" in bootstrap
    assert "return false;" in bootstrap
    # The offer hold is a trusted, generation-bound API, never a wire state.
    # Its only assignment site is cp_bootstrap_hold_offer() itself.
    assert bootstrap.count("session->offer_held = true;") == 1
    hold_def = bootstrap.index("cp_boot_result cp_bootstrap_hold_offer(")
    hold_assign = bootstrap.index("session->offer_held = true;")
    inner_hold = bootstrap.index("cp_boot_result cp_bootstrap_release_offer_hold(")
    assert hold_def < hold_assign < inner_hold
    assert "generation != session->generation" in bootstrap
    assert "offer_hold_generation" in bootstrap
    header = (SRC / "cp_bootstrap.h").read_text(encoding="utf-8")
    assert "cp_bootstrap_hold_offer" in header
    assert "cp_bootstrap_release_offer_hold" in header
    assert "cp_bootstrap_offer_held" in header
    assert "never inferred from the wire" in header
    # A valid but changed on-air readback is an AP-scope change exactly like an
    # off-air readback: both clear the hold (and only the hold).
    assert "ap_scope_changed" in bootstrap
    scope = bootstrap[bootstrap.index("bool ap_scope_changed"):
                      bootstrap.index("memcpy(session->readback_ssid")]
    assert "session->offer_held = false;" in scope
    assert "session->offer_hold_generation = 0u;" in scope
    assert "session->offer_deadline_ms" not in scope
    # Failure paths keep using the central clear_offer() invalidation.
    ident_fail = bootstrap[bootstrap.index("static void fail_identification"):
                           bootstrap.index("static void fail_authentication")]
    auth_fail = bootstrap[bootstrap.index("static void fail_authentication"):
                          bootstrap.index("static bool id_equal_ci")]
    assert "clear_offer(session);" in ident_fail
    assert "clear_offer(session);" in auth_fail


def main() -> int:
    check_sources()
    verify_downlink_fixtures()

    for name, expected in EXPECTED.items():
        assert expected.hex() == PINNED_HEX[name], name
    verify_identification(EXPECTED["ident"], True)
    verify_identification(EXPECTED["ident_no_team"], False)
    verify_wifi(EXPECTED["wifi"])
    verify_start(EXPECTED["start_wpa2"], None)
    verify_start(EXPECTED["start_transition"], 3)
    verify_start(EXPECTED["start_wpa3"], 4)
    verify_start(EXPECTED["start_readback"], None, "CarPlay-AP", 149)

    output_root = Path("/tmp/opencode")
    output_root.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    with tempfile.TemporaryDirectory(prefix="carplay-c1-",
                                     dir=str(output_root)) as tmp_name:
        tmp = Path(tmp_name)
        binary = build_binary(tmp, "test_carplay_bootstrap", ["-O2"])
        tested = run([str(binary)], env=env, timeout=60, text=True,
                     capture_output=True)
        assert "checks passed" in tested.stdout, tested.stdout
        assert "FAIL" not in tested.stdout + tested.stderr
        print("PASS: cp_bootstrap normal strict build and tests")
        fixtures = run([str(binary), "--emit-fixtures"], env=env, timeout=60,
                       text=True, capture_output=True)
        emitted = {}
        for line in fixtures.stdout.splitlines():
            name, value = line.split()
            assert name not in emitted
            emitted[name] = bytes.fromhex(value)
        assert set(emitted) == set(EXPECTED)
        for name, expected in EXPECTED.items():
            assert emitted[name] == expected, name

        verify_identification(emitted["ident"], True)
        verify_identification(emitted["ident_no_team"], False)
        verify_wifi(emitted["wifi"])
        verify_start(emitted["start_wpa2"], None)
        verify_start(emitted["start_transition"], 3)
        verify_start(emitted["start_wpa3"], 4)
        verify_start(emitted["start_readback"], None, "CarPlay-AP", 149)
        assert emitted["aa01"] == EXPECTED["aa01"]
        assert emitted["aa03"] == EXPECTED["aa03"]

        compiler = os.environ.get("CC", "cc")
        if sanitizer_supported(compiler, tmp):
            san_binary = build_binary(
                tmp, "test_carplay_bootstrap_san",
                ["-O1", "-g", "-fno-omit-frame-pointer",
                 "-fsanitize=address,undefined"])
            san_env = dict(env)
            san_env["ASAN_OPTIONS"] = (
                "detect_leaks=1:abort_on_error=1:strict_string_checks=1")
            san_env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
            san = run([str(san_binary)], env=san_env, timeout=120,
                      text=True, capture_output=True)
            assert "checks passed" in san.stdout, san.stdout
            assert "FAIL" not in san.stdout + san.stderr
            print("PASS: cp_bootstrap ASan/UBSan build and tests")
        else:
            print("SKIP: compiler sanitizer runtime unavailable")

    print("carplay c1 python oracle: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
