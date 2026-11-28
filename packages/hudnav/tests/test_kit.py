# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Projection+ runtime switches, settings.conf reload and status file (log-only CAN mode)."""

from pathlib import Path
import json
import socket
import subprocess
import tempfile
import time


def run(work):
    kit = work / "kit"
    kit.mkdir()
    for name in ("enabled", "aa-nav-enable", "cp-enabled"):
        (kit / name).touch()
    status = work / "status.json"
    sock_path = str(work / "hudnav.sock")
    log_path = work / "frames"
    with log_path.open("wb") as log, socket.socket(socket.AF_UNIX) as projection:
        proc = subprocess.Popen(
            ["./out/hudnavd", "--can", "--appmanager-stdin", "--socket", sock_path,
             "--kit", str(kit), "--status", str(status)],
            stdin=subprocess.PIPE, stderr=log)
        try:
            for _ in range(100):
                try:
                    projection.connect(sock_path)
                    break
                except (FileNotFoundError, ConnectionRefusedError):
                    time.sleep(0.02)
            else:
                raise RuntimeError("hudnavd did not listen")

            def owner(app_id):
                header = ("signal sender=:1.28 -> dest=(null destination) serial=1 "
                          "path=/Information; interface=com.lge.avn.appmanager.info; "
                          "member=navi_route_guide_application")
                proc.stdin.write(f"{header}\n   int32 {app_id}\n".encode())
                proc.stdin.flush()

            def projected(*lines):
                projection.sendall(("\n".join(lines) + "\n").encode())

            seen = 0

            def frames(wait=0.55):
                nonlocal seen
                time.sleep(wait)
                lines = [l for l in log_path.read_text().splitlines() if l.startswith("MICOM ")]
                out = []
                for line in lines[seen:]:
                    raw = line.split()[2].split(":", 2)[2]
                    high, low, _size, *payload = (int(b, 16) for b in raw.split(","))
                    out.append(((high << 8) | low, bytes(payload)))
                seen = len(lines)
                return out

            def last_list(rows):
                lists = [d for f, d in rows if f == 0x63E]
                assert lists, rows
                return lists[-1][:4]

            def st():
                return json.loads(status.read_text())

            # AA guiding, default settings (balanced): 347 m -> 350 m
            owner(111)
            projected("STATE s=1", "TURN src=aa man=3", "DIST m=347")
            rows = frames()
            assert last_list(rows) == bytes.fromhex("00 00 5e 01"), rows
            s = st()
            assert s["v"] == 1 and s["health"] == "guiding" and s["source"] == "android_auto", s
            assert s["aa_connected"] is True and s["cp_connected"] is False, s
            assert s["next_turn_m"] > 0 and s["dest_m"] == -1 and s["last_error"] == "", s
            assert abs(s["updated_ms"] / 1000 - time.time()) < 5, s
            assert set(s) == {"updated_ms", "v", "daemon", "version", "health", "source",
                              "next_turn_m", "dest_m", "aa_connected", "cp_connected",
                              "last_error"}, s

            # settings.conf: AA precise -> 347 m exactly (reload within ~1 s, atomic rename)
            tmp = kit / "settings.conf.tmp"
            tmp.write_text("aa_steps=precise\ncp_steps=calm\n")
            tmp.rename(kit / "settings.conf")
            frames(1.3)
            projected("DIST m=347")
            assert last_list(frames()) == bytes.fromhex("00 00 5b 01")
            # invalid value -> default (balanced) again
            tmp.write_text("aa_steps=fast\n")
            tmp.rename(kit / "settings.conf")
            frames(1.3)
            projected("DIST m=347")
            assert last_list(frames()) == bytes.fromhex("00 00 5e 01")

            # AA source switched off: one clear, then silence while AA keeps talking
            (kit / "aa-nav-enable").unlink()
            rows = frames(1.3)
            ids = [f for f, _ in rows]
            assert 0x4EA in ids and 0x4E8 in ids, rows
            projected("DIST m=300")
            assert frames(0.8) == []
            assert st()["health"] == "not_guiding", st()

            # back on: guidance resumes
            (kit / "aa-nav-enable").touch()
            frames(1.3)
            projected("DIST m=300")
            got = last_list(frames())
            # the motion model glides between samples, so only check it is metres, near 300 m
            assert got[0] == 0 and 250 <= int.from_bytes(got[2:4], "little") <= 300, got

            # master off: clear, then nothing; status says disabled
            (kit / "enabled").unlink()
            rows = frames(1.3)
            assert any(f == 0x4EA for f, _ in rows), rows
            projected("DIST m=250")
            assert frames(0.8) == []
            assert st()["health"] == "disabled", st()
            (kit / "enabled").touch()

            # CarPlay off: CP input is ignored entirely
            (kit / "cp-enabled").unlink()
            owner(110)
            frames(1.3)
            projected("STATE s=1", "TURN src=cp man=2", "DIST m=140", "DEST m=800")
            assert not any(f == 0x4CC for f, _ in frames(0.8))
            # CarPlay on with calm: 94 m -> 90 m
            tmp.write_text("cp_steps=calm\n")
            tmp.rename(kit / "settings.conf")
            (kit / "cp-enabled").touch()
            frames(1.3)
            projected("TURN src=cp man=3", "DIST m=94", "DEST m=800")
            got = last_list(frames())
            assert got == bytes.fromhex("00 00 5a 00"), got.hex()
            s = st()
            assert s["source"] == "carplay" and s["dest_m"] == 800 and s["cp_connected"], s

            # no street names or coordinates can appear: only the fixed key set (checked above)
            proc.stdin.close()
            time.sleep(0.6)
            assert st()["health"] == "error" and st()["last_error"], st()
        finally:
            proc.terminate()
            proc.wait(timeout=3)
            if not proc.stdin.closed:
                proc.stdin.close()
        # SIGTERM leaves the last file behind; readers treat > 8 s old as stale (contract §3).
        assert not (work / "status.json.tmp").exists()


with tempfile.TemporaryDirectory(prefix="hudnav-kit-") as directory:
    run(Path(directory))
print("Projection+ switches, settings reload and status file passed")
