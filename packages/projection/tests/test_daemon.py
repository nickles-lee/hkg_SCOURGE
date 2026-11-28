#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Host test for projection_daemon (contract: design/BACKEND_CONTRACT.md §4). Offline only."""
import http.client
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
DAEMON = HERE.parent / "src" / "projection_daemon"
checks = 0
fails = 0


def check(cond, what):
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print("FAIL:", what, file=sys.stderr)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    with tempfile.TemporaryDirectory(prefix="projd-") as d:
        d = Path(d)
        kit, guard, status = d / "kit", d / "guard", d / "status.json"
        port = free_port()
        proc = subprocess.Popen([str(DAEMON), "--port", str(port), "--kit", str(kit),
                                 "--status", str(status), "--guard", str(guard)],
                                stderr=subprocess.DEVNULL)
        try:
            for _ in range(100):
                try:
                    socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                    break
                except OSError:
                    time.sleep(0.02)

            def req(method, path, body=None, raw=None):
                c = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
                data = raw if raw is not None else (json.dumps(body) if body is not None else None)
                c.request(method, path, body=data,
                          headers={"Content-Type": "application/json"} if data is not None else {})
                r = c.getresponse()
                text = r.read().decode()
                c.close()
                try:
                    return r.status, json.loads(text)
                except ValueError:
                    return r.status, text

            def snapshot():
                return {p.name: p.read_bytes() for p in kit.iterdir()} if kit.exists() else None

            # 1. no kit: service answers (fresh), reports not installed, refuses writes
            code, s = req("GET", "/status")
            check(code == 200 and s["supported"] is False, f"no kit status {s}")
            check(abs(s["updated_ms"] / 1000 - time.time()) < 5, "updated_ms is the daemon clock")
            code, s = req("POST", "/config", {"aa_enabled": True})
            check(code == 503 and "not installed" in s.get("error", ""), f"no kit write {code} {s}")
            check(not kit.exists(), "nothing created without a kit")

            # 2. kit present, everything off, no hudnavd status
            kit.mkdir()
            code, s = req("GET", "/status")
            check(code == 200 and s["supported"] is True and s["hudnav_enabled"] is False, s)
            check(s["health"] == "disabled" and s["stale"] is True, f"off + no status {s}")
            check(s["aa_distance_steps"] == "balanced" and s["cp_units"] == "car", "defaults")

            # 3. each valid key
            code, s = req("POST", "/config", {"hudnav_enabled": True, "aa_enabled": True,
                                              "cp_enabled": True})
            check(code == 200 and s["hudnav_enabled"] and s["aa_enabled"] and s["cp_enabled"], s)
            check(all((kit / f).is_file() for f in ("enabled", "aa-nav-enable", "cp-enabled")),
                  "switch files created")
            check(s["health"] == "error" and "not running" in s["last_error"], f"on, no hudnavd {s}")
            for key, vals in (("aa_distance_steps", ("precise", "calm", "balanced")),
                              ("cp_distance_steps", ("calm", "precise")),
                              ("aa_units", ("imperial", "metric", "car")),
                              ("cp_units", ("metric",))):
                for v in vals:
                    code, s = req("POST", "/config", {key: v})
                    check(code == 200 and s[key] == v, f"{key}={v} -> {code} {s}")
            conf = (kit / "settings.conf").read_text()
            check("aa_steps=balanced" in conf and "cp_steps=precise" in conf and
                  "aa_units=car" in conf and "cp_units=metric" in conf, conf)
            check(not (kit / "settings.conf.tmp").exists(), "no tmp left behind")
            code, s = req("POST", "/config", {"cp_enabled": False})
            check(code == 200 and s["cp_enabled"] is False and not (kit / "cp-enabled").exists(),
                  "cp off")

            # 4. invalid requests: 400 and nothing changes on disk
            before = snapshot()
            bad = [
                {"aa_units": "miles"}, {"aa_distance_steps": "PRECISE"}, {"bogus": True},
                {"aa_enabled": "true"}, {"hudnav_enabled": 1}, {"aa_units": True},
                {"restore_stock": False}, {"aa_enabled": True, "nope": 1},
            ]
            for b in bad:
                code, s = req("POST", "/config", b)
                check(code == 400, f"{b} -> {code}")
            for raw in ('{"aa_enabled": true, "aa_enabled": false}', '{"aa_enabled": true} x',
                        '[]', '', '{"aa_enabled" true}', '{"aa_units":"car"', 'null'):
                code, s = req("POST", "/config", raw=raw)
                check(code == 400, f"raw {raw!r} -> {code}")
            check(snapshot() == before, "invalid requests changed nothing")
            code, _ = req("GET", "/config")
            check(code == 400, "GET /config rejected")
            code, _ = req("GET", "/nope")
            check(code == 404, "unknown path 404")

            # 5. oversized request
            code, _ = req("POST", "/config", raw='{"aa_units":"' + "x" * 5000 + '"}')
            check(code in (400, 413), f"oversized -> {code}")
            check(snapshot() == before, "oversized changed nothing")

            # 6. fresh hudnavd status is merged; stale is ignored
            def write_status(age_s, **kw):
                doc = {"updated_ms": int((time.time() - age_s) * 1000), "v": 1, "daemon": "hudnavd",
                       "version": "t1", "health": "guiding", "source": "android_auto",
                       "next_turn_m": 350, "dest_m": -1, "aa_connected": True,
                       "cp_connected": False, "last_error": ""}
                doc.update(kw)
                status.write_text(json.dumps(doc, separators=(",", ":")))
            write_status(0)
            code, s = req("GET", "/status")
            check(s["health"] == "guiding" and s["source"] == "android_auto" and
                  s["next_turn_m"] == 350 and s["guidance_active"] and s["stale"] is False and
                  s["version_hudnav"] == "t1", f"fresh merge {s}")
            write_status(20)
            code, s = req("GET", "/status")
            check(s["stale"] is True and s["health"] == "error" and s["next_turn_m"] == -1,
                  f"stale ignored {s}")
            write_status(0, health="exploded", source="pager")
            code, s = req("GET", "/status")
            check(s["stale"] is True, f"invalid hudnavd health rejected {s}")
            write_status(0, health="not_guiding", source="satnav", last_error='bad\\"x')
            code, s = req("GET", "/status")
            check(s["source"] == "none" and json.dumps(s), f"unknown source -> none {s}")
            status.write_text("{garbage")
            code, s = req("GET", "/status")
            check(code == 200 and s["stale"] is True, "garbage status file")

            # 7. boot guard
            guard.mkdir()
            (guard / "armed").touch()
            check(req("GET", "/status")[1]["boot_guard"] == "armed", "guard armed")
            (guard / "rolled_back").write_text("x")
            (guard / "fails").write_text("2\n")
            s = req("GET", "/status")[1]
            check(s["boot_guard"] == "rolled_back" and s["boot_failures"] == 2, f"guard {s}")

            # 8. restore_stock: everything off, settings untouched
            code, s = req("POST", "/config", {"restore_stock": True, "hudnav_enabled": True,
                                              "aa_enabled": False, "cp_enabled": False})
            check(code == 200 and not s["hudnav_enabled"] and not s["aa_enabled"] and
                  not s["cp_enabled"], f"restore {s}")
            check(not any((kit / f).exists() for f in ("enabled", "aa-nav-enable", "cp-enabled")),
                  "restore removed all switches")
            check((kit / "settings.conf").exists(), "restore keeps settings.conf")

            # 9. symlinked switch is not followed or reported as on
            target = d / "elsewhere"
            os.symlink(target, kit / "enabled")
            check(req("GET", "/status")[1]["hudnav_enabled"] is False, "symlink is not 'on'")
            code, _ = req("POST", "/config", {"hudnav_enabled": True})
            check(code == 503 and not target.exists(), "symlinked switch not written through")
            (kit / "enabled").unlink()

            # 10. hand-edited settings.conf with junk: defaults for invalid values
            (kit / "settings.conf").write_text("aa_steps=fast\ncp_units = imperial \n# x\n")
            s = req("GET", "/status")[1]
            check(s["aa_distance_steps"] == "balanced" and s["cp_units"] == "imperial", s)

            # 11. a silent client does not block others (2 s timeout)
            idle = socket.create_connection(("127.0.0.1", port))
            t0 = time.time()
            code, _ = req("GET", "/status")
            check(code == 200 and time.time() - t0 < 4, "idle client does not block")
            idle.close()
            check(proc.poll() is None, "daemon still running")
        finally:
            proc.terminate()
            proc.wait(timeout=3)
    print(f"projection_daemon: {checks} checks, {fails} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
