#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""C2 Wifi1b AP-network tests.

The fixture build runs the daemon's own query path with every command
intercepted by the WIFI_TEST hook. The optional integration half re-executes
inside `unshare -rn` and only ever configures the disposable network
namespace; it never changes host interfaces, sysctls or firewall state.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
CFLAGS = [
    "-O2", "-Wall", "-Wextra", "-Werror",
    "-Wno-unused-parameter", "-Wno-format-truncation",
    "-Wno-unused-function", "-Wno-unused-variable",
    "-Wno-unused-but-set-variable",
]


def run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kwargs)


def parse_linklocal(text: str, ifname: str) -> tuple[str, int] | None:
    for line in text.splitlines():
        if " inet6 " not in line or " scope link" not in line:
            continue
        if " tentative" in line or "dadfailed" in line:
            continue
        try:
            ifindex = int(line.split(":", 1)[0])
        except ValueError:
            continue
        if not line.split(":", 1)[1].lstrip().startswith(ifname):
            continue
        token = line.split("inet6 ", 1)[1].split()[0]
        address = token.split("/", 1)[0]
        try:
            packed = socket.inet_pton(socket.AF_INET6, address)
        except OSError:
            continue
        if packed[0] != 0xFE or (packed[1] & 0xC0) != 0x80:
            continue
        return address, ifindex
    return None


def linklocal(ifname: str) -> tuple[str, int] | None:
    """Usable link-local address from the current (unshared) netns."""
    try:
        out = run(["ip", "-6", "-o", "addr", "show", "dev", ifname]).stdout
    except subprocess.CalledProcessError:
        return None
    return parse_linklocal(out, ifname)


def wait_linklocal(ifname: str, timeout: float = 6.0) -> tuple[str, int]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        found = linklocal(ifname)
        if found is not None:
            return found
        time.sleep(0.1)
    raise AssertionError(f"no usable IPv6 link-local on {ifname}")


