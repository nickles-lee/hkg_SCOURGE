#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Static safety checks for the default-off experimental CarPlay units."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SYSTEMD = ROOT / "artifacts/systemd"
CARPLAY = ROOT / "artifacts/carplay"


def unit(name: str) -> str:
    return (SYSTEMD / name).read_text()


broker = unit("carplay-broker.service")
auth = unit("scourge-carplay-auth.service")
readme = (CARPLAY / "README.md").read_text()
example = (CARPLAY / "carplay-broker.conf.example").read_text()
payload = (ROOT / "scripts/build_payload.sh").read_text()

# The payload builder must not install or activate either experimental unit.
for marker in ("carplay-broker.service", "scourge-carplay-auth.service",
               "/rw_data/wifi1b/carplay-broker"):
    assert marker not in payload, marker

for text, name in ((broker, "broker"), (auth, "auth")):
    assert "EXPERIMENTAL" in text, name
    assert "not car-validated" in text, name
    assert "Restart=on-failure" in text, name
    assert "UMask=0077" in text, name
    assert "ConditionPathExists=" in text, name

# Broker: owner-supplied config, no hardcoded bus, no privilege or network
# command execution, and a bounded wait for the session-bus env file.
assert "ConditionPathExists=/rw_data/wifi1b/carplay-broker" in broker
assert "ConditionPathExists=/rw_data/wifi1b/carplay-broker.conf" in broker
assert ("ExecStart=/rw_data/wifi1b/carplay-broker --config "
        "/rw_data/wifi1b/carplay-broker.conf") in broker
assert "--bus" not in broker
assert "systemctl enable" not in broker
assert "ip " not in broker and "route" not in broker
assert "while [ ! -s /tmp/session_appmgr.env ]" in broker
assert "After=Dbuslaunch.service wifi1b-daemon.service" in broker
assert "Wants=wifi1b-daemon.service" not in broker
assert "RuntimeDirectory=wifi1b/carplay" in broker
assert "RuntimeDirectoryMode=0700" in broker
assert "/run/wifi1b/broker-status.json" in broker
assert "carplay/broker-status.json" not in broker

# Auth helper: real library/chip path only, no test-only expected-hash flag and
# no fabricated credentials.
assert ("ExecStart=/rw_data/wifi1b/carplay-native-auth --serve "
        "--experimental") in auth
assert "--expected-sha256" not in auth
assert "Before=carplay-broker.service" in auth
assert "ConditionPathExists=/rw_data/wifi1b/carplay-native-auth" in auth

# The operator-facing artifact must keep the status and rollback honest.
assert "not car-validated" in readme.lower()
assert "do **not** run `systemctl enable`" in readme.lower()
assert "restore the stock vbtd" in readme.lower()
assert "static flag is not a" in readme

# The example config must stay fail-closed and must not invent identity.
for key in ("service_name=", "name=", "model=", "manufacturer=", "serial=",
            "firmware=", "hardware=", "eap_name=", "language=",
            "languages=", "transport_name=", "bt_mac=", "device_id=",
            "ap_ssid=", "ap_psk=", "ap_channel=", "ap_security=",
            "link_local=", "airplay_port=", "consent=", "usb_active="):
    assert key in example, key
for key in ("name", "model", "manufacturer", "serial", "firmware", "hardware",
            "eap_name", "transport_name", "bt_mac", "device_id", "ap_ssid",
            "ap_psk", "link_local"):
    assert f"{key}=\n" in example, key
assert "consent=0" in example
assert "usb_active=1" in example
assert "ap_channel=0" in example

# The README's relative links must resolve in-tree.
for link in ("../../../../docs/wireless-carplay/VBT-SPP-HOOK-PORTING.md",
             "../../../../docs/wireless-carplay/DEFERRED-CAR-PROBES.md"):
    assert (CARPLAY / link).resolve().is_file(), link

print("PASS: experimental CarPlay units are default-off and fail closed")
