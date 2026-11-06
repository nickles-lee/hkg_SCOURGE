#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Host-side bench test for battery_daemon on vcan0.

Covers the I-CAN reads (0x2FC temperature/SoC, 0x326 cable), the micom
commands (0x0C7 preheat, 0x4ED dash, 0x4A2 charge door), the charge-door
park override, optimistic feedback, the session-bus Home/Star monitor,
and the bus sniffer.
"""
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

IFACE = "vcan0"
PORT = 18088
DAEMON = os.path.join(os.path.dirname(__file__), "..", "src", "battery_daemon")


def can_socket():
    s = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    s.bind((IFACE,))
    s.settimeout(0.2)
    return s


def send(s, can_id, data):
    s.send(struct.pack("=IB3x8s", can_id, len(data), bytes(data)))


def http(path, method="GET"):
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}{path}", method=method)
    with urllib.request.urlopen(req, timeout=2) as r:
        return json.loads(r.read().decode())


def wait_state(want, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        st = http("/status")
        if st["state"] == want:
            return st
        time.sleep(0.05)
    raise AssertionError(f"state {want!r} not reached; last={st}")


def wait_door(want, timeout=6.0):
    end = time.time() + timeout
    while time.time() < end:
        st = http("/status")
        if st["door_message"] == want:
            return st
        time.sleep(0.05)
    raise AssertionError(f"door {want!r} not reached; last={st}")


def read_log(path):
    try:
        with open(path) as f:
            return f.read()
    except FileNotFoundError:
        return ""


def expect_409(path, reason):
    try:
        http(path, "POST")
        raise AssertionError(f"{path} should be blocked ({reason})")
    except urllib.error.HTTPError as e:
        body = json.loads(e.read().decode())
        assert body["reason"] == reason, body
        return body


def wait_home_field(field, want, timeout=5.0):
    end = time.time() + timeout
    st = None
    while time.time() < end:
        st = http("/status")
        if st.get(field) == want:
            return st
        time.sleep(0.05)
    raise AssertionError(f"{field} {want!r} not reached; last={st}")


GEAR_PARK = [0, 0, 0, 0, 0x10, 0, 0, 0]
GEAR_DRIVE = [0, 0, 0, 0, 0x05, 0, 0, 0]
CABLE_NONE = [0xE9, 0x03, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00]
CABLE_PLUGGED = [0xE9, 0x03, 0x33, 0x04, 0x10, 0x05, 0x00, 0x00]
CABLE_CHARGING = [0xE9, 0x03, 0x33, 0x14, 0x01, 0x00, 0x72, 0x01]
DOOR_CLOSED = [0xFF, 0x00, 0xD0, 0x00, 0x54, 0, 0, 0]
DOOR_OPEN = [0xFF, 0x00, 0x20, 0x00, 0x52, 0, 0, 0]


def main():
    subprocess.run(["ip", "link", "show", IFACE], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    s = can_socket()

    stub_dir = tempfile.mkdtemp(prefix="battery_stub_")
    stub = os.path.join(stub_dir, "dbus-send")
    dbus_log = os.path.join(stub_dir, "dbus.log")
    dbus_fail = os.path.join(stub_dir, "fail")
    door_fail = os.path.join(stub_dir, "door_fail")
    with open(stub, "w") as f:
        f.write("#!/bin/sh\n"
                "printf '%s\\n' \"$*\" >> \"$DBUS_LOG\"\n"
                "case \"$*\" in\n"
                "*0x04,0xa2*)\n"
                "  # Charge-door frames fail only when the regression test\n"
                "  # asks for it, so the dash/burst children keep succeeding.\n"
                "  if [ -f \"$DBUS_DOOR_FAIL\" ]; then exit 1; fi\n"
                "  ;;\n"
                "esac\n"
                "if [ -f \"$DBUS_FAIL\" ]; then exit 1; fi\n"
                "exit 0\n")
    os.chmod(stub, 0o755)
    env = dict(os.environ, DBUS_LOG=dbus_log, DBUS_FAIL=dbus_fail,
               DBUS_DOOR_FAIL=door_fail)

    # Private session bus standing in for the app-manager bus.  Dbuslaunch.sh
    # writes its address to /tmp/session_appmgr on the head unit; here the
    # daemon reads the same kind of file for its Home/Star monitor.
    bus_dir = tempfile.mkdtemp(prefix="battery_bus_")
    bus_sock = os.path.join(bus_dir, "bus")
    bus_file = os.path.join(bus_dir, "session_appmgr")
    bus = subprocess.Popen(
        ["dbus-daemon", "--session", f"--address=unix:path={bus_sock}",
         "--print-address"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    bus_addr = bus.stdout.readline().strip()
    assert bus_addr.startswith("unix:"), bus_addr
    with open(bus_file, "w") as f:
        f.write(bus_addr + "\n")
    dbus_send_bin = shutil.which("dbus-send") or "/usr/bin/dbus-send"

    daemon = subprocess.Popen(
        [DAEMON, "--iface", IFACE, "--port", str(PORT),
         "--status-file", "/tmp/battery_status.json",
         "--dbus-send", stub,
         "--session-bus-file", bus_file],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    try:
        for _ in range(50):
            try:
                http("/health")
                break
            except Exception:
                time.sleep(0.05)
        else:
            raise AssertionError("daemon did not start")

        # ---- no 0x2AD on this bus: start/stop still send the bursts ----
        send(s, 0x2FC, [0x0A, 0, 7, 8, 0, 0, 0x90, 0x9D])
        send(s, 0x326, CABLE_NONE)
        send(s, 0x036, GEAR_PARK)
        send(s, 0x3AA, DOOR_CLOSED)
        time.sleep(0.1)
        before = read_log(dbus_log)
        r = http("/start", "POST")
        assert r["ok"] is True, r
        time.sleep(0.9)
        log = read_log(dbus_log)
        new = [l for l in log[len(before):].splitlines() if "0x00,0xc7,0x08" in l]
        assert len([l for l in new if "0x40,0x03" in l]) >= 3, new
        st = http("/status")
        # WiCAN: without status frames nothing confirms the start, so the
        # daemon keeps "starting" until the started timeout assumes success.
        assert st["state"] == "starting", st
        assert st["ver"], st
        log = read_log(dbus_log)
        new = [l for l in log[len(before):].splitlines() if "0x04,0xe8,0x08" in l]
        assert new, "no cluster display frames during start"
        print("no-0x2AD start burst + cluster display ok")
        before = read_log(dbus_log)
        r = http("/stop", "POST")
        assert r["ok"] is True, r
        time.sleep(0.9)
        log = read_log(dbus_log)
        new = [l for l in log[len(before):].splitlines() if "0x00,0xc7,0x08" in l]
        assert len([l for l in new if "0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00" in l]) >= 3, new
        st = http("/status")
        assert st["state"] == "idle" and st["message"] == "stopped", st
        print("no-0x2AD start/stop bursts ok")

        # ---- baseline I-CAN frames -----------------------------------
        send(s, 0x038, [0x04, 0, 0, 0, 0xF0, 0x39, 0x96, 0xD4])
        # 0x2FC: bytes 2/3 are min/max temperature (7/8 C), byte7 SoC 0x9D.
        send(s, 0x2FC, [0x0A, 0, 7, 8, 0, 0, 0x90, 0x9D])
        send(s, 0x2AE, [0x01, 0x00, 0xFF, 0x3F, 0x01, 0x01, 0x00, 0x00])
        send(s, 0x2AD, [0, 0x01, 0, 0x40, 0xFF, 0xFF, 0xFF, 0x0F])
        time.sleep(0.2)

        st = http("/status")
        assert st["batt_min_c"] == 7, st
        assert st["temp_source"] == "0x2FC", st
        assert st["batt_raw"] == "0A 00 07 08 00 00 90 9D", st
        assert st["soc_pct"] == 78.5, st
        assert st["soc_valid"] is True, st
        print("baseline ok: 0x2FC min 7 C, SoC", st["soc_pct"])

        # 0x152 must not override the fresh 0x2FC minimum.
        send(s, 0x152, [10, 12, 0, 0, 0, 0, 0, 0])
        time.sleep(0.1)
        st = http("/status")
        assert st["batt_min_c"] == 7, st
        assert st["temp_source"] == "0x2FC", st
        print("0x152 fallback priority ok")

        # Min temperature at/above 21 C blocks preheat (WiCAN cutoff).
        send(s, 0x2FC, [0x0A, 0, 21, 0, 0, 0, 0x90, 0x9D])
        time.sleep(0.1)
        expect_409("/start", "temp_high")
        send(s, 0x2FC, [0x0A, 0, 25, 0, 0, 0, 0x90, 0x9D])
        time.sleep(0.1)
        expect_409("/start", "temp_high")
        send(s, 0x2FC, [0x0A, 0, 7, 8, 0, 0, 0x90, 0x9D])
        time.sleep(0.1)
        r = http("/start", "POST")
        assert r["ok"] is True, r
        http("/stop", "POST")
        send(s, 0x2AD, [0, 0x01, 0, 0x40, 0xFF, 0xFF, 0xFF, 0x0F])
        wait_state("idle")
        print("min-temperature 21 C block ok")

        # ---- start via micom 0x0C7 -----------------------------------
        before = read_log(dbus_log)
        r = http("/start", "POST")
        assert r["ok"] is True, r
        time.sleep(0.6)
        log = read_log(dbus_log)
        new = [l for l in log[len(before):].splitlines() if "0x00,0xc7,0x08" in l]
        assert len([l for l in new if "0x40,0x03" in l]) >= 3, new
        assert len([l for l in new if "0xe0,0x07" in l]) >= 3, new
        print("start micom burst ok:", new[0])

        send(s, 0x2AD, [0, 0x05, 0, 0x40, 0xFF, 0xFF, 0xFF, 0x0F])
        wait_state("preparing")
        send(s, 0x2AD, [0, 0x15, 0, 0x40, 0xFF, 0xFF, 0xFF, 0x0F])
        wait_state("active")
        print("state machine ok: preparing -> active")

        # ---- cluster display via micom 0x4E8 + 0x4CC -----------------
        # Manual toggle while ACTIVE: unit YD, flag BLUE_1, countdown 70 s.
        before = read_log(dbus_log)
        r = http("/dash/on", "POST")
        assert r["ok"] is True, r
        time.sleep(0.6)
        log = read_log(dbus_log)
        new = [l for l in log[len(before):].splitlines() if "0x04,0xe8,0x08" in l]
        # A start-display frame may still be in flight from the PREPARING
        # phase just before the toggle; the manual line (countdown 70 s,
        # unit YD, flag BLUE_1) must appear somewhere in the window.
        assert any("0x04,0x00,0x00,0x00,0x46,0x00,0x01,0x00" in l for l in new), new
        new = [l for l in log[len(before):].splitlines() if "0x04,0xcc,0x08" in l]
        assert any("0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00" in l for l in new), new
        print("cluster display ok:", read_log(dbus_log).splitlines()[-2:])
        http("/dash/off", "POST")

        # ---- stop via micom 0x0C7 ------------------------------------
        before = read_log(dbus_log)
        r = http("/stop", "POST")
        assert r["ok"] is True, r
        time.sleep(0.6)
        log = read_log(dbus_log)
        new = [l for l in log[len(before):].splitlines() if "0x00,0xc7,0x08" in l]
        assert len([l for l in new if "0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00" in l]) >= 3, new
        send(s, 0x2AD, [0, 0x01, 0, 0x40, 0xFF, 0xFF, 0xFF, 0x0F])
        wait_state("idle")
        print("stop micom burst ok")

        # ---- BMS 0x00 means "off": stop must not grind ----------------
        # Regression: raw 0x00 matched no transition, so ST_STOPPING ran
        # four 10 s stop retries (~50 s of "Stopping") before giving up.
        send(s, 0x2AD, [0, 0x00, 0, 0x40, 0xFF, 0xFF, 0xFF, 0x0F])
        time.sleep(0.1)
        before = read_log(dbus_log)
        r = http("/stop", "POST")
        assert r["ok"] is True, r
        time.sleep(0.9)
        st = http("/status")
        assert st["state"] == "idle", st
        log = read_log(dbus_log)
        new = [l for l in log[len(before):].splitlines() if "0x00,0xc7,0x08" in l]
        assert len([l for l in new if "0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00" in l]) == 3, new
        print("stop with BMS 0x00 (off) exits promptly ok")

        # ---- spoofed nav frame is independent of preheat state ---------
        # The 0x4ED nav-precondition frame must flow from /dash/on while
        # idle, so it can be tested (and present) before a preheat start.
        before = read_log(dbus_log)
        r = http("/dash/on", "POST")
        assert r["ok"] is True and r["dash"] is True, r
        time.sleep(1.2)
        log = read_log(dbus_log)
        new = [l for l in log[len(before):].splitlines() if "0x04,0xe8,0x08" in l]
        assert len(new) >= 2, new
        http("/dash/off", "POST")
        time.sleep(0.3)
        n2 = len([l for l in read_log(dbus_log)[len(log):].splitlines()
                  if "0x04,0xe8,0x08" in l])
        assert n2 == 0, "display frames kept flowing after dash/off"
        print("cluster display sends without preheat ok")

        # ---- advisory checks must not block --------------------------
        send(s, 0x2FC, [0x0A, 0, 7, 8, 0, 0, 0x90, 0x18])  # 12%
        send(s, 0x152, [25, 25, 0, 0, 0, 0, 0, 0])
        send(s, 0x2AE, [0x01, 0x00, 0xFF, 0x3F, 0x06, 0x01, 0x00, 0x00])
        time.sleep(0.1)
        r = http("/start", "POST")
        assert r["ok"] is True, r
        http("/stop", "POST")
        send(s, 0x2AD, [0, 0x01, 0, 0x40, 0xFF, 0xFF, 0xFF, 0x0F])
        wait_state("idle")
        send(s, 0x2FC, [0x0A, 0, 7, 8, 0, 0, 0x90, 0x9D])
        send(s, 0x2AE, [0x01, 0x00, 0xFF, 0x3F, 0x01, 0x01, 0x00, 0x00])
        print("advisory checks do not block ok")

        # ---- charge door --------------------------------------------
        send(s, 0x036, GEAR_PARK)
        send(s, 0x326, CABLE_NONE)
        send(s, 0x3AA, DOOR_CLOSED)
        time.sleep(0.3)

        st = http("/status")
        assert st["park"] is True and st["gear"] == 0, st
        assert st["cable"] is False and st["cable_known"] is True, st
        assert st["door_state"] == 2 and st["door_known"] is True, st
        print("door baseline ok: Park, cable absent, closed")

        before = read_log(dbus_log)
        r = http("/chargedoor/open?park=1", "POST")
        assert r["ok"] is True and r["door"] == "opening", r
        time.sleep(0.3)
        log = read_log(dbus_log)
        assert "0x04,0xa2,0x08,0xff,0xff,0xff,0xdf" in log[len(before):], log[-400:]
        print("door open micom ok")

        send(s, 0x3AA, DOOR_OPEN)
        st = wait_door("open")
        assert st["door_state"] == 1 and st["door_source"] == "can", st
        print("door open CAN feedback ok")

        before = read_log(dbus_log)
        r = http("/chargedoor/open?park=1", "POST")
        assert r["ok"] is True and r["door"] == "open", r
        time.sleep(0.2)
        before_doors = [l for l in before.splitlines() if "0x04,0xa2,0x08" in l]
        after_doors = [l for l in read_log(dbus_log).splitlines() if "0x04,0xa2,0x08" in l]
        assert after_doors == before_doors, "duplicate open command was sent"
        print("door already-open no-op ok")

        time.sleep(0.6)
        before = read_log(dbus_log)
        r = http("/chargedoor/close?park=1", "POST")
        assert r["ok"] is True and r["door"] == "closing", r
        time.sleep(0.3)
        assert "0x04,0xa2,0x08,0xff,0xff,0xff,0xef" in read_log(dbus_log)[len(before):]
        send(s, 0x3AA, DOOR_CLOSED)
        st = wait_door("closed")
        assert st["door_state"] == 2 and st["door_source"] == "can", st
        print("door close CAN feedback ok")

        # Park veto: QML says park, but the bus says Drive.
        send(s, 0x036, GEAR_DRIVE)
        time.sleep(0.2)
        expect_409("/chargedoor/open?park=1", "not_park")
        print("door park veto ok")

        # Cable gate.
        send(s, 0x036, GEAR_PARK)
        send(s, 0x326, CABLE_PLUGGED)
        time.sleep(0.2)
        expect_409("/chargedoor/open?park=1", "cable_plugged")
        send(s, 0x326, CABLE_CHARGING)
        time.sleep(0.2)
        expect_409("/chargedoor/open?park=1", "cable_plugged")
        print("door cable/charging gate ok")

        # Park unknown without the QML override.
        time.sleep(2.5)
        expect_409("/chargedoor/open", "park_unknown")
        print("door park-unknown gate ok")

        # Park unknown with the QML override and no cable -> allowed.
        send(s, 0x326, CABLE_NONE)
        time.sleep(0.1)
        r = http("/chargedoor/open?park=1", "POST")
        assert r["ok"] is True, r
        st = wait_door("open", timeout=4.0)
        assert st["door_source"] == "command", st
        print("door park override + optimistic feedback ok")

        # dbus-send failure -> retry once -> send_failed.
        time.sleep(0.6)
        send(s, 0x036, GEAR_PARK)
        send(s, 0x326, CABLE_NONE)
        time.sleep(0.1)
        open(dbus_fail, "w").close()
        r = http("/chargedoor/close?park=1", "POST")
        assert r["ok"] is True, r
        st = wait_door("send_failed", timeout=4.0)
        os.unlink(dbus_fail)
        print("door send-failed path ok")

        # ---- unknown cable state no longer blocks the door ------------
        # Regression: 0x326 is invisible on the car, so "cable_unknown"
        # permanently disabled the charge door.  An observed cable still
        # vetoes; an unknown one is advisory only.
        send(s, 0x036, GEAR_PARK)
        time.sleep(2.5)                    # age out the cable/door frames
        r = http("/chargedoor/close?park=1", "POST")
        assert r["ok"] is True and r["door"] == "closing", r
        st = wait_door("closed", timeout=4.0)
        assert st["door_source"] == "command", st
        r = http("/chargedoor/open?park=1", "POST")
        assert r["ok"] is True and r["door"] == "opening", r
        st = wait_door("open", timeout=4.0)
        assert st["door_source"] == "command", st
        print("door with unknown cable allowed ok")

        # ---- door send failure must not be masked by dash frames ------
        # Regression: the door feedback used to read a child-exit status
        # shared with the dash/burst sends.  With the dash overlay active
        # (a fork every 500 ms exiting 0), a failing door command was
        # masked as a success within 1.2 s and the door was reported
        # "closed (commanded)".  The door now tracks its own child.
        send(s, 0x036, GEAR_PARK)
        send(s, 0x326, CABLE_NONE)
        time.sleep(0.1)
        r = http("/dash/on", "POST")
        assert r["ok"] is True, r
        time.sleep(0.8)                    # dash children now exiting 0
        time.sleep(0.6)                    # past the door cooldown
        open(door_fail, "w").close()
        r = http("/chargedoor/close?park=1", "POST")
        assert r["ok"] is True, r
        st = wait_door("send_failed", timeout=6.0)
        assert st["door_state_name"] != "closed" or st["door_source"] != "command", st
        os.unlink(door_fail)
        http("/dash/off", "POST")
        print("door send-fail not masked by dash frames ok")

        # ---- session-bus Home/Star monitor ---------------------------
        st = wait_home_field("home_monitor", True)
        assert st["home_seq"] == 0 and st["home_source"] == "none", st
        assert st["last_app_key"] == -1, st

        # Star/custom key: signal custom_key_value(int) on
        # com.lge.avn.appmanager.info.
        subprocess.run(
            [dbus_send_bin, f"--address={bus_addr}", "--type=signal",
             "/test", "com.lge.avn.appmanager.info.custom_key_value",
             "int32:7"], check=True)
        st = wait_home_field("home_seq", 1)
        assert st["home_source"] == "star", st
        assert st["last_app_key"] == 7, st
        print("star custom_key_value ok: seq 1, key 7")

        # Home request: method call NotifyRequestHomeScreen on
        # com.lge.hkmc.avn.appfunc.appInterface.  dbus-send reports
        # ServiceUnknown because nothing owns the destination name; the
        # monitor still observes the method call.
        subprocess.run(
            [dbus_send_bin, f"--address={bus_addr}", "--type=method_call",
             "--dest=com.test", "/test",
             "com.lge.hkmc.avn.appfunc.appInterface.NotifyRequestHomeScreen",
             "array:byte:0x01"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        st = wait_home_field("home_seq", 2)
        assert st["home_source"] == "home", st
        print("home NotifyRequestHomeScreen ok: seq 2")

        # ---- WiCAN sequence on a fast-timing daemon --------------------
        # Second instance with short retry/timeout pacing.  It opens its own
        # socket, so it only sees frames sent from now on.
        PORT2 = 18189
        def http2(path, method="GET"):
            req = urllib.request.Request(
                f"http://127.0.0.1:{PORT2}{path}", method=method)
            with urllib.request.urlopen(req, timeout=2) as r:
                return json.loads(r.read().decode())

        def wait_state2(want, timeout=6.0):
            end = time.time() + timeout
            st = None
            while time.time() < end:
                st = http2("/status")
                if st["state"] == want:
                    return st
                time.sleep(0.05)
            raise AssertionError(f"daemon2 state {want!r} not reached; last={st}")

        proc2 = subprocess.Popen(
            [DAEMON, "--iface", IFACE, "--port", str(PORT2),
             "--status-file", "/tmp/battery_status2.json",
             "--dbus-send", stub, "--no-session-monitor",
             "--retry-ms", "300", "--started-timeout-ms", "1500"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        try:
            for _ in range(40):
                try:
                    http2("/health"); break
                except Exception:
                    time.sleep(0.05)
            else:
                raise AssertionError("fast daemon did not start")

            # No status frames: one burst, then assume ACTIVE after the
            # started timeout (WiCAN wait_starting_tick).
            before = read_log(dbus_log)
            r = http2("/start", "POST")
            assert r["ok"] is True, r
            # The burst is paced by the 50 ms poll loop (6 frames) and each
            # frame's stub child needs time to fork/exec/append: wait for the
            # burst lines instead of reading the log at a fixed offset.
            deadline = time.time() + 4.0
            burst_lines = []
            while time.time() < deadline:
                burst_lines = [l for l in read_log(dbus_log)[len(before):].splitlines()
                               if "0x00,0xc7,0x08" in l]
                if (len([l for l in burst_lines if "0x40,0x03" in l]) >= 3
                        and len([l for l in burst_lines if "0xe0,0x07" in l]) >= 3):
                    break
                time.sleep(0.1)
            assert len([l for l in burst_lines if "0x40,0x03" in l]) == 3, burst_lines
            assert len([l for l in burst_lines if "0xe0,0x07" in l]) == 3, burst_lines
            # start display: no status -> unit M, flag BLUE_4, countdown 1 s
            deadline = time.time() + 3.0
            disp = []
            while time.time() < deadline:
                disp = [l for l in read_log(dbus_log)[len(before):].splitlines()
                        if "0x04,0xe8,0x08" in l]
                if disp:
                    break
                time.sleep(0.1)
            assert disp and "0x00,0x00,0x00,0x00,0x01,0x00,0x04,0x00" in disp[0], disp
            st = wait_state2("active", timeout=4.0)
            assert st["message"] == "active", st
            print("no-status start assumes active after timeout ok")

            # Status appears (BMS off): start retries 4x, then the WiCAN
            # cleanup stop burst (one shot, no stop retries) -> "stopped".
            send(s, 0x2AD, [0, 0x00, 0, 0x40, 0xFF, 0xFF, 0xFF, 0x0F])
            time.sleep(0.1)
            http2("/stop", "POST")
            wait_state2("idle", timeout=4.0)
            before = read_log(dbus_log)
            r = http2("/start", "POST")
            assert r["ok"] is True, r
            st = wait_state2("idle", timeout=8.0)
            assert st["message"] == "stopped", st
            deadline = time.time() + 4.0
            new = []
            while time.time() < deadline:
                new = [l for l in read_log(dbus_log)[len(before):].splitlines()
                       if "0x00,0xc7,0x08" in l]
                # 5 start bursts (initial + 4 retries) then one cleanup stop burst
                if (len([l for l in new if "0x40,0x03" in l]) >= 15
                        and len([l for l in new
                                 if "0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00" in l]) >= 3):
                    break
                time.sleep(0.1)
            assert len([l for l in new if "0x40,0x03" in l]) == 15, new
            assert len([l for l in new if "0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00" in l]) == 3, new
            print("start retries exhausted -> cleanup stop burst ok")
        finally:
            proc2.terminate()
            try:
                proc2.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc2.kill()

        # ---- bus sniffer --------------------------------------------
        proc = subprocess.Popen(
            [DAEMON, "--iface", IFACE, "--sniff", "1"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        for _ in range(12):
            send(s, 0x2FC, [0x0A, 0, 7, 8, 0, 0, 0x90, 0x9D])
            send(s, 0x326, CABLE_NONE)
            time.sleep(0.05)
        out, err = proc.communicate(timeout=5)
        assert proc.returncode == 0, (proc.returncode, err)
        assert "0x2FC" in out and "0x326" in out, out
        print("sniff ok:", out.splitlines()[0])

        print("ALL TESTS PASSED")
        return 0
    finally:
        daemon.terminate()
        try:
            daemon.wait(timeout=3)
        except subprocess.TimeoutExpired:
            daemon.kill()
        out, err = daemon.communicate()
        if err.strip():
            print("daemon stderr:", err.strip(), file=sys.stderr)
        bus.terminate()
        bus.wait(timeout=3)
        shutil.rmtree(stub_dir, ignore_errors=True)
        shutil.rmtree(bus_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