def nsenter(pid: int, command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return run(["nsenter", "-t", str(pid), "-n", "--", *command], **kwargs)


def wait_peer_linklocal(pid: int, timeout: float = 6.0) -> tuple[str, int]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        out = nsenter(pid, ["ip", "-6", "-o", "addr", "show", "dev", "cpeer0"]).stdout
        found = parse_linklocal(out, "cpeer0")
        if found is not None:
            return found
        time.sleep(0.1)
    raise AssertionError("no usable IPv6 link-local on cpeer0")


PEER_SEND = r"""
import socket, sys
peer, wlan, port, pif = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
if sys.argv[5] == "udp":
    c = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    c.bind((peer, 0, 0, pif))
    c.sendto(b"carplay-c2", (wlan, port, 0, pif))
elif sys.argv[5] == "tcp":
    c = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
    c.bind((peer, 0, 0, pif))
    c.settimeout(3.0)
    c.connect((wlan, port, 0, pif))
    c.sendall(b"carplay-c2")
    c.close()
"""


def inner(binary: Path, app: Path) -> int:
    if os.geteuid() != 0:
        print("SKIP namespace integration: not root inside the user namespace")
        return 0
    for tool in ("ip", "ip6tables", "nsenter", "unshare", "sleep"):
        if shutil.which(tool) is None:
            print(f"SKIP namespace integration: {tool} unavailable")
            return 0
    try:
        run(["ip", "link", "add", "wlan0", "type", "veth", "peer", "name", "cpeer0"])
    except subprocess.CalledProcessError as error:
        print(f"SKIP namespace integration: no CAP_NET_ADMIN ({error.stderr.strip()})")
        return 0

    peer = subprocess.Popen(
        ["unshare", "-n", "sh", "-c", "ip link set lo up; exec sleep 300"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        own_netns = os.readlink("/proc/self/ns/net")
        deadline = time.monotonic() + 5
        while True:
            if peer.poll() is not None:
                raise AssertionError("peer network namespace exited")
            try:
                if os.readlink(f"/proc/{peer.pid}/ns/net") != own_netns:
                    break
            except OSError:
                pass
            if time.monotonic() >= deadline:
                print("SKIP namespace integration: peer netns did not appear")
                return 0
            time.sleep(0.05)
        try:
            run(["ip", "link", "set", "cpeer0", "netns", str(peer.pid)])
        except subprocess.CalledProcessError as error:
            print("SKIP namespace integration: nested network namespace "
                  f"unavailable ({error.stderr.strip()})")
            return 0
        run(["ip", "link", "set", "lo", "up"])
        run(["ip", "link", "set", "wlan0", "up"])
        nsenter(peer.pid, ["ip", "link", "set", "lo", "up"])
        deadline = time.monotonic() + 5
        while True:
            try:
                nsenter(peer.pid, ["ip", "link", "set", "cpeer0", "up"])
                break
            except subprocess.CalledProcessError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        nsenter(peer.pid, ["ip", "-4", "addr", "add", "10.42.0.2/24",
                           "dev", "cpeer0"])
        run(["ip", "-4", "addr", "add", "10.42.0.1/24", "dev", "wlan0"])
        wlan6, _wif = wait_linklocal("wlan0")
        peer6, pif = wait_peer_linklocal(peer.pid)

        # IPv4 cleanup must not remove the IPv6 link-local address.
        run([str(binary), "--flush"])
        v4 = run(["ip", "-o", "-4", "addr", "show", "dev", "wlan0"]).stdout
        assert "10.42.0.1" not in v4, v4
        assert linklocal("wlan0") is not None, "IPv4 flush removed the IPv6 link-local"

        # Install only our owned chains, then prove the packet path in this netns.
        run([str(binary), "--fw-open-ap"])
        run([str(binary), "--fw-open"])
        ap6 = run(["ip6tables", "-S", "WIFI1B_APP_AP6"]).stdout
        cp6 = run(["ip6tables", "-S", "WIFI1B_APP_CP"]).stdout
        assert "ipv6-icmp" in ap6, ap6
        assert "--dport 5020" in cp6, cp6
        assert "--dport 6030" in cp6, cp6
        assert "--dport 547" in cp6, cp6
        run(["ip6tables", "-P", "INPUT", "DROP"])
        subprocess.run(["ip", "-6", "neigh", "flush", "dev", "wlan0"],
                       check=False, capture_output=True)
        nsenter(peer.pid, ["ip", "-6", "neigh", "flush", "dev", "cpeer0"])

        def udp(port: int, expect: bool) -> None:
            srv = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
            srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            srv.bind((wlan6, port, 0, _ifindex("wlan0")))
            srv.settimeout(2.0 if expect else 0.7)
            try:
                nsenter(peer.pid, [sys.executable, "-c", PEER_SEND,
                                   peer6, wlan6, str(port), str(pif), "udp"])
                data, _ = srv.recvfrom(64)
                assert expect, f"UDP/{port} should have been dropped"
                assert data == b"carplay-c2"
            except socket.timeout:
                assert not expect, f"UDP/{port} should have been accepted"
            finally:
                srv.close()

        udp(5020, True)
        udp(5999, False)

        listener = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((wlan6, 6030, 0, _ifindex("wlan0")))
        listener.listen(1)
        listener.settimeout(2.0)
        try:
            nsenter(peer.pid, [sys.executable, "-c", PEER_SEND,
                               peer6, wlan6, "6030", str(pif), "tcp"])
            accepted, _ = listener.accept()
            accepted.close()
        finally:
            listener.close()

        print("PASS: namespace IPv6 link-local retention and owned IPv6 firewall flow")
    finally:
        peer.terminate()
        try:
            peer.wait(timeout=3)
        except subprocess.TimeoutExpired:
            peer.kill()
    return 0


def _ifindex(ifname: str) -> int:
    return socket.if_nametoindex(ifname)


def main() -> int:
    if len(sys.argv) >= 4 and sys.argv[1] == "--inner":
        return inner(Path(sys.argv[2]), Path(sys.argv[3]))

    with tempfile.TemporaryDirectory(prefix="wifi1b-c2-", dir="/tmp/opencode") as tmp:
        tmp_path = Path(tmp)
        app = tmp_path / "app"
        app.mkdir()
        radio = tmp_path / "radio-owner"
        binary = tmp_path / "test_carplay_network"
        run(["gcc", *CFLAGS, "-DWIFI_TEST",
             f'-DAPP_DIR="{app}"', f'-DRADIO_OWNER_PATH="{radio}"',
             '-DWIFI_BUILD_ID="test"', "-static",
             "-o", str(binary),
             str(ROOT / "tests/test_carplay_network.c")])
        run([str(binary)])

        # Queries must not delete runtime .tmp state.
        stale = app / "status.json.tmp.12345"
        stale.write_text("stale\n")
        os.utime(stale, (time.time() - 7200, time.time() - 7200))
        status = run([str(binary), "--query-status"]).stdout
        parsed = json.loads(status)
        assert parsed["app"] == "wifi1b"
        assert parsed["ap_channel"] == 36
        assert parsed["ap_linklocal"] == "fe80::3%wlan0"
        assert parsed["ap_network_ready"] is True
        assert stale.exists(), "--status deleted .tmp state"
        run([str(binary), "--query-port"])
        assert stale.exists(), "--choose-port deleted .tmp state"
        print("PASS: query modes preserve .tmp state and expose live AP readback")

        if shutil.which("unshare") is None:
            print("SKIP namespace integration: unshare unavailable")
            return 0
        probe = subprocess.run(["unshare", "-rn", "true"],
                               capture_output=True, text=True)
        if probe.returncode != 0:
            print("SKIP namespace integration: unshare -rn unavailable "
                  f"({probe.stderr.strip() or 'unprivileged user namespaces'})")
            return 0
        return subprocess.run(
            ["unshare", "-rn", sys.executable, str(Path(__file__).resolve()),
             "--inner", str(binary), str(app)],
            check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
