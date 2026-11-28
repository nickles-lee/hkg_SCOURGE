#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Offline smoke test: feed hudnavd a synthetic AA maneuver stream over its unix
socket and print the emitted frames. No car, no D-Bus. PROTOTYPE."""
import socket, subprocess, sys, time, os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SOCK = "/tmp/hudnav-smoke.sock"
BIN = os.path.join(ROOT, "out", "hudnavd")

proc = subprocess.Popen([BIN, "--socket", SOCK, "--can"],
                        stderr=subprocess.PIPE, text=True)
time.sleep(0.3)
try:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    def send(line):
        s.sendall((line + "\n").encode())
    send("OWNER src=aa")
    send("STATE s=1")
    send("TURN src=aa man=3 raw=4 side=2")      # HN_MAN_TURN_RIGHT
    # approach: 1 Hz samples from 150 m down, heartbeats throughout
    for i, m in enumerate([150, 140, 130, 120, 110, 100, 80, 60, 40, 20, 10, 5]):
        send("HB")
        send(f"DIST m={m} disp={m} unit=0")
        time.sleep(0.25)
    send("STATE s=0")                            # guidance end -> clear
    send("HB")
    time.sleep(0.4)
    s.close()
finally:
    proc.terminate()
    err = proc.stderr.read()
    proc.wait()
print(err)
