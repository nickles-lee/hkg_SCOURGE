#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Exercise the opt-in trace with fake tools; never touch a radio or system bus."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="wifi-apdiag-", dir="/tmp/opencode") as temporary:
    root = Path(temporary)
    tools = root / "tools"
    tools.mkdir()
    fake = tools / "fake"
    fake.write_text('''#!/usr/bin/python3
import json, os, pathlib, subprocess, sys, time
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["CALLS"], "a") as f: f.write(json.dumps([name, args]) + "\\n")
if name == "timeout":
    assert (args[:2] == ["-k", "1"] and args[2] in ("1", "2", "25")) or args[:3] == ["-k", "2", "90"]
    sys.exit(subprocess.run(args[3:]).returncode)
elif name == "sleep": time.sleep(.01)
elif name == "dbus-monitor":
    assert "--profile" in args and "--system" in args
    if os.environ.get("UNSUPPORTED"):
        print('method call sender=:1.42 interface=net.connman.Technology; member=SetProperty\\n   string "historical-secret"')
        print("profile unsupported", file=sys.stderr); sys.exit(1)
    print("#type timestamp serial sender destination path interface member")
    print("mc 1.0 1 :1.42 :1.1 /fi/w1 fi.w1.wpa_supplicant1.Interface Disconnect", flush=True)
elif name == "dbus-send":
    assert args[-1] == "string::1.42"
    print("   uint32 424242")
elif name == "connmanctl":
    assert args == ["technologies"]
    print("/net/connman/technology/wifi\\n  Powered = True\\n  Tethering = False\\n  TetheringIdentifier = Lab\\n  TetheringPassphrase = historical-secret")
elif name == "wpa_cli":
    assert args[-1] == "status" or args[-2:] == ["get", "ap_scan"]
    print("mode=AP\\nwpa_state=COMPLETED\\nssid=Lab\\npsk=historical-secret" if args[-1] == "status" else "2")
elif name in ("iw", "iwconfig", "ifconfig"):
    assert not any(a in args for a in ("up", "down", "set", "scan"))
    print("fixture read-only interface state")
else: raise AssertionError(name)
''')
    fake.chmod(0o755)
    for name in ("timeout", "sleep", "dbus-monitor", "dbus-send", "connmanctl", "wpa_cli", "iw", "iwconfig", "ifconfig"):
        (tools / name).symlink_to(fake)
    script = root / "capture.sh"
    script.write_text((ROOT / "scripts/run_stuff_apdiag.sh").read_text().replace(
        "PATH=/bin:/sbin:/usr/bin:/usr/sbin", f"PATH={tools}:/usr/bin:/bin").replace(
        "/rw_data/wifi/status.json", str(root / "status.json")))
    (root / "status.json").write_text('{"build_id":"abc123","carplay_psk":"historical-secret"}')
    env = dict(os.environ, USB_DIR=str(root), CALLS=str(root / "calls.jsonl"))
    subprocess.run(["sh", "-n", str(script)], check=True)
    for unsupported in (False, True):
        (root / "calls.jsonl").write_text("")
        subprocess.run(["sh", str(script)], env=dict(env, UNSUPPORTED="1" if unsupported else ""), check=True, timeout=20)
        calls = [json.loads(line) for line in (root / "calls.jsonl").read_text().splitlines()]
        assert sum(name == "dbus-monitor" for name, _ in calls) == 1  # no raw-output fallback
        assert all(name != "connmanctl" or args == ["technologies"] for name, args in calls)
    bundles = list(root.glob("wifi_apdiag_*"))
    assert len(bundles) == 2
    for bundle in bundles:
        text = "\n".join(path.read_text() for path in bundle.iterdir() if path.is_file())
        assert "historical-secret" not in text and "abc123" in text and "get ap_scan" in text
    assert any("bus=:1.42 pid=424242" in (bundle / "callers.txt").read_text() for bundle in bundles)
print("PASS: bounded opt-in AP trace, header-only fallback policy, caller resolution and credential filtering (mock tools)")
