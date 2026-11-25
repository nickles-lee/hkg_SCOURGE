#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""DHCP release-gate tests for Wifi1b.

Covers the client identifier in DISCOVER *and* REQUEST, address/mask helpers,
and DHCP server lease persistence/renewal. All sockets are loopback-only and
the whole test re-executes in a disposable network namespace when available.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
MAGIC = b"\x63\x82\x53\x63"
SERVER = "10.42.0.1"


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, capture_output=True, text=True,
                          **kwargs)


def nsenter(pid: int, command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return run(["nsenter", "-t", str(pid), "-n", "--", *command], **kwargs)


def packet(xid: int, mac: bytes, msgtype: int, requested: str | None = None,
           client_id: bytes | None = None, ciaddr: str | None = None,
           server_id: str | None = None, giaddr: str | None = None,
           flags: int = 0x8000) -> bytes:
    ci = int.from_bytes(socket.inet_aton(ciaddr), "big") if ciaddr else 0
    gi = int.from_bytes(socket.inet_aton(giaddr), "big") if giaddr else 0
    fixed = struct.pack("!BBBBIHHIIII16s64s128s", 1, 1, 6, 0, xid, 0,
                        flags, ci, 0, 0, gi, mac + b"\0" * 10,
                        b"\0" * 64, b"\0" * 128)
    opts = MAGIC + bytes([53, 1, msgtype, 55, 3, 1, 3, 6])
    if requested:
        opts += bytes([50, 4]) + socket.inet_aton(requested)
    if server_id:
        opts += bytes([54, 4]) + socket.inet_aton(server_id)
    if client_id:
        opts += bytes([61, len(client_id)]) + client_id
    opts += b"\xff"
    return fixed + opts


def fixed_fields(data: bytes) -> dict:
    op, htype, hlen, hops, xid, secs, flags, ci, yi, si, gi = struct.unpack(
        "!BBBBIHHIIII", data[:28])
    return {
        "op": op, "htype": htype, "hlen": hlen, "hops": hops,
        "xid": xid, "secs": secs, "flags": flags,
        "ciaddr": socket.inet_ntoa(struct.pack("!I", ci)),
        "yiaddr": socket.inet_ntoa(struct.pack("!I", yi)),
        "siaddr": socket.inet_ntoa(struct.pack("!I", si)),
        "giaddr": socket.inet_ntoa(struct.pack("!I", gi)),
    }


def parse(data: bytes):
    op, htype, hlen, hops, xid, secs, flags, ci, yi, si, gi = struct.unpack(
        "!BBBBIHHIIII", data[:28])
    opts = data[236:]
    assert opts[:4] == MAGIC
    out = {}
    i = 4
    while i < len(opts):
        code = opts[i]
        i += 1
        if code == 0:
            continue
        if code == 255:
            break
        length = opts[i]
        i += 1
        out[code] = opts[i:i + length]
        i += length
    return op, socket.inet_ntoa(struct.pack("!I", yi)), out


def assert_zero_padding(data: bytes) -> None:
    opts = data[236:]
    i = 4
    while i < len(opts):
        code = opts[i]
        i += 1
        if code == 0:
            continue
        if code == 255:
            break
        length = opts[i]
        i += 1 + length
    else:
        raise AssertionError("missing DHCP end option")
    assert all(byte == 0 for byte in opts[i:]), "non-zero DHCP padding"


def reply(xid: int, mac: bytes, msgtype: int, yiaddr: str) -> bytes:
    fixed = struct.pack("!BBBBIHHIIII16s64s128s", 2, 1, 6, 0, xid, 0,
                        0x8000, 0, int.from_bytes(socket.inet_aton(yiaddr), "big"),
                        int.from_bytes(socket.inet_aton(SERVER), "big"), 0,
                        mac + b"\0" * 10, b"\0" * 64, b"\0" * 128)
    opts = MAGIC + bytes([53, 1, msgtype, 54, 4]) + \
        socket.inet_aton(SERVER) + bytes([51, 4]) + struct.pack("!I", 3600)
    opts += bytes([1, 4]) + socket.inet_aton("255.255.255.0")
    opts += bytes([3, 4]) + socket.inet_aton(SERVER) + b"\xff"
    return fixed + opts


def test_client_id(binary: Path, env: dict) -> None:
    rp_all = Path("/proc/sys/net/ipv4/conf/all/rp_filter")
    rp_lo = Path("/proc/sys/net/ipv4/conf/lo/rp_filter")
    before_rp = (rp_all.read_text() if rp_all.exists() else "",
                 rp_lo.read_text() if rp_lo.exists() else "")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", 6870))
    sock.settimeout(8)
    client = subprocess.Popen([str(binary), "--dhcp-client"], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True)
    try:
        discover, client_addr = sock.recvfrom(1500)
        op, _yi, opts1 = parse(discover)
        assert op == 1 and opts1[53] == b"\x01", opts1
        assert len(discover) >= 300, "DISCOVER below DHCP minimum 300"
        assert_zero_padding(discover)
        assert 61 in opts1, "DISCOVER omitted client identifier"
        xid = struct.unpack("!I", discover[4:8])[0]
        mac = discover[28:34]
        sock.sendto(reply(xid, mac, 2, "10.42.0.10"), client_addr)

        request, client_addr = sock.recvfrom(1500)
        op, _yi, opts2 = parse(request)
        assert op == 1 and opts2[53] == b"\x03", opts2
        assert len(request) >= 300, "REQUEST below DHCP minimum 300"
        assert_zero_padding(request)
        assert 61 in opts2, "REQUEST omitted client identifier"
        assert opts1[61] == opts2[61], (opts1[61], opts2[61])
        assert opts2[61][0] == 1 and opts2[61][1:] == mac
        assert opts2.get(50) == socket.inet_aton("10.42.0.10")
        sock.sendto(reply(xid, mac, 5, "10.42.0.10"), client_addr)
        out, err = client.communicate(timeout=10)
        assert client.returncode == 0, (out, err)
        parsed = json.loads(out)
        assert parsed["ok"] is True and parsed["ip"] == "10.42.0.10", parsed
        after_rp = (rp_all.read_text() if rp_all.exists() else "",
                    rp_lo.read_text() if rp_lo.exists() else "")
        assert before_rp == after_rp, (before_rp, after_rp)
    finally:
        if client.poll() is None:
            client.kill()
            client.wait()
        sock.close()
    print("PASS: DHCP client sends the same option 61 in DISCOVER and REQUEST")


def route_lines() -> list[str]:
    return [line for line in subprocess.run(
        ["ip", "-4", "route", "show"], capture_output=True, text=True).stdout.splitlines()
        if line.startswith("default")]


def setup_modem_default(metric: int) -> None:
    subprocess.run(["ip", "link", "add", "modem0", "type", "dummy"],
                   check=False, capture_output=True)
    subprocess.run(["ip", "link", "set", "modem0", "up"], check=False,
                   capture_output=True)
    subprocess.run(["ip", "-4", "addr", "replace", "192.0.2.1/24",
                    "dev", "modem0"], check=False, capture_output=True)
    subprocess.run(["ip", "-4", "route", "del", "default", "dev", "modem0"],
                   check=False, capture_output=True)
    run(["ip", "-4", "route", "add", "default", "via", "192.0.2.2",
         "dev", "modem0", "metric", str(metric)])


def owned_route_lines() -> list[str]:
    return [line for line in route_lines() if " proto 66 " in line]


def ctest_json(binary: Path, *args: str, check: bool = True) -> dict:
    result = subprocess.run([str(binary), *args], capture_output=True,
                            text=True)
    if check:
        assert result.returncode == 0, (result.returncode, result.stdout,
                                        result.stderr)
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError:
        return {"ok": False, "raw": result.stdout, "stderr": result.stderr}


def test_modem_safe(ctest: Path) -> None:
    """modem_safe suppresses the WiFi default while rmnet has IPv4 and
    restores the high-metric fallback when the modem data call is gone."""
    subprocess.run(["ip", "-4", "route", "del", "default", "dev", "modem0"],
                   check=False, capture_output=True)
    subprocess.run(["ip", "link", "add", "rmnet_usb0", "type", "dummy"],
                   check=False, capture_output=True)
    subprocess.run(["ip", "link", "set", "rmnet_usb0", "up"],
                   check=False, capture_output=True)
    run(["ip", "-4", "addr", "replace", "198.51.100.135/28",
         "dev", "rmnet_usb0"])
    run(["ip", "-4", "route", "add", "default", "via", "198.51.100.136",
         "dev", "rmnet_usb0", "metric", "0"])

    result = ctest_json(ctest, "--route-install", "10.42.0.1", "lo")
    assert result["ok"] is True and result["modem"] is True, result
    assert result["wifi_default"] is False and result["route_ok"] is True, result
    assert result["metric"] == 0 and result["proto"] == 0, result
    assert not owned_route_lines()
    assert any("rmnet_usb0" in line for line in route_lines())

    run(["ip", "-4", "addr", "del", "198.51.100.135/28", "dev", "rmnet_usb0"])
    # Deleting the address may already remove its default route.
    subprocess.run(["ip", "-4", "route", "del", "default",
                    "via", "198.51.100.136", "dev", "rmnet_usb0",
                    "metric", "0"], check=False, capture_output=True)
    result = ctest_json(ctest, "--route-install", "10.42.0.1", "lo")
    assert result["ok"] is True and result["modem"] is False, result
    assert result["wifi_default"] is True and result["metric"] == 1000, result
    owned = owned_route_lines()
    assert len(owned) == 1 and "proto 66" in owned[0], owned
    assert ctest_json(ctest, "--route-cleanup")["ok"] is True
    subprocess.run(["ip", "link", "del", "rmnet_usb0"],
                   check=False, capture_output=True)
    setup_modem_default(100)
    print("PASS: modem_safe suppresses/restores the WiFi default by modem state")


def test_route_audit_no_flap(ctest: Path, app: Path) -> None:
    """The 30 s audit must recognise its own dev-qualified wlan0 route and
    not delete/re-add it.  The target iproute2 omits `dev wlan0` from a
    filtered query, so this uses a real dummy wlan0 and the unfiltered table."""
    subprocess.run(["ip", "link", "add", "wlan0", "type", "dummy"],
                   check=False, capture_output=True)
    subprocess.run(["ip", "link", "set", "wlan0", "up"],
                   check=False, capture_output=True)
    run(["ip", "-4", "addr", "replace", "10.42.0.2/24", "dev", "wlan0"])
    try:
        result = ctest_json(ctest, "--route-install", "10.42.0.1", "wlan0")
        assert result["ok"] is True and result["wifi_default"] is True, result
        ledger = app / "client.route"
        before = ledger.stat().st_ino
        first = ctest_json(ctest, "--route-audit")
        second = ctest_json(ctest, "--route-audit")
        assert first["ok"] is True and second["ok"] is True
        assert first["wifi_default"] is True and second["wifi_default"] is True
        assert ledger.stat().st_ino == before, (
            "audit rewrote the owned route/ledger; dev-qualified matching failed")
        assert any("proto 66" in line for line in owned_route_lines())
        assert ctest_json(ctest, "--route-cleanup")["ok"] is True
    finally:
        subprocess.run(["ip", "link", "del", "wlan0"],
                       check=False, capture_output=True)
    print("PASS: route audit recognises its own wlan0 default without flapping")


def test_client_apply(binary: Path, ctest: Path, env: dict) -> None:
    apply_env = dict(env)
    apply_env.pop("WIFI1B_DHCP_TEST_NOAPPLY", None)
    setup_modem_default(100)
    # The client verifies the connected route. A loopback address does not
    # create one, so seed the isolated namespace with the equivalent route.
    subprocess.run(["ip", "-4", "route", "replace", "10.42.0.0/24", "dev", "lo"],
                   check=True, capture_output=True)
    before_defaults = route_lines()
    assert any("metric 100" in line and "modem0" in line
               for line in before_defaults), before_defaults
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", 6870))
    sock.settimeout(8)
    client = subprocess.Popen([str(binary), "--dhcp-client"], env=apply_env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True)
    try:
        discover, client_addr = sock.recvfrom(1500)
        xid = struct.unpack("!I", discover[4:8])[0]
        mac = discover[28:34]
        sock.sendto(reply(xid, mac, 2, "10.42.0.10"), client_addr)
        request, client_addr = sock.recvfrom(1500)
        assert parse(request)[2].get(61) == b"\x01" + mac
        sock.sendto(reply(xid, mac, 5, "10.42.0.10"), client_addr)
        out, err = client.communicate(timeout=10)
        assert client.returncode == 0, (out, err)
        assert json.loads(out)["ok"] is True
        addr = subprocess.run(["ip", "-o", "-4", "addr", "show", "dev", "lo"],
                              capture_output=True, text=True).stdout
        assert "10.42.0.10/24" in addr, addr
        connected = subprocess.run(["ip", "-4", "route", "show", "dev", "lo"],
                                   capture_output=True, text=True).stdout
        assert "10.42.0.0/24" in connected, connected
        after_defaults = route_lines()
        assert all(line in after_defaults for line in before_defaults), (
            before_defaults, after_defaults)
        owned = owned_route_lines()
        assert len(owned) == 1, owned
        assert "via 10.42.0.1" in owned[0] and "dev lo" in owned[0], owned
        assert "metric 101" in owned[0], owned
        assert "metric 100" in " ".join(before_defaults)
    finally:
        if client.poll() is None:
            client.kill()
            client.wait()
        sock.close()
    cleaned = ctest_json(ctest, "--route-cleanup")
    assert cleaned["ok"] is True
    assert not owned_route_lines()
    assert all(line in route_lines() for line in before_defaults)
    print("PASS: DHCP client verifies address/readback and installs only an "
          "owned lower-priority default route")


def test_renew_rebind(ctest: Path, env: dict) -> None:
    mac = "02:00:11:22:33:44"
    for step in ("renew", "rebind"):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("127.0.0.1", 6870))
        sock.settimeout(8)
        client = subprocess.Popen(
            [str(ctest), "--renew-client", step, "10.42.0.10",
             "127.0.0.1", mac], env=env,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            request, client_addr = sock.recvfrom(1500)
            assert len(request) >= 300, "RENEW/REBIND below DHCP minimum 300"
            assert_zero_padding(request)
            xid = struct.unpack("!I", request[4:8])[0]
            opts = parse(request)[2]
            assert socket.inet_ntoa(request[12:16]) == "10.42.0.10", (
                step, request[12:16])
            assert opts[53] == b"\x03", opts
            assert opts.get(61) == b"\x01" + bytes.fromhex(mac.replace(":", "")), (
                step, opts)
            assert 50 not in opts and 54 not in opts, (step, opts)
            sock.sendto(reply(xid, bytes.fromhex(mac.replace(":", "")), 5,
                              "10.42.0.10"), client_addr)
            out, err = client.communicate(timeout=10)
            assert client.returncode == 0, (step, out, err)
            assert json.loads(out)["ok"] is True, (step, out)
        finally:
            if client.poll() is None:
                client.kill()
                client.wait()
            sock.close()
    print("PASS: RENEW/REBIND requests carry ciaddr and the same client id")


def test_route_policy(ctest: Path) -> None:
    # Existing lower metric -> owned route is strictly higher.
    result = ctest_json(ctest, "--route-install", "10.42.0.1", "lo")
    assert result["ok"] is True and result["metric"] == 101, result
    assert any("via 10.42.0.1" in l and "metric 101" in l
               and "proto 66" in l for l in route_lines())
    assert any("via 192.0.2.2" in l and "metric 100" in l
               for l in route_lines())

    # Lease gateway change removes the old owned route and installs the new.
    result = ctest_json(ctest, "--route-install", "10.42.0.2", "lo")
    assert result["ok"] is True and result["metric"] == 101, result
    owned = owned_route_lines()
    assert len(owned) == 1 and "via 10.42.0.2" in owned[0], owned
    assert "via 10.42.0.1" not in " ".join(owned)

    # Explicit cleanup removes only our route; foreign stays unchanged.
    assert ctest_json(ctest, "--route-cleanup")["ok"] is True
    assert not owned_route_lines()
    assert any("via 192.0.2.2" in l and "metric 100" in l
               for l in route_lines())

    # Existing higher metric -> owned route is still above it.
    subprocess.run(["ip", "-4", "route", "del", "default", "dev", "modem0"],
                   check=False, capture_output=True)
    run(["ip", "-4", "route", "add", "default", "via", "192.0.2.2",
         "dev", "modem0", "metric", "500"])
    result = ctest_json(ctest, "--route-install", "10.42.0.1", "lo")
    assert result["ok"] is True and result["metric"] == 501, result
    assert ctest_json(ctest, "--route-cleanup")["ok"] is True
    assert any("metric 500" in l and "modem0" in l for l in route_lines())

    # No existing default -> bounded high fallback metric, never 0.
    subprocess.run(["ip", "-4", "route", "del", "default", "dev", "modem0"],
                   check=False, capture_output=True)
    assert not route_lines()
    result = ctest_json(ctest, "--route-install", "10.42.0.1", "lo")
    assert result["ok"] is True and result["metric"] == 1000, result
    assert ctest_json(ctest, "--route-cleanup")["ok"] is True

    # Installation failure is false and leaves no owned ledger/route.
    failure = ctest_json(ctest, "--route-install", "203.0.113.1", "lo",
                         check=False)
    assert failure["ok"] is False, failure
    assert not owned_route_lines()

    # Restore the lower foreign default for the raw-client route assertion.
    setup_modem_default(100)
    print("PASS: owned default-route metric, gateway change and cleanup policy")


def _checksum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\x00"
    total = sum(struct.unpack(f"!{len(data)//2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def raw_dhcp_reply(xid: int, mac: bytes, msgtype: int, yiaddr: str,
                   server: str, client_port: int, server_port: int) -> bytes:
    fixed = struct.pack("!BBBBIHHIIII16s64s128s", 2, 1, 6, 0, xid, 0,
                        0x8000, 0, int.from_bytes(socket.inet_aton(yiaddr), "big"),
                        int.from_bytes(socket.inet_aton(server), "big"), 0,
                        mac + b"\0" * 10, b"\0" * 64, b"\0" * 128)
    opts = MAGIC + bytes([53, 1, msgtype, 54, 4]) + socket.inet_aton(server)
    opts += bytes([1, 4]) + socket.inet_aton("255.255.255.0")
    opts += bytes([3, 4]) + socket.inet_aton(server)
    opts += bytes([51, 4]) + struct.pack("!I", 3600)
    opts += bytes([58, 4]) + struct.pack("!I", 1800)
    opts += bytes([59, 4]) + struct.pack("!I", 3150)
    opts += bytes([61, 7, 1]) + mac + b"\xff"
    dhcp = fixed + opts
    udp_len = 8 + len(dhcp)
    udp = struct.pack("!HHHH", server_port, client_port, udp_len, 0) + dhcp
    pseudo = (socket.inet_aton(server) + socket.inet_aton("255.255.255.255") +
              b"\0\x11" + struct.pack("!H", udp_len))
    udp_sum = _checksum(pseudo + udp)
    udp = struct.pack("!HHHH", server_port, client_port, udp_len, udp_sum) + dhcp
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 1, 0x4000,
                     64, 17, 0, socket.inet_aton(server),
                     socket.inet_aton("255.255.255.255"))
    ip = ip[:10] + struct.pack("!H", _checksum(ip)) + ip[12:]
    return ip + udp


def test_nak_destination_netns(binary: Path, env: dict) -> None:
    """Capture a NAK sent to 255.255.255.255 in a private netns.

    The request carries a non-local ciaddr; receiving the NAK on a wildcard
    socket proves the server did not use TEST_LOOPBACK or unicast the NAK to
    the wrong address.
    """
    subprocess.run(["ip", "-4", "route", "del", "10.42.0.0/24", "dev", "lo"],
                   check=False, capture_output=True)
    subprocess.run(["ip", "link", "del", "wlan0"], check=False,
                   capture_output=True)
    run(["ip", "link", "add", "wlan0", "type", "dummy"])
    run(["ip", "link", "set", "wlan0", "up"])
    run(["ip", "-4", "addr", "add", "10.42.0.1/24", "dev", "wlan0"])
    server_env = dict(env)
    server_env.pop("WIFI1B_DHCP_TEST_LOOPBACK", None)
    server_env["WIFI1B_DHCP_TEST_PORT"] = "6881"
    server_env["WIFI1B_DHCP_TEST_CLIENT_PORT"] = "6882"
    server = subprocess.Popen([str(binary), "--dhcpd"], env=server_env,
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", 6882))
    sock.settimeout(3)
    try:
        mac = bytes.fromhex("020000000011")
        pkt = packet(0x12345678, mac, 3, requested="10.42.0.250",
                     ciaddr="10.42.0.99", client_id=b"\x01" + mac)
        data = None
        for _ in range(10):
            sock.sendto(pkt, ("10.42.0.1", 6881))
            try:
                data, _ = sock.recvfrom(1500)
                break
            except socket.timeout:
                time.sleep(0.1)
        assert data is not None, "no broadcast NAK received"
        assert len(data) >= 300, "broadcast NAK below DHCP minimum 300"
        fixed = fixed_fields(data)
        assert fixed["ciaddr"] == "0.0.0.0", fixed
        assert fixed["yiaddr"] == "0.0.0.0", fixed
        assert fixed["siaddr"] == "0.0.0.0", fixed
        assert fixed["giaddr"] == "0.0.0.0", fixed
        assert fixed["flags"] == 0x8000, fixed
        opts = parse(data)[2]
        assert opts[53] == b"\x06", opts
        assert 54 in opts, opts
        for forbidden in (1, 3, 6, 28, 51, 58, 59):
            assert forbidden not in opts, (forbidden, opts)
        assert set(opts).issubset({53, 54}), opts
        assert_zero_padding(data)

        # INFORM is addressed to ciaddr, retains yiaddr=0 and omits leases.
        sock.close()
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("10.42.0.1", 6882))
        sock.settimeout(3)
        inform = packet(0x2222, mac, 8, ciaddr="10.42.0.1",
                        client_id=b"\x01" + mac)
        sock.sendto(inform, ("10.42.0.1", 6881))
        inform_data, _ = sock.recvfrom(1500)
        assert len(inform_data) >= 300
        info_fixed = fixed_fields(inform_data)
        assert info_fixed["ciaddr"] == "10.42.0.1", info_fixed
        assert info_fixed["yiaddr"] == "0.0.0.0", info_fixed
        assert info_fixed["siaddr"] == "0.0.0.0", info_fixed
        info_opts = parse(inform_data)[2]
        assert info_opts[53] == b"\x05", info_opts
        for forbidden in (51, 58, 59):
            assert forbidden not in info_opts, (forbidden, info_opts)
        assert_zero_padding(inform_data)
    finally:
        sock.close()
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
        subprocess.run(["ip", "link", "del", "wlan0"], check=False,
                       capture_output=True)
    print("PASS: non-loopback private-netns NAK uses broadcast and NAK fields")


def test_raw_client(ctest: Path, env: dict, tmp_path: Path) -> None:
    if shutil.which("nsenter") is None or shutil.which("unshare") is None:
        print("SKIP raw AF_PACKET client: nsenter/unshare unavailable")
        return
    run(["ip", "link", "add", "wlan0", "type", "veth", "peer", "name", "cpeer0"])
    peer = subprocess.Popen(
        ["unshare", "-n", "sh", "-c", "ip link set lo up; exec sleep 300"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    try:
        own_netns = os.readlink("/proc/self/ns/net")
        deadline = time.monotonic() + 5
        while True:
            if peer.poll() is not None:
                raise AssertionError(f"peer namespace exited: {peer.stderr.read()}")
            try:
                if os.readlink(f"/proc/{peer.pid}/ns/net") != own_netns:
                    break
            except OSError:
                pass
            if time.monotonic() >= deadline:
                raise AssertionError("peer network namespace did not appear")
            time.sleep(0.05)
        run(["ip", "link", "set", "cpeer0", "netns", str(peer.pid)])
        run(["ip", "link", "set", "lo", "up"])
        run(["ip", "link", "set", "wlan0", "up"])
        nsenter(peer.pid, ["ip", "link", "set", "lo", "up"])
        deadline = time.monotonic() + 5
        while True:
            if peer.poll() is not None:
                raise AssertionError(f"peer namespace exited: {peer.stderr.read()}")
            try:
                nsenter(peer.pid, ["ip", "link", "set", "cpeer0", "up"])
                link = nsenter(peer.pid, ["ip", "link", "show", "cpeer0"]).stdout
                break
            except subprocess.CalledProcessError as error:
                if time.monotonic() >= deadline:
                    print("nsenter failed:", error.stderr)
                    print("peer poll:", peer.poll(),
                          "stderr:", peer.stderr.read() if peer.poll() is not None else "")
                    raise
                time.sleep(0.05)
        peer_mac = next(line.split()[1] for line in link.splitlines()
                        if "link/ether" in line)
        subprocess.run(["ip", "-4", "route", "del", "10.42.0.0/24", "dev", "lo"],
                       check=False, capture_output=True)
        mac = "02:00:11:22:33:44"
        raw_env = dict(env)
        raw_env.pop("WIFI1B_DHCP_TEST_NOAPPLY", None)
        raw_env.update({
            "WIFI1B_DHCP_TEST_LOOPBACK": "1",
            "WIFI1B_DHCP_TEST_IFACE": "wlan0",
            "WIFI1B_DHCP_TEST_FORCE_RAW": "1",
            "WIFI1B_DHCP_TEST_PORT": "6870",
            "WIFI1B_DHCP_TEST_CLIENT_PORT": "6869",
        })
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("127.0.0.1", 6870))
        sock.settimeout(10)
        client = subprocess.Popen([str(ctest), "--raw-client", mac],
                                  env=raw_env, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        try:
            discover, client_addr = sock.recvfrom(1500)
            xid = struct.unpack("!I", discover[4:8])[0]
            chaddr = discover[28:34]
            assert chaddr == bytes.fromhex(mac.replace(":", ""))
            offer = raw_dhcp_reply(xid, chaddr, 2, "10.42.0.10",
                                   "10.42.0.1", 6869, 6870)
            frame = (b"\xff" * 6 + bytes.fromhex(peer_mac.replace(":", "")) +
                     b"\x08\x00" + offer)
            frame_file = tmp_path / "raw-offer.bin"
            frame_file.write_bytes(frame)
            nsenter(peer.pid, [sys.executable, "-c", (
                "import socket,sys\n"
                "frame=open(sys.argv[1],'rb').read()\n"
                "s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(0x0800))\n"
                "s.bind(('cpeer0',0))\n"
                "s.send(frame)\n"), str(frame_file)])
            request, client_addr = sock.recvfrom(1500)
            opts = parse(request)[2]
            assert opts.get(61) == b"\x01" + chaddr
            ack = raw_dhcp_reply(xid, chaddr, 5, "10.42.0.10",
                                 "10.42.0.1", 6869, 6870)
            frame = (b"\xff" * 6 + bytes.fromhex(peer_mac.replace(":", "")) +
                     b"\x08\x00" + ack)
            frame_file.write_bytes(frame)
            nsenter(peer.pid, [sys.executable, "-c", (
                "import socket,sys\n"
                "frame=open(sys.argv[1],'rb').read()\n"
                "s=socket.socket(socket.AF_PACKET,socket.SOCK_RAW,socket.htons(0x0800))\n"
                "s.bind(('cpeer0',0))\n"
                "s.send(frame)\n"), str(frame_file)])
            out, err = client.communicate(timeout=15)
            assert client.returncode == 0, (out, err)
            parsed = json.loads(out)
            assert parsed["ok"] is True and parsed["route_ok"] is True, parsed
            addr = subprocess.run(["ip", "-o", "-4", "addr", "show", "dev", "wlan0"],
                                  capture_output=True, text=True).stdout
            assert "10.42.0.10/24" in addr, addr
            owned = owned_route_lines()
            assert len(owned) == 1 and "via 10.42.0.1" in owned[0], owned
            assert "metric 101" in owned[0], owned
            assert ctest_json(ctest, "--route-cleanup")["ok"] is True
            assert not owned_route_lines()
            assert any("metric 100" in l and "modem0" in l
                       for l in route_lines())
        finally:
            if client.poll() is None:
                client.kill()
                client.wait()
            sock.close()
    finally:
        peer.terminate()
        try:
            peer.wait(timeout=3)
        except subprocess.TimeoutExpired:
            peer.kill()
    print("PASS: AF_PACKET raw DHCP fallback drives a full lease without sysctls")


def start_server(binary: Path, app: Path, env: dict) -> subprocess.Popen:
    proc = subprocess.Popen([str(binary), "--dhcpd"], env=env,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + 5
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("127.0.0.1", 6872))
    finally:
        sock.close()
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise AssertionError("DHCP server exited during startup")
        time.sleep(0.1)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("127.0.0.1", 6871))
            sock.close()
            return proc
        except OSError:
            sock.close()
            time.sleep(0.2)
    proc.kill()
    raise AssertionError("DHCP server did not open the test port")


def send_recv(sock: socket.socket, data: bytes) -> bytes:
    for _ in range(10):
        sock.sendto(data, ("127.0.0.1", 6871))
        try:
            reply_data, _ = sock.recvfrom(1500)
            return reply_data
        except socket.timeout:
            time.sleep(0.1)
    raise AssertionError("no DHCP reply")


def test_server_leases(binary: Path, app: Path, env: dict) -> None:
    now = int(time.time())
    leases = app / "dhcp.leases"
    leases.write_text(
        "# mac ip expires_unix\n"
        f"02:00:00:00:00:0a 10.42.0.10 {now + 120}\n"
        f"02:00:00:00:00:0b 10.42.0.11 {now - 10}\n"
        f"02:00:00:00:00:0e 10.42.0.12 {now + 120} extra\n"
        "not-a-mac 10.42.0.13 " + f"{now + 120}\n"
        "02:00:00:00:00:0c 10.42.0.250 " + f"{now + 120}\n")
    server = start_server(binary, app, env)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", 6872))
    sock.settimeout(3)
    try:
        # The persisted .10 lease must not be offered to a different client.
        mac_b = bytes.fromhex("02000000000b")
        discover_b = packet(0x1001, mac_b, 1,
                            client_id=b"\x01" + mac_b)
        offer_data = send_recv(sock, discover_b)
        op, yi, opts = parse(offer_data)
        assert len(offer_data) >= 300, "OFFER below DHCP minimum 300"
        assert_zero_padding(offer_data)
        assert op == 2 and opts[53] == b"\x02" and yi == "10.42.0.11", (op, yi, opts)
        request_b = packet(0x1001, mac_b, 3, requested=yi,
                           client_id=b"\x01" + mac_b)
        ack_data = send_recv(sock, request_b)
        op, yi2, opts2 = parse(ack_data)
        assert len(ack_data) >= 300, "ACK below DHCP minimum 300"
        assert_zero_padding(ack_data)
        assert op == 2 and opts2[53] == b"\x05" and yi2 == "10.42.0.11"

        # A real renewal carries ciaddr and no requested-IP option.
        renew_b = send_recv(sock, packet(0x0906, mac_b, 3,
                                         ciaddr="10.42.0.11",
                                         client_id=b"\x01" + mac_b))
        op, yi_renew, opts_renew = parse(renew_b)
        assert op == 2 and opts_renew[53] == b"\x05" and yi_renew == "10.42.0.11"
        assert_zero_padding(renew_b)

        # Identity/address mismatch and unknown renewal are NAKed, not reused.
        bad_identity = packet(0x0907, mac_b, 3, requested="10.42.0.10",
                              client_id=b"\x01" + mac_b)
        op, yi_bad, opts_bad = parse(send_recv(sock, bad_identity))
        assert op == 2 and opts_bad[53] == b"\x06" and yi_bad == "0.0.0.0"
        mac_e = bytes.fromhex("02000000000e")
        unknown_renew = packet(0x0908, mac_e, 3, ciaddr="10.42.0.150",
                               client_id=b"\x01" + mac_e)
        op, yi_unknown, opts_unknown = parse(send_recv(sock, unknown_renew))
        assert op == 2 and opts_unknown[53] == b"\x06" and yi_unknown == "0.0.0.0"

        # INFORM must have no yiaddr/lease timers.
        inform_data = send_recv(sock, packet(0x0902, mac_b, 8,
                                             ciaddr="127.0.0.1",
                                             client_id=b"\x01" + mac_b))
        op, yi_info, opts_info = parse(inform_data)
        assert len(inform_data) >= 300, "INFORM ACK below DHCP minimum 300"
        assert_zero_padding(inform_data)
        assert op == 2 and yi_info == "0.0.0.0", (op, yi_info)
        inform_fixed = fixed_fields(inform_data)
        assert inform_fixed["ciaddr"] == "127.0.0.1"
        assert inform_fixed["yiaddr"] == "0.0.0.0"
        assert inform_fixed["siaddr"] == "0.0.0.0"
        assert inform_fixed["giaddr"] == "0.0.0.0"
        assert opts_info[53] == b"\x05" and 54 in opts_info
        assert 51 not in opts_info and 58 not in opts_info and 59 not in opts_info

        # A REQUEST for another server's identifier is ignored, not ACKed.
        sock.settimeout(0.8)
        wrong_sid = packet(0x0903, bytes.fromhex("02000000000d"), 3,
                           requested="10.42.0.10", server_id="10.42.0.2",
                           client_id=b"\x01" + bytes.fromhex("02000000000d"))
        sock.sendto(wrong_sid, ("127.0.0.1", 6871))
        try:
            sock.recvfrom(1500)
            raise AssertionError("REQUEST with wrong server-id got a reply")
        except socket.timeout:
            pass
        sock.settimeout(3)

        # Relayed requests are unsupported and must not get a wrong-destination
        # ACK/NAK.
        relay = packet(0x0910, bytes.fromhex("020000000010"), 3,
                       requested="10.42.0.10", giaddr="10.42.0.254",
                       client_id=b"\x01" + bytes.fromhex("020000000010"))
        sock.settimeout(0.8)
        sock.sendto(relay, ("127.0.0.1", 6871))
        try:
            sock.recvfrom(1500)
            raise AssertionError("relayed REQUEST got a reply")
        except socket.timeout:
            pass
        sock.settimeout(3)

        # Out-of-pool and in-use requested addresses must be NAKed, never
        # allocated outside the configured pool.
        mac_c = bytes.fromhex("02000000000c")
        out_pool = packet(0x0904, mac_c, 3, requested="10.42.0.250",
                          client_id=b"\x01" + mac_c)
        pool_data = send_recv(sock, out_pool)
        op, yi_pool, opts_pool = parse(pool_data)
        assert len(pool_data) >= 300, "NAK below DHCP minimum 300"
        assert_zero_padding(pool_data)
        assert op == 2 and opts_pool[53] == b"\x06" and yi_pool == "0.0.0.0"
        nak_fixed = fixed_fields(pool_data)
        assert nak_fixed["ciaddr"] == "0.0.0.0", nak_fixed
        assert nak_fixed["yiaddr"] == "0.0.0.0", nak_fixed
        assert nak_fixed["siaddr"] == "0.0.0.0", nak_fixed
        assert nak_fixed["giaddr"] == "0.0.0.0", nak_fixed
        assert nak_fixed["flags"] == 0x8000, nak_fixed
        for forbidden in (1, 3, 6, 28, 51, 58, 59):
            assert forbidden not in opts_pool, (forbidden, opts_pool)
        assert set(opts_pool).issubset({53, 54}), opts_pool

        # NAK flags are copied from the request independently of the
        # broadcast destination; a clear broadcast flag stays clear.
        mac_clear = bytes.fromhex("02000000000e")
        out_pool_clear = packet(0x0906, mac_clear, 3,
                                requested="10.42.0.250",
                                client_id=b"\x01" + mac_clear, flags=0)
        clear_data = send_recv(sock, out_pool_clear)
        assert fixed_fields(clear_data)["flags"] == 0
        assert parse(clear_data)[2][53] == b"\x06"
        mac_d = bytes.fromhex("02000000000d")
        in_use = packet(0x0905, mac_d, 3, requested="10.42.0.11",
                        client_id=b"\x01" + mac_d)
        op, yi_used, opts_used = parse(send_recv(sock, in_use))
        assert op == 2 and opts_used[53] == b"\x06" and yi_used == "0.0.0.0"
        assert "10.42.0.250" not in leases.read_text()

        # Renewal of the loaded .10 lease must refresh its lifetime and persist.
        before = leases.read_text()
        mac_a = bytes.fromhex("02000000000a")
        renew_a = packet(0x1002, mac_a, 3, requested="10.42.0.10",
                         client_id=b"\x01" + mac_a)
        op, yi3, opts3 = parse(send_recv(sock, renew_a))
        assert op == 2 and opts3[53] == b"\x05" and yi3 == "10.42.0.10"

        def expiry(text: str, mac: str) -> int:
            for line in text.splitlines():
                if line.startswith(mac):
                    return int(line.split()[2])
            raise AssertionError(f"missing lease {mac}")

        # The server persists immediately after sending the ACK; allow that
        # short window before asserting the renewal hit the file.
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            after = leases.read_text()
            try:
                if expiry(after, "02:00:00:00:00:0a") > expiry(
                        before, "02:00:00:00:00:0a") + 3000:
                    break
            except AssertionError:
                pass
            time.sleep(0.05)
        else:
            raise AssertionError((before, after))
        assert "02:00:00:00:00:0a 10.42.0.10" in after, after
        assert "02:00:00:00:00:0b 10.42.0.11" in after, after
        assert "10.42.0.12" not in after, after
        assert "10.42.0.13" not in after, after
        assert "10.42.0.250" not in after, after
    finally:
        sock.close()
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
    print("PASS: DHCP server loads persisted leases and refreshes renewals")


def inner(isolated: bool) -> int:
    if os.geteuid() == 0:
        subprocess.run(["ip", "link", "set", "lo", "up"], check=False,
                       capture_output=True)
    with tempfile.TemporaryDirectory(prefix="wifi1b-dhcp-gates-",
                                     dir="/tmp/opencode") as tmp:
        tmp_path = Path(tmp)
        app = tmp_path / "app"
        app.mkdir()
        binary = tmp_path / "wifi1b_daemon_dhcp"
        ctest = tmp_path / "test_carplay_dhcp"
        subprocess.run([
            "gcc", "-O2", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-Wno-format-truncation",
            "-Wno-unused-function", "-Wno-unused-variable",
            "-Wno-unused-but-set-variable",
            "-DWIFI_TEST", f'-DAPP_DIR="{app}"',
            f'-DRADIO_OWNER_PATH="{app}/radio.owner"',
            '-DWIFI_BUILD_ID="test"',
            "-static", "-o", str(binary),
            str(ROOT / "src/wifi1b_daemon.c")], check=True, timeout=90)
        subprocess.run([
            "gcc", "-O2", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-Wno-format-truncation",
            "-Wno-unused-function", "-Wno-unused-variable",
            "-Wno-unused-but-set-variable",
            "-DWIFI_TEST", f'-DAPP_DIR="{app}"',
            f'-DRADIO_OWNER_PATH="{app}/radio.owner"',
            '-DWIFI_BUILD_ID="test"',
            "-static", "-o", str(ctest),
            str(ROOT / "tests/test_carplay_dhcp.c")], check=True, timeout=90)
        run([str(ctest)])
        stub = tmp_path / "bin"
        stub.mkdir()
        (stub / "wpa_cli").write_text("#!/bin/sh\nexit 1\n")
        (stub / "wpa_cli").chmod(0o755)
        env = dict(os.environ)
        env["PATH"] = f"{stub}:{env.get('PATH', '')}"
        env.update({
            "WIFI1B_DHCP_TEST_LOOPBACK": "1",
            "WIFI1B_DHCP_TEST_IFACE": "lo",
            "WIFI1B_DHCP_TEST_NOAPPLY": "1",
            "WIFI1B_DHCP_TEST_PORT": "6870",
            "WIFI1B_DHCP_TEST_CLIENT_PORT": "6869",
        })
        test_client_id(binary, env)
        test_renew_rebind(ctest, env)
        if isolated:
            test_client_apply(binary, ctest, env)
            test_route_policy(ctest)
            test_nak_destination_netns(binary, env)
            test_modem_safe(ctest)
            test_route_audit_no_flap(ctest, app)
            test_raw_client(ctest, env, tmp_path)
        else:
            print("SKIP apply/readback/route/raw checks: requires the network namespace")

        server_env = dict(env)
        server_env["WIFI1B_DHCP_TEST_PORT"] = "6871"
        server_env["WIFI1B_DHCP_TEST_CLIENT_PORT"] = "6872"
        test_server_leases(binary, app, server_env)
    return 0


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "--inner":
        return inner(True)
    probe = subprocess.run(["unshare", "-rn", "true"], capture_output=True)
    if probe.returncode == 0:
        return subprocess.run(
            ["unshare", "-rn", sys.executable, str(Path(__file__).resolve()),
             "--inner"], check=False).returncode
    print("SKIP namespace isolation unavailable; running loopback-only sockets")
    return inner(False)


if __name__ == "__main__":
    raise SystemExit(main())
