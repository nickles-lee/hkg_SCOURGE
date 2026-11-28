# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""hudnav-run.sh plumbing: dbus-monitor -> fifo -> hudnavd stdin, start/status/stop, tx gate.

A fake dbus-monitor stands in for the car's (same line format the handoff test and the
first car capture use). What this proves is the part that was broken: that hudnavd
actually receives ownership signals and projection lines, and is torn down cleanly.
"""

import os
import shutil
import socket
import stat
import subprocess
import tempfile
import time
from pathlib import Path

SCRIPT = Path("scripts/hudnav-run.sh").resolve()
DAEMON = Path("out/hudnavd-diag").resolve()

FAKE_MONITOR = """#!/bin/sh
echo "$@" > "$FAKE_MON_ARGS"
sleep 1
printf 'signal sender=:1.28 -> dest=(null destination) serial=8477 path=/Information; interface=com.lge.avn.appmanager.info; member=navi_route_guide_application\\n   int32 111\\n'
exec sleep 60
"""


def procs_mentioning(token):
    found = []
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            cmd = Path(f"/proc/{pid}/cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace")
        except OSError:
            continue
        if token in cmd and int(pid) != os.getpid():
            found.append((pid, cmd.strip()))
    return found


def wait_for(predicate, what, seconds=5.0):
    deadline = time.time() + seconds
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}")


def verify_unfed_stdin_is_silent():
    """The pitfall hudnav-run.sh exists to avoid (original runbook step 2.2, and the
    systemd unit): --appmanager-stdin with nothing feeding stdin hits EOF, the daemon
    treats that as "native nav owns guidance" and never emits, however good the input."""
    with tempfile.TemporaryDirectory(prefix="hudnav-eof-") as d:
        sock = Path(d) / "h.sock"
        log = Path(d) / "log"
        with log.open("wb") as out:
            proc = subprocess.Popen(
                ["./out/hudnavd", "--can", "--appmanager-stdin", "--socket", str(sock)],
                stdin=subprocess.DEVNULL, stderr=out)
            try:
                wait_for(sock.exists, "hudnavd socket")
                with socket.socket(socket.AF_UNIX) as c:
                    c.connect(str(sock))
                    c.sendall(b"OWNER src=aa\nSTATE s=1\nTURN src=aa man=3\nDIST m=90\nHB\n")
                    time.sleep(1.5)
            finally:
                proc.terminate()
                proc.wait(timeout=3)
        assert "MICOM" not in log.read_text(), log.read_text()
    print("unfed --appmanager-stdin stays silent (as documented in hudnav-run.sh)")


def verify_tx_killswitch():
    """tx mode against a recording fake micom-send: transmits while armed, `pause` sends
    guidance-off + clear frames and then goes quiet, `resume` re-arms, `stop` leaves
    nothing running. This is the safety control for the one phase that transmits."""
    with tempfile.TemporaryDirectory(prefix="hudnav-tx-") as d:
        d = Path(d)
        rw, run, fake = d / "rw", d / "run", d / "fakebin"
        (rw / "bin").mkdir(parents=True)
        fake.mkdir()
        shutil.copy(Path("out/hudnavd").resolve(), rw / "bin" / "hudnavd")
        sent = d / "sent.log"
        ms = rw / "bin" / "micom-send"
        ms.write_text(f'#!/bin/sh\necho "$1" >> "{sent}"\n')
        ms.chmod(0o755)
        tp_sent = d / "tp_sent.log"
        tp = rw / "bin" / "micom-send-tp"      # records each argument (one TP message) on its own line
        tp.write_text(f'#!/bin/sh\nfor a in "$@"; do echo "$a" >> "{tp_sent}"; done\n')
        tp.chmod(0o755)
        mon = fake / "dbus-monitor"
        mon.write_text(FAKE_MONITOR)
        mon.chmod(0o755)
        session = d / "session_appmgr"
        session.write_text("unix:abstract=/tmp/dbus-TESTADDR,guid=0123abcd\n")
        sock = d / "hudnav.sock"
        # Projection+ runtime switches: hudnavd --kit gates on these (an installed kit has them)
        for name in ("enabled", "aa-nav-enable", "cp-enabled"):
            (rw / name).touch()
        env = dict(os.environ, HUDNAV_PATH=f"{fake}:/usr/bin:/bin", HUDNAV_RW_DIR=str(rw),
                   HUDNAV_RUN_DIR=str(run), HUDNAV_SOCK=str(sock),
                   HUDNAV_STATUS=str(d / "status.json"),
                   HUDNAV_SESSION_FILE=str(session), FAKE_MON_ARGS=str(d / "mon.args"))

        def sh(*args):
            return subprocess.run(["sh", str(SCRIPT), *args], env=env, capture_output=True,
                                  text=True, timeout=30)

        def lines():
            return sent.read_text().splitlines() if sent.exists() else []

        try:
            r = sh("start", "tx", "--yes")
            assert r.returncode == 0 and "started: mode=tx" in r.stdout, (r.stdout, r.stderr)
            assert (run / "tx.enable").exists()
            wait_for(sock.exists, "hudnavd socket")
            with socket.socket(socket.AF_UNIX) as c:
                c.connect(str(sock))
                c.sendall(b"OWNER src=aa\nSTATE s=1\nTURN src=aa man=3\n")

                def feed(seconds):
                    end = time.time() + seconds
                    while time.time() < end:
                        c.sendall(b"HB\nDIST m=90\n")
                        time.sleep(0.25)

                # Armed: the monitor delivers owner=111 after ~1s, then frames flow.
                deadline = time.time() + 8
                while time.time() < deadline and not any("0x04,0xcc,0x08,0x0d" in x for x in lines()):
                    feed(0.5)
                assert any("0x04,0xcc,0x08,0x0d" in x for x in lines()), lines()
                assert any("0x04,0xea,0x08" in x and x.endswith("0x10,0x00") for x in lines()), lines()
                # the arrow itself: an empty F2, then the right-turn icon 0x43, in ONE helper call
                wait_for(tp_sent.exists, "TP helper call")
                assert tp_sent.read_text().splitlines()[:2] == [
                    "array:byte:0x06,0xe7,0x03,0xf2,0x00,0x00,0x00",
                    "array:byte:0x06,0xe7,0x08,0xf2,0x01,0x43,0x00,0x00,0x00,0x00,0x00,0x00"], tp_sent.read_text()

                # pause: guidance-off + clear frames, then quiet.
                assert "paused" in sh("pause").stdout
                deadline = time.time() + 5
                while time.time() < deadline and not any(
                        "0x04,0xcc,0x08,0x08,0x00,0x00,0x21" in x for x in lines()):
                    feed(0.3)
                assert any("0x04,0xcc,0x08,0x08,0x00,0x00,0x21" in x for x in lines()), lines()
                assert any("0x04,0xea,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00" in x for x in lines()), lines()
                wait_for(lambda: "0xf0,0x00,0x00,0x00" in tp_sent.read_text(), "blank F0 at clear")
                assert tp_sent.read_text().splitlines()[-3:] == [
                    "array:byte:0x06,0xe7,0x03,0xf4,0x00,0x00,0x00",
                    "array:byte:0x06,0xe7,0x03,0xf2,0x00,0x00,0x00",
                    "array:byte:0x06,0xe7,0x03,0xf0,0x00,0x00,0x00"], tp_sent.read_text()
                feed(0.5)
                quiet_at = len(lines())
                feed(2.0)
                assert len(lines()) == quiet_at, "daemon kept transmitting while paused"

                # resume: transmitting again.
                assert "resumed" in sh("resume").stdout
                deadline = time.time() + 6
                while time.time() < deadline and len(lines()) == quiet_at:
                    feed(0.3)
                assert len(lines()) > quiet_at, "no frames after resume"

                # stop while armed: a final clear goes out before the kill.
                before = len(lines())
                r = sh("stop")
                assert "stopped" in r.stdout, (r.stdout, r.stderr)
            assert any("0x04,0xcc,0x08,0x08,0x00,0x00,0x21" in x for x in lines()[before:]), lines()[before:]
            wait_for(lambda: not procs_mentioning(str(d)), "all hudnav processes gone")
        finally:
            for pid, _ in procs_mentioning(str(d)):
                try:
                    os.kill(int(pid), 9)
                except OSError:
                    pass
    print("tx kill switch (pause/resume/stop) passed")


def main():
    verify_unfed_stdin_is_silent()
    verify_tx_killswitch()
    with tempfile.TemporaryDirectory(prefix="hudnav-wrap-") as d:
        d = Path(d)
        rw, run, fake = d / "rw", d / "run", d / "fakebin"
        (rw / "bin").mkdir(parents=True)
        fake.mkdir()
        shutil.copy(DAEMON, rw / "bin" / "hudnavd-diag")
        mon = fake / "dbus-monitor"
        mon.write_text(FAKE_MONITOR)
        mon.chmod(mon.stat().st_mode | stat.S_IXUSR)
        session = d / "session_appmgr"
        session.write_text("unix:abstract=/tmp/dbus-TESTADDR,guid=0123abcd\n")
        sock = d / "hudnav.sock"
        env = dict(
            os.environ,
            HUDNAV_PATH=f"{fake}:/usr/bin:/bin",
            HUDNAV_RW_DIR=str(rw),
            HUDNAV_RUN_DIR=str(run),
            HUDNAV_SOCK=str(sock),
            HUDNAV_SESSION_FILE=str(session),
            FAKE_MON_ARGS=str(d / "mon.args"),
        )

        def sh(*args):
            return subprocess.run(["sh", str(SCRIPT), *args], env=env, capture_output=True,
                                  text=True, timeout=30)

        try:
            # tx must refuse without --yes, and must not start anything.
            r = sh("start", "tx")
            assert r.returncode != 0 and "--yes" in r.stderr, (r.returncode, r.stderr)
            assert sh("status").stdout.startswith("not running")

            r = sh("start", "diag")
            assert r.returncode == 0 and "started: mode=diag" in r.stdout, (r.stdout, r.stderr)
            st = sh("status").stdout
            assert st.startswith("running: pid=") and "mode=diag" in st, st

            # Monitor was given the bus address WITHOUT ",guid=..." and the two match rules.
            args = wait_for(lambda: (d / "mon.args").exists() and (d / "mon.args").read_text(),
                            "fake dbus-monitor args")
            assert "--address unix:abstract=/tmp/dbus-TESTADDR " in args, args
            assert ",guid=" not in args, args
            assert "member='navi_route_guide_application'" in args, args
            assert "member='NameOwnerChanged'" in args, args

            # The ownership signal reached hudnavd through the fifo (the broken part).
            log = run / "navigation.log"
            wait_for(lambda: log.exists() and "APPMANAGER owner=111" in log.read_text(),
                     "APPMANAGER owner=111 in navigation.log")

            # A projection client on the socket reaches the receiver (shim default path).
            wait_for(sock.exists, "hudnavd socket")
            with socket.socket(socket.AF_UNIX) as c:
                c.connect(str(sock))
                c.sendall(b"OWNER src=aa\nSTATE s=1\nTURN src=aa man=3\nDIST m=90\nHB\n")
                wait_for(lambda: "RX TURN src=aa man=3" in log.read_text()
                         and "RX DIST m=90" in log.read_text(), "RX lines in navigation.log")

            # Clean teardown: nothing left running, socket and fifo removed.
            r = sh("stop")
            assert "stopped" in r.stdout, (r.stdout, r.stderr)
            wait_for(lambda: not procs_mentioning(str(d)), "all hudnav processes gone")
            assert not sock.exists() and not (run / "owners").exists()
            assert sh("status").stdout.startswith("not running")
        finally:
            for pid, _ in procs_mentioning(str(d)):
                try:
                    os.kill(int(pid), 9)
                except OSError:
                    pass
    print("hudnav-run.sh plumbing passed")


main()
