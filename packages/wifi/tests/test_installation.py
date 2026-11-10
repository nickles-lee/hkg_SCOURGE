#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Exercise the real shell installer/restore with fake system commands and paths.

No vehicle binary is executed, no host /etc or /proc is changed, and root is
not required. The production manifest gate stays enabled against test fixtures.
"""
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
KIND = sys.argv[1] if len(sys.argv) > 1 else "stock"
COMMAND = r'''#!/usr/bin/env python3
import json, os, pathlib, sys
root=pathlib.Path(os.environ['FIXTURE'])
statefile=root/'system-state.json'
state=json.loads(statefile.read_text())
name=pathlib.Path(sys.argv[0]).name
args=sys.argv[1:]
failure=os.environ.get('FAIL_POINT','')
with (root/'commands.log').open('a') as f: f.write(name+' '+repr(args)+'\n')
def save(): statefile.write_text(json.dumps(state))
if name in ('sleep','mount','sync','chown','iptables','ip6tables'): sys.exit(0)
if name=='setfattr': sys.exit(1 if failure=='qml-label' else 0)
if name=='date':
 print('20260101T000000Z' if any(a.startswith('+') for a in args) else 'fixture time'); sys.exit(0)
if name=='getfattr':
 if failure in ('label','qml-label'): print('_'); sys.exit(0)
 sys.exit(1)
if name=='chsmack': sys.exit(1 if failure=='label' else 0)
if name=='cp':
 if failure=='qml' and args[-1].endswith('/HomeScreen/WifiPage.qml') and '/usb/payload/' in args[-2]: sys.exit(1)
 os.execv('/usr/bin/cp',['cp',*args])
if name=='pidof':
 if args[0]=='AppHomeScreen' and state['home']: print(state['home']); sys.exit(0)
 sys.exit(1)
if name=='dbus-send':
 if any('.terminate' in a for a in args): state['home']=None
 if any('.launch' in a for a in args): state['serial']+=1; state['home']=state['serial']
 save(); sys.exit(0)
if name=='systemctl':
 command=args[0]; unit=args[-1]
 if command=='is-active': sys.exit(0 if state['active'].get(unit,False) else 3)
 if command=='is-enabled':
  value=state['enabled'].get(unit,'disabled')
  if '--quiet' not in args: print(value)
  sys.exit(0 if value in ('enabled','static','masked') else 1)
 if command=='show': print('MainPID=1234'); sys.exit(0)
 if command=='mask': state['enabled'][unit]='masked'
 elif command=='unmask': state['enabled'][unit]='enabled'
 elif command=='enable': state['enabled'][unit]='enabled'
 elif command=='disable': state['enabled'][unit]='disabled'
 elif command=='stop': state['active'][unit]=False
 elif command in ('start','restart'):
  if failure=='daemon' and unit=='wifi-daemon.service' and command=='restart': sys.exit(1)
  state['active'][unit]=True
  if unit=='wifi-daemon.service':
   rw=root/'root/rw_data/wifi'; rw.mkdir(parents=True,exist_ok=True)
   (rw/'status.json').write_text(json.dumps({'build_id':'fixture-build'},separators=(',',':')))
   proc=root/'root/proc/1234'; proc.mkdir(parents=True,exist_ok=True)
   (proc/'exe').write_bytes((rw/'wifi_daemon').read_bytes())
 save(); sys.exit(0)
sys.exit(1)
'''


class Fixture:
    def __init__(self, directory, kind="stock", existing=False):
        self.path = Path(directory)
        self.root = self.path / "root"
        self.usb = self.path / "usb"
        self.rw = self.root / "rw_data/wifi"
        self.qml = self.root / "app/share/AppHomeScreen/qml_eco3.0"
        self.bin = self.path / "bin"
        self.bin.mkdir()
        command = self.bin / "fake-command"
        command.write_text(COMMAND)
        command.chmod(0o755)
        for name in ("systemctl", "sleep", "mount", "sync", "chown", "date", "getfattr", "setfattr",
                     "chsmack", "cp", "pidof", "dbus-send", "iptables", "ip6tables"):
            (self.bin / name).symlink_to(command)
        for directory in ("etc/systemd/system", "tmp", "proc", "home", "rw_data/wifi"):
            (self.root / directory).mkdir(parents=True, exist_ok=True)
        (self.root / "proc/mounts").write_text("")
        (self.root / "tmp/shells").write_text("")
        for name in ("passwd.legacy", "passwd.secure"):
            (self.root / "etc" / name).write_text("root:x:0:0:root:/root:/bin/sh\n")
        self.car = "car:x:123:123:Existing owner:/home/existing:/bin/sh\n"
        (self.root / "tmp/passwd").write_text("root:x:0:0:root:/root:/bin/sh\n" + (self.car if existing else ""))
        (self.root / "etc/shadow.Global").write_text("root:*:0:0:99999:7:::\n" + ("car:owner-custom-hash:0:0:99999:7:::\n" if existing else ""))
        (self.root / "etc/passwd").symlink_to(self.root / "tmp/passwd")
        (self.root / "tmp/shadow").symlink_to(self.root / "etc/shadow.Global")
        (self.root / "etc/shadow").symlink_to(self.root / "tmp/shadow")
        base = {"stock": "stock", "hello": "hello", "battery": "battery", "hello_battery": "battery_hello",
                "battery_tile": "battery_tile", "hello_battery_tile": "battery_tile_hello"}[kind]
        advanced = "QHMI_HomeScreenMain_Advanced.hello.qml" if kind == "hello_battery" else "QHMI_HomeScreenMain_Advanced.qml"
        names = {"HomeScreen/QHMI_HomeScreenMain_Advanced.qml": ROOT / "base" / base / "HomeScreen" / advanced}
        other_base = "hello" if "hello" in kind else "stock"
        for relative in ("HomeScreen/QHMI_HomeScreenMain_Wide.qml", "HomeScreen/Screen_HomeScreen_AllMenus.qml",
                         "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml", "component/QCustomizedMenuListAllowEmpty.qml"):
            names[relative] = ROOT / "base" / other_base / relative
        theme = ROOT / "out/usb/backup/AppHomeScreen/qml_eco3.0/component/common/Q_ThemeImage.qml"
        names["component/common/Q_ThemeImage.qml"] = theme
        self.original_qml = {}
        for relative, source in names.items():
            target = self.qml / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
            self.original_qml[relative] = target.read_bytes()
        shutil.copytree(ROOT / "out/usb", self.usb)
        daemon = self.usb / "payload/bin/wifi_daemon"
        daemon.write_text("#!/bin/sh\nexit 0\n")
        daemon.chmod(0o755)
        (self.usb / "payload/build-id.txt").write_text("fixture-build\n")
        self.manifest()
        state = {"active": {}, "enabled": {"supplicantmanager.path": "enabled", "supplicantmanager.service": "static"},
                 "home": 1000, "serial": 1000}
        (self.path / "system-state.json").write_text(json.dumps(state))
        for script in ("run_stuff.sh", "run_stuff_restore.sh"):
            text = (ROOT / "scripts" / script).read_text()
            for prefix, target in [("/root/.cache", self.root / "cache"), ("/rw_data", self.root / "rw_data"),
                                   ("/home/car", self.root / "home/car"), ("/app", self.root / "app"),
                                   ("/etc", self.root / "etc"), ("/proc", self.root / "proc"), ("/tmp", self.root / "tmp")]:
                # A placeholder prevents replacement of '/tmp' inside our own
                # already-expanded fixture directory path.
                text = re.sub(re.escape(prefix) + r"(?=/|[^A-Za-z0-9_]|$)",
                              lambda _: "@@" + str(target.relative_to(self.path)) + "@@", text)
            text = re.sub(r"@@(.*?)@@", lambda m: str(self.path / m.group(1)), text)
            (self.path / script).write_text(text)
        self.env = dict(os.environ, FIXTURE=str(self.path), WIFI_PATH=str(self.bin) + ":/usr/bin:/bin",
                        USB_DIR=str(self.usb), WIFI_RW_DIR=str(self.rw),
                        WIFI_QML_ROOT=str(self.qml.parent), WIFI_SERVICE=str(self.root / "etc/systemd/system/wifi-daemon.service"))

    def manifest(self):
        lines = [f"{hashlib.md5(p.read_bytes()).hexdigest()}  {p.relative_to(self.usb)}\n"
                 for p in sorted((self.usb / "payload").rglob("*")) if p.is_file()]
        (self.usb / "manifest.md5").write_text("".join(lines))

    def run(self, restore=False, failure="", success=True):
        env = dict(self.env, FAIL_POINT=failure)
        if failure in ("label", "qml-label"): (self.root / "proc/mounts").write_text("smackfs /sys/fs/smackfs smackfs rw 0 0\n")
        script = self.path / ("run_stuff_restore.sh" if restore else "run_stuff.sh")
        result = subprocess.run(["sh", str(script)], env=env, capture_output=True, text=True, timeout=25)
        report = (self.usb / "run_stuff_result.txt").read_text()
        if success:
            assert result.returncode == 0 and "status=OK" in report, (report, result.stderr, (self.rw / "install.log").read_text())
        else:
            assert result.returncode != 0 and "status=FAIL" in report, (report, result.stderr)

    def assert_qml_original(self):
        for relative, contents in self.original_qml.items():
            assert (self.qml / relative).read_bytes() == contents, relative


with tempfile.TemporaryDirectory(prefix="wifi-install-", dir="/tmp/opencode") as directory:
    if KIND == "ownership":
        fixture = Fixture(directory, existing=True)
        original_keys = "fixture-owner-public-key\n"
        for relative in ("ssh/authorized_keys", "home/.ssh/authorized_keys"):
            target = fixture.rw / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(original_keys)
        fixture.run()
        fixture.run(restore=True)
        assert fixture.car.strip() in (fixture.root / "etc/passwd").read_text()
        assert "owner-custom-hash" in (fixture.root / "etc/shadow.Global").read_text()
        for name in ("passwd.legacy", "passwd.secure"):
            assert "car:" not in (fixture.root / "etc" / name).read_text()
        for relative in ("ssh/authorized_keys", "home/.ssh/authorized_keys"):
            assert (fixture.rw / relative).read_text() == original_keys
        print("PASS: per-file account ownership preserves pre-existing car entries")
    else:
        fixture = Fixture(directory, KIND)
        fixture.run()
        baseline = (fixture.rw / "backup/latest").resolve()
        fixture.run()  # reinstall must retain the original restore point
        assert (fixture.rw / "backup/latest").resolve() == baseline
        assert len(list((fixture.rw / "backup").glob("20260101T000000Z*"))) == 2
        for identity, relative in [("app", "app/data/vbt/VBTConfig.json"), ("rw", "rw_data/app_data/vbt/VBTConfig.json")]:
            vbt = fixture.root / relative
            vbt.parent.mkdir(parents=True, exist_ok=True)
            original = {"ENABLE_WIRELESS_CARPLAY": False, "identity": identity, "other": 1}
            (fixture.rw / "backup" / f"vbt-{identity}.original").write_text(json.dumps(original))
            applied = dict(original, ENABLE_WIRELESS_CARPLAY=True)
            (fixture.rw / "backup" / f"vbt-{identity}.applied").write_text(json.dumps(applied))
            vbt.write_text(json.dumps(dict(applied, other=2)))
        # Model the firmware's boot-time template copy, preserving symlinks.
        shutil.copyfile(fixture.root / "etc/passwd.secure", fixture.root / "tmp/passwd")
        (fixture.rw / "ap.station-ip").write_text("192.0.2.10")
        fixture.run(restore=True)
        fixture.assert_qml_original()
        for relative in ("etc/passwd", "etc/passwd.legacy", "etc/passwd.secure", "etc/shadow.Global"):
            assert "car:" not in (fixture.root / relative).read_text(), relative
        assert (fixture.root / "etc/passwd").is_symlink() and (fixture.root / "etc/shadow").is_symlink()
        for identity, relative in [("app", "app/data/vbt/VBTConfig.json"), ("rw", "rw_data/app_data/vbt/VBTConfig.json")]:
            data = json.loads((fixture.root / relative).read_text())
            assert data == {"ENABLE_WIRELESS_CARPLAY": False, "identity": identity, "other": 2}, data
        assert not (fixture.rw / "wifi_daemon").exists()
        assert not (fixture.rw / "ap.station-ip").exists()
        print(f"PASS: {KIND} install/reinstall/restore, symlinks and independent VBT baselines")

if KIND == "stock":
    for owner_dropin in (False, True):
        with tempfile.TemporaryDirectory(prefix="wifi-mdns-rollback-", dir="/tmp/opencode") as directory:
            fixture = Fixture(directory)
            stock = b"stock-mdns-fixture\n"
            rooted = b"rooted-mdns-fixture\n"
            target = fixture.root / "app/bin/mdnsd"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(stock)
            inode = target.stat().st_ino
            dropin = fixture.root / "etc/systemd/system/carplay_mdnsd.service.d/override.conf"
            if owner_dropin:
                dropin.parent.mkdir(parents=True, exist_ok=True)
                dropin.write_text("[Service]\nRestart=on-failure\n")
            (fixture.usb / "payload/bin/mdnsd.rooted").write_bytes(rooted)
            (fixture.usb / "MDNSD_ROOT_ARM").touch()
            script = fixture.path / "run_stuff.sh"
            script.write_text(script.read_text().replace("f63a65c4c2b0c011ae8ed17557b45c1b", hashlib.md5(stock).hexdigest())
                              .replace("f847373d7c5a6906453ba921bb790d22", hashlib.md5(rooted).hexdigest()))
            fixture.manifest()
            fixture.run(failure="daemon", success=False)
            assert target.read_bytes() == stock and target.stat().st_ino == inode
            if owner_dropin:
                assert dropin.read_text() == "[Service]\nRestart=on-failure\n"
            else:
                assert not dropin.exists()
            print("PASS: armed mdns experiment rolls back binary and drop-in on later failure")
    for failure in ("qml", "daemon", "label", "manifest"):
        with tempfile.TemporaryDirectory(prefix="wifi-rollback-", dir="/tmp/opencode") as directory:
            fixture = Fixture(directory)
            if failure == "manifest": (fixture.usb / "payload/build-id.txt").write_text("corrupted\n")
            fixture.run(failure=failure, success=False)
            fixture.assert_qml_original()
            assert (fixture.root / "etc/passwd").is_symlink()
            for relative in ("etc/passwd", "etc/passwd.legacy", "etc/passwd.secure", "etc/shadow.Global"):
                assert "car:" not in (fixture.root / relative).read_text(), (failure, relative)
            assert not (fixture.rw / "wifi_daemon").exists(), failure
            assert not (fixture.rw / "backup/latest").exists(), failure
            state = json.loads((fixture.path / "system-state.json").read_text())
            assert state["enabled"]["supplicantmanager.path"] == "enabled", failure
            assert not state["active"].get("wifi-daemon.service", False), failure
            print(f"PASS: rollback preserves original state after {failure} failure")
    # onlycap=Privileged rejects security-xattr writes from the '_' installer
    # context: setfattr fails while getfattr still returns the label.  The
    # install must complete anyway (this is what kept the fixed WifiPage.qml
    # off the car on 2026-11-10), and restore must stay usable in the same
    # state.  When the inherited label already equals the target label the
    # installer continues silently; when it differs and is the floor label
    # it warns and keeps it.
    with tempfile.TemporaryDirectory(prefix="wifi-onlycap-", dir="/tmp/opencode") as directory:
        fixture = Fixture(directory)
        fixture.run(failure="qml-label")
        result = (fixture.usb / "run_stuff_result.txt").read_text()
        assert re.search(r"^status=(OK|REBOOT_REQUIRED)(\s|$)", result, re.M), result
        assert "FAIL" not in result, result
        # the new WifiPage.qml is installed (cp kept the inode/label)
        assert (fixture.qml / "HomeScreen/WifiPage.qml").exists()
        fixture.run(restore=True, failure="qml-label")
        fixture.assert_qml_original()
        print("PASS: onlycap setfattr failure keeps install/restore working")
