# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Exercise the AppManager feed, projection socket and CAN frames in log-only mode."""

from pathlib import Path
import socket
import subprocess
import tempfile
import time


def verify_handoff(work):
    sock_path = str(work / "hudnav.sock")
    log_path = work / "frames"
    with log_path.open("wb") as log, socket.socket(socket.AF_UNIX) as projection:
        proc = subprocess.Popen(
            ["./out/hudnavd", "--can", "--appmanager-stdin", "--socket", sock_path],
            stdin=subprocess.PIPE,
            stderr=log,
        )
        try:
            for _ in range(100):
                try:
                    projection.connect(sock_path)
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    time.sleep(0.02)
            else:
                raise RuntimeError("hudnavd did not listen")

            def owner(app_id, kind="signal"):
                # The header and int32 body are separate lines in dbus-monitor.
                header = (
                    f"{kind} sender=:1.28 -> dest=(null destination) serial=8477 "
                    "path=/Information; interface=com.lge.avn.appmanager.info; "
                    "member=navi_route_guide_application"
                )
                proc.stdin.write(f"{header}\n   int32 {app_id}\n".encode())
                proc.stdin.flush()

            def projected(*lines):
                projection.sendall(("\n".join(lines) + "\n").encode())

            seen = 0

            def frames_after_step():
                nonlocal seen
                time.sleep(0.55)
                lines = [line for line in log_path.read_text().splitlines() if line.startswith("MICOM ")]
                result = []
                for line in lines[seen:]:
                    raw = line.split()[2].split(":", 2)[2]
                    high, low, size, *payload = (int(byte, 16) for byte in raw.split(","))
                    assert size == len(payload) == 8
                    result.append(((high << 8) | low, bytes(payload)))
                seen = len(lines)
                return result

            tp_seen = 0

            def tp_after_step():
                """CAN-TP messages (MICOMTP lines) logged since the last call, as hex strings."""
                nonlocal tp_seen
                lines = [l for l in log_path.read_text().splitlines() if l.startswith("MICOMTP ")]
                out = [bytes(int(b, 16) for b in l.split()[2].split(",")).hex() for l in lines[tp_seen:]]
                tp_seen = len(lines)
                return out

            EMPTY_F2 = "06e703f2000000"

            def f2(icon):
                return f"06e708f201{icon:02x}000000000000"

            def verify_turn(turn, list_prefix, dest):
                rows = frames_after_step()
                assert rows[0][0] == 0x4EA and rows[0][1][6] == 0x10, rows
                frames = dict(rows)
                assert frames[0x4CC] == bytes.fromhex(turn), frames
                assert frames[0x63E][:4] == bytes.fromhex(list_prefix), frames
                # lookahead legs are unknown -> stock "no further maneuver" marker
                assert frames[0x63E][4:] == b"\xff\xff\xff\xff", frames
                assert frames[0x4E8] == bytes.fromhex(dest), frames

            owner(110, kind="method call")
            projected("OWNER src=cp", "STATE s=1", "TURN src=cp man=2", "DIST m=90")
            assert frames_after_step() == []

            owner(110)
            assert frames_after_step() == []
            # 1600 m destination -> 0x4E8 B0=0x61 (1.6 km), B3=0x0c (LHT roundabout exit 90 deg)
            projected("TURN src=cp man=15 exit=270", "DIST m=90", "DEST m=1600")
            verify_turn("21 00 00 0c 5a 00 00 00", "00 00 5a 00",
                        "61 15 1f 0c 01 00 50 00")
            # the HUD arrow comes from the F2 list: empty F2 first, then ONE icon (LHT roundabout exit
            # 270 -> 0xC9), and nothing more while the maneuver is unchanged
            assert tp_after_step() == [EMPTY_F2, f2(0xC9)]

            owner(30)
            assert frames_after_step() == []
            projected("OWNER src=cp", "STATE s=1", "TURN src=cp man=3", "DIST m=90")
            assert frames_after_step() == []

            owner(110)
            projected("DIST m=90")
            assert frames_after_step() == []
            # 800 m destination -> unit 0 (metres), B3=0x24 (left)
            projected("TURN src=cp man=2", "DIST m=140", "DEST m=800")
            verify_turn("0d 00 00 24 8c 00 00 00", "00 00 8c 00",
                        "00 15 1f 24 20 03 50 00")
            assert tp_after_step() == [EMPTY_F2, f2(0x46)]    # left turn
            owner(0)
            cleared = frames_after_step()
            # stock end-of-route: blank F4, F2, F0 (the blank F2 removes the HUD arrow)
            assert tp_after_step() == ["06e703f4000000", EMPTY_F2, "06e703f0000000"]
            assert [frame_id for frame_id, _ in cleared] == [
                0x4EA, 0x4E9, 0x63E, 0x640, 0x4E8, 0x4CC, 0x4E8], cleared
            assert cleared[0][1][6] == 0
            assert cleared[4][1] == bytes.fromhex("ff ff ff ff ff ff ff 00")
            # Car 2026-11-27: type 03 (stock end-of-route) left the checkered flag on the HUD with the
            # car parked, so the clear no longer sends it. Never type 09 either ("Calculating"
            # spinner). The 0x4CC of the clear set is the idle compass frame.
            assert not any(f == 0x4CC and d[0] in (0x03, 0x09) for f, d in cleared), cleared
            assert cleared[5][1] == bytes.fromhex("08 00 00 21 00 00 00 00"), cleared
            assert cleared[6][1] == bytes.fromhex("ff ff ff 21 ff ff ff 00"), cleared
            assert frames_after_step() == []

            owner(111)
            assert frames_after_step() == []
            projected("STATE s=1", "TURN src=cp man=3", "DIST m=90")
            assert frames_after_step() == []
            # AA has no destination-distance callback -> 0x4E8 stays hidden
            projected("TURN src=aa man=3", "DIST m=90")
            verify_turn("0d 00 00 0c 5a 00 00 00", "00 00 5a 00",
                        "ff ff ff 00 ff ff ff 00")
            assert tp_after_step() == [EMPTY_F2, f2(0x43)]    # right turn
            proc.stdin.close()
            assert frames_after_step() == []
        finally:
            proc.terminate()
            proc.wait(timeout=3)
            if not proc.stdin.closed:
                proc.stdin.close()


with tempfile.TemporaryDirectory(prefix="hudnav-handoff-") as directory:
    verify_handoff(Path(directory))
print("AppManager handoff and stock maneuver replay passed")
