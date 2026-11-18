#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Fixture-only smoke test for the experimental broker helper front end."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
BROKER = ROOT / "out/carplay-broker/carplay-broker"
subprocess.run(["make", "-C", str(SRC), "carplay-broker"],
               check=True, timeout=120, stdout=subprocess.DEVNULL)

# Placeholder identity only. Never put an owner's vehicle MAC/device ID in a
# tracked fixture; those belong in the gitignored owner config.
IDENTITY = """\
service_name=SCOURGE-iAP
name=Ioniq 5
model=IONIQ5
manufacturer=Hyundai
serial=TEST-SERIAL
firmware=TEST-FW
hardware=TEST-HW
eap_name=TEST-EAP
language=en
languages=en
transport_name=SCOURGE-TEST
bt_mac=AA:BB:CC:DD:EE:FF
device_id=AA:BB:CC:DD:EE:01
ap_ssid=IONIQ5-CarPlay
ap_psk=test-passphrase
ap_channel=6
ap_security=WPA2
link_local=fe80::1
airplay_port=7000
consent=1
usb_active=0
"""

with tempfile.TemporaryDirectory(prefix="broker-main-", dir="/tmp/opencode") as d:
    tmp = Path(d)

    # Missing config is a clean fail-closed exit.
    r = subprocess.run([str(BROKER), "--config", str(tmp / "missing"),
                        "--broker-status", str(tmp / "s0")],
                       capture_output=True, text=True, timeout=20)
    assert r.returncode == 2, r

    # Incomplete identity is reported without opening the RX socket or bus.
    cfg = tmp / "incomplete.conf"
    cfg.write_text("service_name=SCOURGE-iAP\nname=only\n")
    status = tmp / "s1"
    r = subprocess.run([str(BROKER), "--config", str(cfg),
                        "--broker-status", str(status)],
                       capture_output=True, text=True, timeout=20)
    assert r.returncode == 2, r
    text = status.read_text()
    assert '"state":"identity_incomplete"' in text, text
    assert '"ok":false' in text, text

    # An unknown security value must not silently become WPA2.
    cfg2 = tmp / "badsec.conf"
    cfg2.write_text(IDENTITY.replace("ap_security=WPA2", "ap_security=BOGUS"))
    status2 = tmp / "s2"
    r = subprocess.run([str(BROKER), "--config", str(cfg2),
                        "--broker-status", str(status2)],
                       capture_output=True, text=True, timeout=20)
    assert r.returncode == 2, r
    text = status2.read_text()
    assert '"state":"identity_incomplete"' in text, text
    assert '"ok":false' in text, text
    assert "unsupported AP security" in text, text

    # A complete config reaches the root-only RX directory gate and fails
    # closed without touching a bus or the daemon status.  As root the helper
    # would create /run/wifi1b; skip that fixture rather than mutating the host.
    if os.geteuid() == 0:
        print("SKIP: root fixture would create /run/wifi1b; RX-gate check "
              "requires an unprivileged run")
    else:
        cfg3 = tmp / "complete.conf"
        cfg3.write_text(IDENTITY)
        status3 = tmp / "s3"
        r = subprocess.run([str(BROKER), "--config", str(cfg3),
                            "--broker-status", str(status3),
                            "--status", str(tmp / "no-daemon-status")],
                           capture_output=True, text=True, timeout=20)
        assert r.returncode == 2, r
        text = status3.read_text()
        assert '"state":"fatal"' in text, text
        assert '"ok":false' in text, text
        assert "cannot create rx" in text, text

print("PASS: broker helper config/identity/security gates fail closed")
