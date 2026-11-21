#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Offline side-by-side install/restore test for Wifi1b.

Builds a fake head-unit root, installs the WiFi artifacts first, then runs the
real Wifi1b installer and restore scripts with fake system commands.  It never
touches the host /etc, /proc, /tmp, /app or /rw_data.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
WIFI = ROOT.parent / "wifi"
TWEAKS = ROOT.parent / "tweaks"
sys.path.insert(0, str(ROOT.parent / "common"))
from qml_layout import HELPERS  # noqa: E402


def write_synthetic_qml(qml: Path, wifi: bool, tweaks: bool) -> None:
    """Build fixture QML with the anchors the first-party patcher expects.

    No stock/OEM QML is copied.  These are synthetic fixtures only.
    """
    common_marker = "// WIFI_PAYLOAD_260128\n" if wifi else ""
    tweaks_marker = "// TWEAKS_PAYLOAD_260128\n"
    header = common_marker + tweaks_marker + "// SCOURGE_LAYOUT_V2\n"
    advanced = [header, HELPERS]
    if wifi:
        advanced.append("WifiPage { id: wifiPage }\n")
    for page in (1, 2, 3):
        advanced.append("QMenuPageNoDrag { }\n")
        if wifi:
            advanced.append(f"WifiTile {{ id: wifiTile{page}; width: 1; height: 1 }}\n")
        advanced.append(f"BatteryTile {{ id: batteryTile{page}; width: 1; height: 1 }}\n")
        if tweaks:
            advanced.append(f"TweaksTile {{ id: tweaksTile{page}; width: 1; height: 1 }}\n")
            advanced.append(f"RebootTile {{ id: rebootTile{page}; width: 1; height: 1 }}\n")
    advanced.append("Item { }\n")
    (qml / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml").write_text("".join(advanced))

    wide_anchor = "itemWifi" if wifi else ("itemReboot" if tweaks else "itemWifi")
    wide = [header, "Item {\n    id: root\n"]
    wide.append(f"    QHomeItem {{ id: {wide_anchor}; width: 200 : 207 }}\n")
    wide.append("    GridView { }\n}\n")
    (qml / "HomeScreen/QHMI_HomeScreenMain_Wide.qml").write_text("".join(wide))

    for name in ("Screen_HomeScreen_AllMenus.qml", "Screen_HomeScreen_Submenu_Setup.qml"):
        (qml / "HomeScreen" / name).write_text(
            header + "Item {\n    onMenuItemClicked: { }\n    BatteryTile { id: batteryTile0 }\n}\n")

    custom = [header, "Item {\n"]
    custom.append("    signal wifiClicked()\n" if wifi else "    signal tweaksClicked()\n")
    custom.append("    BatteryTile { id: batteryTile9; width: 1; height: 1 }\n}\n")
    (qml / "component/QCustomizedMenuListAllowEmpty.qml").write_text("".join(custom))


def build_payload_fixture(usb: Path, workdir: Path) -> None:
    """Assemble a first-party fixture payload without any prebuilt out/usb.

    Only repository source/artifacts and a locally compiled first-party
    qmlpatch are used; the daemon is replaced by the fixture stub later.
    """
    payload = usb / "payload"
    for directory in ("bin", "lib", "assets/wifi1b", "systemd",
                      "qml/AppHomeScreen/qml_eco3.0/HomeScreen",
                      "qml/AppHomeScreen/qml_eco3.0/component"):
        (payload / directory).mkdir(parents=True, exist_ok=True)
    (usb / "backup").mkdir(parents=True)
    (usb / "backup/README.txt").write_text("fixture backups\n")
    shutil.copyfile(ROOT.parent / "common/install_io.sh", payload / "lib/install_io.sh")
    shutil.copyfile(ROOT.parent / "common/runtime_cleanup.sh", payload / "lib/runtime_cleanup.sh")
    shutil.copyfile(ROOT / "wifi1b.txt.sample", payload / "wifi1b.txt.sample")
    for name in ("wifi1b-daemon.service", "60-wifi1b-foreground.conf",
                 "60-connman-nodevice.conf"):
        shutil.copyfile(ROOT / "artifacts/systemd" / name, payload / "systemd" / name)
    qml_source = ROOT / "artifacts/qml/AppHomeScreen/qml_eco3.0"
    for rel in ("HomeScreen/Wifi1bPage.qml", "component/Wifi1bTile.qml",
                "component/Wifi1bSwitch.qml", "component/Wifi1bButton.qml"):
        shutil.copyfile(qml_source / rel, payload / "qml/AppHomeScreen/qml_eco3.0" / rel)
    qmlpatch = workdir / "fixture-wifi1b_qmlpatch"
    subprocess.run(["cc", "-O2", "-o", str(qmlpatch),
                    str(ROOT / "src/wifi1b_qmlpatch.c")], check=True, timeout=120)
    shutil.copyfile(qmlpatch, payload / "bin/wifi1b_qmlpatch")
    (payload / "bin/wifi1b_qmlpatch").chmod(0o755)
    (payload / "bin/wifi1b_daemon").write_text("#!/bin/sh\nexit 0\n")
    (payload / "bin/wifi1b_daemon").chmod(0o755)
    (payload / "build-id.txt").write_text("fixture-wifi1b-build\n")
    digests = set()
    for package in ("battery", "wifi", "tweaks"):
        qml_root = ROOT.parent / package / "artifacts/qml/AppHomeScreen/qml_eco3.0"
        if not qml_root.is_dir():
            continue
        for path in qml_root.rglob("*.qml"):
            digests.add(hashlib.md5(path.read_bytes()).hexdigest())
    assert digests, "no canonical QML fixture artifacts found"
    (payload / "accepted-qml.md5").write_text("\n".join(sorted(digests)) + "\n")

STUB_DAEMON = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Fixture stub for the real wifi1b_daemon: never touches the host beyond
 * its arguments.  Fails --import-file (import-failure test), writes a
 * status.json with the build id from the environment, then stays alive so
 * /proc/<pid>/exe checks pass. */
int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--import-file")) return 1;   /* import failure */
        if (!strcmp(argv[i], "--mode")) return 0;          /* one-shot mode */
        if (!strcmp(argv[i], "--usb0-cleanup")) {
            const char *called = getenv("WIFI1B_STUB_USB0_CALLED_FILE");
            if (called) { FILE *f = fopen(called, "w"); if (f) { fputs("called\n", f); fclose(f); } }
            const char *fail = getenv("WIFI1B_STUB_USB0_FAIL");
            printf("WIFI1B_USB0_CLEANUP\nstatus=%s\n", fail ? "FAIL" : "OK");
            return fail ? 1 : 0;
        }
        if (!strcmp(argv[i], "--status")) return 0;
        if (!strcmp(argv[i], "--choose-port")) { printf("{\"ok\":true,\"port\":2224}\n"); return 0; }
    }
    const char *status = NULL;
    for (int i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--status-file")) status = argv[i + 1];
    const char *build = getenv("WIFI1B_STUB_BUILD_ID");
    if (!build) build = "stub";
    if (status) {
        FILE *f = fopen(status, "w");
        if (f) {
            fprintf(f, "{\"ok\":true,\"app\":\"wifi1b\",\"build_id\":\"%s\",\"mode\":\"off\"}\n", build);
            fclose(f);
        }
    }
    for (;;) pause();
    return 0;
}
"""


FAKE = r'''#!/usr/bin/env python3
import json, os, pathlib, sys, hashlib
root = pathlib.Path(os.environ['FIXTURE'])
statefile = root / 'state.json'
state = json.loads(statefile.read_text())
name = pathlib.Path(sys.argv[0]).name
args = sys.argv[1:]
failure = os.environ.get('WIFI1B_FAIL_POINT', '')
with (root / 'commands.log').open('a') as f:
    f.write(name + ' ' + repr(args) + '\n')
def save():
    statefile.write_text(json.dumps(state))
if name == 'md5sum':
    if len(args) == 1 and args[0].endswith('/lib/systemd/system/connman.service'):
        print('58f77230485566943fc8b7a38175034a  ' + args[0])
        sys.exit(0)
    os.execv('/usr/bin/md5sum', ['md5sum', *args])
if name in ('sleep', 'mount', 'sync', 'chown', 'setfattr', 'iptables', 'ip6tables'):
    sys.exit(0)
if name == 'date':
    print('20260101T000000Z' if any(a.startswith('+') for a in args) else 'fixture time')
    sys.exit(0)
if name == 'getfattr':
    print('_')
    sys.exit(0)
if name == 'chsmack':
    sys.exit(0)
if name == 'cp':
    if failure == 'qml' and args[-1].endswith('/HomeScreen/Wifi1bPage.qml'):
        sys.exit(1)
    if failure == 'icons' and '/wifi1b/wifi1b_icon_' in args[-1]:
        sys.exit(1)
    os.execv('/usr/bin/cp', ['cp', *args])
if name == 'pidof':
    if args and args[0] == 'AppHomeScreen':
        print(state.get('home_pid', 1000))
        sys.exit(0)
    sys.exit(1)
if name == 'dbus-send':
    if any('.terminate' in a for a in args):
        state['home_pid'] = None
    if any('.launch' in a for a in args):
        state['serial'] += 1
        state['home_pid'] = 1000 + state['serial']
    save()
    sys.exit(0)
if name == 'systemctl':
    command = args[0] if args else ''
    unit = args[-1] if args else ''
    if failure == 'systemd' and unit == 'wifi1b-daemon.service' and command in ('enable', 'restart', 'start'):
        sys.exit(1)
    if command == 'is-active':
        if unit == 'wifi1b-daemon.service':
            rw = root / 'rw_data/wifi1b'
            rw.mkdir(parents=True, exist_ok=True)
            build = (pathlib.Path(os.environ['FIXTURE_USB']) / 'payload/build-id.txt').read_text().strip()
            (rw / 'status.json').write_text(json.dumps({'build_id': build, 'app': 'wifi1b', 'mode': 'off'}, separators=(',', ':')))
            proc = root / 'proc/1234'
            proc.mkdir(parents=True, exist_ok=True)
            exe = proc / 'exe'
            if exe.exists() or exe.is_symlink():
                exe.unlink()
            exe.symlink_to(rw / 'wifi1b_daemon')
            state['active'][unit] = True
            save()
            sys.exit(0)
        sys.exit(0 if state['active'].get(unit, False) else 3)
    if command == 'is-enabled':
        value = state['enabled'].get(unit, 'disabled')
        print(value)
        sys.exit(0 if value in ('enabled', 'static') else 1)
    if command == 'show':
        print('MainPID=1234')
        sys.exit(0)
    if command == 'enable':
        state['enabled'][unit] = 'enabled-runtime' if '--runtime' in args else 'enabled'
    elif command == 'disable':
        state['enabled'][unit] = 'disabled'
    elif command == 'mask':
        state['enabled'][unit] = 'masked-runtime' if '--runtime' in args else 'masked'
    elif command == 'stop':
        state['active'][unit] = False
    elif command in ('start', 'restart'):
        state['active'][unit] = True
        if unit == 'wifi1b-daemon.service':
            rw = root / 'rw_data/wifi1b'
            rw.mkdir(parents=True, exist_ok=True)
            build = (pathlib.Path(os.environ['FIXTURE_USB']) / 'payload/build-id.txt').read_text().strip()
            (rw / 'status.json').write_text(json.dumps({'build_id': build, 'app': 'wifi1b', 'mode': 'off'}, separators=(',', ':')))
            proc = root / 'proc/1234'
            proc.mkdir(parents=True, exist_ok=True)
            exe = proc / 'exe'
            if exe.exists() or exe.is_symlink():
                exe.unlink()
            exe.symlink_to(rw / 'wifi1b_daemon')
    save()
    sys.exit(0)
sys.exit(1)
'''


class Fixture:
    def __init__(self, directory: Path, tweaks: bool = False, standalone: bool = False):
        self.path = directory
        self.root = directory / "root"
        self.usb = directory / "usb"
        self.rw2 = self.root / "rw_data/wifi1b"
        self.rw1 = self.root / "rw_data/wifi"
        self.qml = self.root / "app/share/AppHomeScreen/qml_eco3.0"
        self.bin = directory / "bin"
        self.standalone = standalone
        self.bin.mkdir(parents=True)
        fake = self.bin / "fake"
        fake.write_text(FAKE)
        fake.chmod(0o755)
        for name in ("systemctl", "sleep", "mount", "sync", "chown", "date", "getfattr",
                     "setfattr", "chsmack", "cp", "pidof", "dbus-send", "iptables",
                     "ip6tables", "md5sum"):
            (self.bin / name).symlink_to(fake)
        for directory in ("etc/systemd/system", "etc/systemd/system/connman.service.d",
                          "lib/systemd/system", "tmp", "proc", "run/wifi1b", "home",
                          "rw_data/wifi1b", "rw_data/wifi", "root/cache",
                          "app/share/AppHomeScreen"):
            (self.root / directory).mkdir(parents=True, exist_ok=True)
        (self.root / "lib/systemd/system/connman.service").write_text(
            "# fixture vendor connman unit\n[Service]\nExecStart=/bin/true\n")
        (self.root / "proc/mounts").write_text(f'device {self.root} ext4 rw 0 0\n')
        (self.root / "tmp/shells").write_text("")
        (self.root / "etc/passwd").write_text("root:x:0:0:root:/root:/bin/sh\ncar:x:0:0:Car User:/rw_data/wifi/home:/bin/sh\n")
        for name in ("passwd.legacy", "passwd.secure"):
            (self.root / "etc" / name).write_text("root:x:0:0:root:/root:/bin/sh\ncar:x:0:0:Car User:/rw_data/wifi/home:/bin/sh\n")
        (self.root / "etc/shadow.Global").write_text("root:*:0:0:99999:7:::\ncar:!:0:0:99999:7:::\n")
        (self.root / "etc/shadow").write_text("root:*:0:0:99999:7:::\ncar:!:0:0:99999:7:::\n")

        # Synthetic fixture QML (no stock/OEM bytes copied).  WiFi-present
        # layouts carry the WiFi marker; standalone uses the Tweaks base.
        qml_assets = {
            "HomeScreen/WifiPage.qml": WIFI / "artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/WifiPage.qml",
            "component/WifiTile.qml": WIFI / "artifacts/qml/AppHomeScreen/qml_eco3.0/component/WifiTile.qml",
            "component/WifiSwitch.qml": WIFI / "artifacts/qml/AppHomeScreen/qml_eco3.0/component/WifiSwitch.qml",
            "component/WifiButton.qml": WIFI / "artifacts/qml/AppHomeScreen/qml_eco3.0/component/WifiButton.qml",
            "HomeScreen/TweaksPage.qml": TWEAKS / "artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/TweaksPage.qml",
            "HomeScreen/RebootPage.qml": TWEAKS / "artifacts/qml/AppHomeScreen/qml_eco3.0/HomeScreen/RebootPage.qml",
            "component/TweaksTile.qml": TWEAKS / "artifacts/qml/AppHomeScreen/qml_eco3.0/component/TweaksTile.qml",
            "component/RebootTile.qml": TWEAKS / "artifacts/qml/AppHomeScreen/qml_eco3.0/component/RebootTile.qml",
        }
        for rel, source in qml_assets.items():
            if standalone and (rel.startswith("HomeScreen/Wifi") or rel.startswith("component/Wifi")):
                continue
            if not standalone and (rel.startswith("HomeScreen/Wifi") or rel.startswith("component/Wifi")):
                pass
            elif not (tweaks or standalone) and ("Tweaks" in rel or "Reboot" in rel):
                continue
            target = self.qml / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, target)
        write_synthetic_qml(self.qml, wifi=not standalone, tweaks=(tweaks or standalone))
        self.wifi_originals = {p: p.read_bytes() for p in self.qml.rglob("*.qml")}
        if not standalone:
            (self.rw1 / "config.conf").write_text("config_version=2\nmode=off\nwlan_on=0\nssh=0\nssh_port=2222\n")
            (self.rw1 / "status.json").write_text('{"build_id":"wifi","mode":"off","wlan_on":false}\n')
            (self.root / "etc/systemd/system/wifi-daemon.service").write_text("[Service]\nExecStart=/bin/true\n")
            (self.rw1 / "wifi_daemon").write_text("#!/bin/sh\nexit 0\n")
            (self.rw1 / "wifi_daemon").chmod(0o755)

        build_payload_fixture(self.usb, self.path)
        accepted = {line.strip() for line in
                    (self.usb / "payload/accepted-qml.md5").read_text().splitlines()
                    if line.strip()}
        accepted.update(hashlib.md5(p.read_bytes()).hexdigest()
                        for p in self.qml.rglob("*.qml"))
        accept_dir = self.path / "accepted"
        accept_dir.mkdir()
        for rel, kind in (("HomeScreen/QHMI_HomeScreenMain_Advanced.qml", "advanced"),
                          ("HomeScreen/QHMI_HomeScreenMain_Wide.qml", "wide"),
                          ("HomeScreen/Screen_HomeScreen_AllMenus.qml", "menu"),
                          ("HomeScreen/Screen_HomeScreen_Submenu_Setup.qml", "menu"),
                          ("component/QCustomizedMenuListAllowEmpty.qml", "custom")):
            temporary = accept_dir / Path(rel).name
            shutil.copyfile(self.qml / rel, temporary)
            subprocess.run([str(self.usb / "payload/bin/wifi1b_qmlpatch"), kind,
                            str(temporary)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            accepted.add(hashlib.md5(temporary.read_bytes()).hexdigest())
        shutil.rmtree(accept_dir)
        (self.usb / "payload/accepted-qml.md5").write_text(
            "\n".join(sorted(accepted)) + "\n")
        assert hashlib.md5((self.qml / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml").read_bytes()).hexdigest() in accepted
        self.manifest()
        self.install = self.path / "run_stuff.sh"
        self.restore = self.path / "run_stuff_restore.sh"
        for src, dst in ((ROOT / "scripts/run_stuff1b.sh", self.install),
                         (ROOT / "scripts/run_stuff_restore1b.sh", self.restore)):
            text = src.read_text()
            # Rewrite absolute host paths into the fixture.  Use placeholders
            # so the fixture path itself is not rewritten repeatedly.
            for prefix, target in [("/root", self.root / "root"), ("/rw_data", self.root / "rw_data"),
                                   ("/home", self.root / "home"), ("/app", self.root / "app"),
                                   ("/etc", self.root / "etc"), ("/proc", self.root / "proc"),
                                   ("/tmp", self.root / "tmp"), ("/run", self.root / "run")]:
                text = re.sub(r"(?<![A-Za-z0-9_])" + re.escape(prefix) + r"(?=/|[^A-Za-z0-9_]|$)",
                              lambda _: "@@" + str(target.relative_to(self.path)) + "@@", text)
            text = re.sub(r"@@(.*?)@@", lambda m: str(self.path / m.group(1)), text)
            dst.write_text(text)
            dst.chmod(0o755)
        self.env = dict(os.environ,
                        INSTALL_IO_ROOT=str(self.root), INSTALL_IO_MOUNTS=str(self.root / 'proc/mounts'),
                        FIXTURE=str(self.root), FIXTURE_USB=str(self.usb),
                        WIFI1B_PATH=str(self.bin) + ":/usr/bin:/bin",
                        USB_DIR=str(self.usb),
                        WIFI1B_RW_DIR=str(self.rw2),
                        WIFI1B_QML_ROOT=str(self.qml.parent),
                        CLEANUP_PROC_ROOT=str(self.root / "proc"),
                        WIFI1B_SERVICE=str(self.root / "etc/systemd/system/wifi1b-daemon.service"),
                        WIFI1B_CONNMAN_UNIT=str(self.root / "lib/systemd/system/connman.service"),
                        WIFI1B_CONNMAN_DROPIN=str(
                            self.root / "etc/systemd/system/connman.service.d/60-scourge-nodevice.conf"))
        state = {"active": {}, "enabled": {"wifi-daemon.service": "enabled"},
                 "home_pid": 1000, "serial": 1000}
        (self.root / "state.json").write_text(json.dumps(state))
        self.stub_daemon()
        actual_adv = hashlib.md5((self.qml / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml").read_bytes()).hexdigest()
        ledger = (self.usb / "payload/accepted-qml.md5").read_text().split()
        assert actual_adv in ledger, (actual_adv, ledger[:5])

    def run(self, script: Path, fail_point: str = "", expect_fail: bool = False):
        env = dict(self.env)
        if fail_point:
            env["WIFI1B_FAIL_POINT"] = fail_point
        result = subprocess.run(["sh", str(script)], env=env, capture_output=True,
                                text=True, timeout=90)
        report = (self.usb / "run_stuff_result.txt").read_text()
        failed = result.returncode != 0 or "status=FAIL" in report
        if failed and not expect_fail:
            status = (self.rw2 / "status.json").read_text() if (self.rw2 / "status.json").exists() else "<no status>"
            build = (self.usb / "payload/build-id.txt").read_text() if (self.usb / "payload/build-id.txt").exists() else "<no build>"
            raise AssertionError((report, result.stdout, result.stderr, status, build,
                                  (self.root / 'commands.log').read_text(),
                                  [str(x) for x in self.root.rglob('status.json')],
                                  (self.rw2 / "install.log").read_text() if (self.rw2 / "install.log").exists() else "",
                                  (self.rw2 / "restore.log").read_text() if (self.rw2 / "restore.log").exists() else ""))
        if expect_fail and not failed:
            raise AssertionError(("expected failure but install succeeded", report))
        return report

    def kill_manual_daemon(self):
        """Stop fixture-owned stub daemons by exact executable path.

        Deliberately no host-wide pkill: only PIDs whose /proc/<pid>/exe
        resolves to this fixture's private daemon copy are signalled, and
        only their own process group is terminated.
        """
        wanted = str((self.rw2 / "wifi1b_daemon").resolve())
        own_group = os.getpgrp()
        for proc in Path("/proc").iterdir():
            if not proc.name.isdigit():
                continue
            try:
                exe = os.readlink(proc / "exe")
            except OSError:
                continue
            if exe not in (wanted, wanted + " (deleted)"):
                continue
            pid = int(proc.name)
            if pid == os.getpid():
                continue
            try:
                pgid = os.getpgid(pid)
            except OSError:
                pgid = pid
            try:
                if pgid != own_group and pgid == pid:
                    os.killpg(pgid, 15)
                else:
                    os.kill(pid, 15)
            except OSError:
                pass
            deadline = time.time() + 5
            while time.time() < deadline:
                if not proc.exists():
                    break
                try:
                    os.kill(pid, 0)
                except OSError:
                    break
                time.sleep(0.05)
            else:
                try:
                    os.kill(pid, 9)
                except OSError:
                    pass

    def manifest(self):
        lines = []
        for p in sorted((self.usb / "payload").rglob("*")):
            if p.is_file():
                lines.append(f"{hashlib.md5(p.read_bytes()).hexdigest()}  {p.relative_to(self.usb)}\n")
        (self.usb / "manifest.md5").write_text("".join(lines))

    def stub_daemon(self):
        """Replace the payload daemon with a compiled stub so no real daemon
        ever runs on the host.  The stub is the file the installer copies,
        starts and md5-verifies, so every check path stays honest."""
        stub_src = self.path / "wifi1b_stub.c"
        stub_src.write_text(STUB_DAEMON)
        stub_bin = self.path / "wifi1b_stub"
        subprocess.run(["gcc", "-O2", "-static", "-o", str(stub_bin), str(stub_src)],
                      check=True, timeout=60)
        target = self.usb / "payload/bin/wifi1b_daemon"
        shutil.copyfile(stub_bin, target)
        target.chmod(0o755)
        build = (self.usb / "payload/build-id.txt").read_text().strip()
        self.env["WIFI1B_STUB_BUILD_ID"] = build
        self.manifest()


def exercise(tweaks: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="wifi1b-side-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=tweaks)
        report = fixture.run(fixture.install)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        adv = (fixture.qml / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml").read_text()
        assert "WIFI_PAYLOAD_260128" in adv and "WIFI1B_PAYLOAD_260128" in adv
        assert 'id: wifiPage' in adv and 'id: wifi1bPage' in adv
        assert adv.count("WifiTile {") == 3 and adv.count("Wifi1bTile {") == 3
        if tweaks:
            assert "TWEAKS_PAYLOAD_260128" in adv
            assert adv.count("TweaksTile {") == 3
        expected = 1 + sum(f'id: {name}Tile1' in adv for name in ('hello','battery','tweaks','reboot'))
        assert f"firstEmptySlot({expected})" in adv
        for name in ("HomeScreen/WifiPage.qml", "component/WifiTile.qml",
                     "component/WifiSwitch.qml", "component/WifiButton.qml"):
            assert (fixture.qml / name).read_bytes() == fixture.wifi_originals[fixture.qml / name], name
        if tweaks:
            for name in ("HomeScreen/TweaksPage.qml", "component/TweaksTile.qml"):
                assert (fixture.qml / name).read_bytes() == fixture.wifi_originals[fixture.qml / name], name
        assert (fixture.rw1 / "config.conf").read_text().startswith("config_version=2")
        assert (fixture.rw1 / "status.json").read_text().find('"mode":"off"') >= 0
        assert (fixture.root / "etc/systemd/system/wifi-daemon.service").read_text().find("/bin/true") >= 0
        cfg = (fixture.rw2 / "config.conf").read_text()
        assert "mode=off" in cfg and "wlan_on=0" in cfg and "ssh=0" in cfg and "ssh_port=2224" in cfg
        assert "carplay_ssid=IONIQ5-CarPlay-B" in cfg
        restore_point = (fixture.rw2 / 'backup/latest').resolve()
        for _ in range(4):
            fixture.run(fixture.install)
        assert (fixture.rw2 / 'backup/latest').resolve() == restore_point
        assert (fixture.rw2 / 'backup/latest').exists()
        # Repeated power-on re-installs must not accumulate snapshots: the
        # installer prunes to the restore point + two newest, plus the new one.
        snaps = [p for p in (fixture.rw2 / 'backup').glob('20*') if p.is_dir()]
        assert len(snaps) <= 4, sorted(p.name for p in snaps)
        assert (fixture.rw2 / 'config.conf').read_text() == cfg
        passwd = (fixture.root / "etc/passwd").read_text()
        assert "wifi1b:" in passwd and "car:" in passwd
        for name in ("HomeScreen/Wifi1bPage.qml", "component/Wifi1bTile.qml",
                     "component/Wifi1bSwitch.qml", "component/Wifi1bButton.qml"):
            assert (fixture.qml / name).exists(), name
        assert (fixture.root / "etc/systemd/system/wifi1b-daemon.service").exists()
        dropin = fixture.root / 'etc/systemd/system/carplay_mdnsd.service.d/60-wifi1b-foreground.conf'
        assert dropin.read_bytes() == (fixture.usb / 'payload/systemd/60-wifi1b-foreground.conf').read_bytes()

        report = fixture.run(fixture.restore)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        for path, original in fixture.wifi_originals.items():
            assert path.read_bytes() == original, path
        assert "WIFI1B_PAYLOAD_260128" not in (fixture.qml / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml").read_text()
        for name in ("HomeScreen/Wifi1bPage.qml", "component/Wifi1bTile.qml",
                     "component/Wifi1bSwitch.qml", "component/Wifi1bButton.qml"):
            assert not (fixture.qml / name).exists(), name
        assert "wifi1b:" not in (fixture.root / "etc/passwd").read_text()
        assert "car:" in (fixture.root / "etc/passwd").read_text()
        assert (fixture.rw1 / "config.conf").exists()
        assert (fixture.rw1 / "status.json").exists()
        assert (fixture.root / "etc/systemd/system/wifi-daemon.service").exists()
        assert not (fixture.root / "etc/systemd/system/wifi1b-daemon.service").exists()
        assert not (fixture.rw2 / "bin/wifi1b_qmlpatch").exists()
        assert not dropin.exists()

def exercise_hardening() -> None:
    """The two on-car install failures must be non-fatal in Wifi1b."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-hard-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        # A wifi.txt at the stick root makes the installer run the daemon's
        # --import-file; the stub fails it.  The install must still succeed:
        # on 2026-11-10 this exact failure aborted the whole WiFi install.
        (fixture.usb / "wifi.txt").write_text("ssid=FixtureNet\npsk=fixture-password\n")
        report = fixture.run(fixture.install)
        assert "wifi1b_import=FAIL" in report, report
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        assert (fixture.qml / "HomeScreen/Wifi1bPage.qml").exists()
        assert (fixture.root / "etc/systemd/system/wifi1b-daemon.service").exists()

    with tempfile.TemporaryDirectory(prefix="wifi1b-sysfail-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        try:
            report = fixture.run(fixture.install, fail_point="systemd")
            # systemd enable/restart failed; the manual start must have taken over.
            assert "daemon=manual-ok" in report, report
            assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
            adv = (fixture.qml / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml").read_text()
            assert "WIFI1B_PAYLOAD_260128" in adv and "id: wifi1bPage" in adv
            status = (fixture.rw2 / "status.json").read_text()
            assert "\"app\":\"wifi1b\"" in status
        finally:
            fixture.kill_manual_daemon()


exercise(False)
exercise(True)
exercise_hardening()
for failure in ('qml', 'icons', 'unknown'):
    with tempfile.TemporaryDirectory(prefix='wifi1b-failure-', dir='/tmp/opencode') as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        advanced = fixture.qml / 'HomeScreen/QHMI_HomeScreenMain_Advanced.qml'
        inode = advanced.stat().st_ino
        if failure == 'unknown':
            advanced.write_bytes(advanced.read_bytes() + b'\n// unknown owner change\n')
            before = advanced.read_bytes()
            fixture.run(fixture.install, expect_fail=True)
            assert advanced.read_bytes() == before
        elif failure == 'qml':
            fixture.run(fixture.install, fail_point='qml', expect_fail=True)
            for path, original in fixture.wifi_originals.items():
                assert path.read_bytes() == original, path
            assert not (fixture.root / 'etc/systemd/system/carplay_mdnsd.service.d/60-wifi1b-foreground.conf').exists()
        else:
            fixture.run(fixture.install, fail_point='icons')
            assert 'WIFI1B_PAYLOAD_260128' in advanced.read_text()
        assert advanced.stat().st_ino == inode
print('PASS: Wifi1b reinstall retains restore point/settings; unknown QML fails closed; core-copy rollback and optional icons preserve live inodes')
print("PASS: Wifi1b installs beside WiFi/Tweaks, survives import and systemd failures, and restores cleanly")


def exercise_standalone() -> None:
    """Wifi1b must install and restore with no WiFi app present at all."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-standalone-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), standalone=True)
        # Owner key material must survive install and restore even though the
        # payload ships no key at the stick root.
        (fixture.rw2 / "ssh").mkdir(parents=True, exist_ok=True)
        (fixture.rw2 / "ssh/authorized_keys").write_text("owner-key\n")
        report = fixture.run(fixture.install)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        assert (fixture.rw2 / "ssh/authorized_keys").read_text() == "owner-key\n"
        assert "layout=standalone" in report, report
        adv = (fixture.qml / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml").read_text()
        assert "WIFI1B_PAYLOAD_260128" in adv and "TWEAKS_PAYLOAD_260128" in adv
        assert "WIFI_PAYLOAD_260128" not in adv
        assert "id: wifi1bPage" in adv and adv.count("Wifi1bTile {") == 3
        assert "WifiTile {" not in adv
        for name in ("HomeScreen/Wifi1bPage.qml", "component/Wifi1bTile.qml"):
            assert (fixture.qml / name).exists(), name
        assert (fixture.root / "etc/systemd/system/wifi1b-daemon.service").exists()
        # Idempotent reinstall keeps the restore point and settings.
        cfg = (fixture.rw2 / "config.conf").read_text()
        restore_point = (fixture.rw2 / "backup/latest").resolve()
        fixture.run(fixture.install)
        assert (fixture.rw2 / "backup/latest").resolve() == restore_point
        assert (fixture.rw2 / "config.conf").read_text() == cfg
        report = fixture.run(fixture.restore)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        for path, original in fixture.wifi_originals.items():
            assert path.read_bytes() == original, path
        assert "WIFI1B_PAYLOAD_260128" not in (fixture.qml / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml").read_text()
        for name in ("HomeScreen/Wifi1bPage.qml", "component/Wifi1bTile.qml"):
            assert not (fixture.qml / name).exists(), name
        assert not (fixture.root / "etc/systemd/system/wifi1b-daemon.service").exists()
        assert (fixture.rw2 / "ssh/authorized_keys").read_text() == "owner-key\n"


exercise_standalone()
print("PASS: Wifi1b installs and restores standalone (no WiFi app), anchored on Tweaks/Battery/Reboot")


def exercise_bounded_logs_first() -> None:
    """An unsafe product destination is refused before any write, including logs."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-logs-first-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        (fixture.rw2 / "daemon.log").write_text("fill /rw_data\n")
        victim = fixture.path / "victim"
        victim.mkdir()
        (fixture.rw2 / "backup").symlink_to(victim)
        report = fixture.run(fixture.install, expect_fail=True)
        assert (fixture.rw2 / "daemon.log").read_text() == "fill /rw_data\n", \
            "a write occurred before unsafe destination refusal"
        assert not (victim / "daemon.log").exists()
        assert "unsafe " in report and "destination" in report, report


exercise_bounded_logs_first()
print("PASS: direct Wifi1b payload refuses symlinked product destinations before any write, including bounded log truncation")


def exercise_product_symlink() -> None:
    """No installer write/delete traverses a symlinked product subdirectory."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-home-link-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        victim = fixture.path / "ssh-victim"
        victim.mkdir()
        key = victim / "authorized_keys"
        key.write_text("owner-key\n")
        (fixture.rw2 / "home").symlink_to(victim)
        assert (fixture.rw2 / "home").is_symlink()
        fixture.run(fixture.install, expect_fail=True)
        assert key.read_text() == "owner-key\n", "wrote through a symlinked home directory"
        assert not (victim / ".ssh").exists() or not (victim / ".ssh/authorized_keys").exists()


exercise_product_symlink()
print("PASS: direct Wifi1b payload refuses symlinked home/product subtrees without writing through them")


def exercise_account_symlink() -> None:
    """The firmware's /etc/passwd symlink is preserved; only our entry is removed."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-account-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        target = fixture.root / "tmp/passwd"
        target.write_text("root:x:0:0:root:/root:/bin/sh\ncar:x:0:0:Car User:/rw_data/wifi/home:/bin/sh\n")
        passwd = fixture.root / "etc/passwd"
        passwd.unlink()
        passwd.symlink_to(target)
        fixture.run(fixture.install)
        assert passwd.is_symlink() and os.readlink(passwd) == str(target)
        assert "wifi1b:" in target.read_text()
        report = fixture.run(fixture.restore)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        assert passwd.is_symlink() and os.readlink(passwd) == str(target), "account symlink replaced"
        text = target.read_text()
        assert "wifi1b:" not in text and "car:" in text, text


exercise_account_symlink()
print("PASS: firmware account symlink preserved while only the owned wifi1b account entry is removed")


def exercise_stick_inputs() -> None:
    """All three owner stick inputs are forwarded/imported without byte loss."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-inputs-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        wifi = b"ssid=OwnerNet\npsk=owner-pass\n"
        iphone = b"ssid=OwnerPhone\npsk=owner-phone-pass\n"
        keys = b"owner-key-one\nowner-key-two\n"
        for name, data in (("wifi.txt", wifi), ("iphone_wifi.txt", iphone),
                           ("authorized_keys", keys)):
            (fixture.usb / name).write_bytes(data)
        fixture.run(fixture.install)
        assert (fixture.rw2 / "wifi.txt").read_bytes() == iphone, "iphone_wifi.txt not preferred/imported"
        assert (fixture.rw2 / "ssh/authorized_keys").read_bytes() == keys
        assert (fixture.rw2 / "home/.ssh/authorized_keys").read_bytes() == keys
        fixture.run(fixture.restore)
        for name, data in (("wifi.txt", wifi), ("iphone_wifi.txt", iphone),
                           ("authorized_keys", keys)):
            assert (fixture.usb / name).read_bytes() == data, f"owner input changed: {name}"


exercise_stick_inputs()
print("PASS: wifi.txt/iphone_wifi.txt/authorized_keys are forwarded, imported and retained byte-exactly")


def exercise_symlink_destinations() -> None:
    """Every QML/asset/service destination is checked before any write."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-qml-link-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        victim = fixture.path / "victim-qml"
        shutil.move(str(fixture.qml), str(victim))
        fixture.qml.symlink_to(victim, target_is_directory=True)
        advanced = victim / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
        before = advanced.read_bytes()
        report = fixture.run(fixture.install, expect_fail=True)
        assert fixture.qml.is_symlink() and advanced.read_bytes() == before
        assert "unsafe " in report and "destination" in report, report

    with tempfile.TemporaryDirectory(prefix="wifi1b-asset-link-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        victim = fixture.path / "victim-assets"
        victim.mkdir()
        asset = fixture.root / "app/share/AppHomeScreen/wifi1b"
        asset.symlink_to(victim, target_is_directory=True)
        report = fixture.run(fixture.install, expect_fail=True)
        assert not list(victim.iterdir()) and "unsafe " in report and "destination" in report, (
            report, [p.name for p in victim.iterdir()])

    with tempfile.TemporaryDirectory(prefix="wifi1b-service-link-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        victim = fixture.path / "victim-systemd"
        shutil.move(str(fixture.root / "etc/systemd/system"), str(victim))
        (fixture.root / "etc/systemd/system").symlink_to(victim, target_is_directory=True)
        before = {p.name: p.read_bytes() for p in victim.iterdir() if p.is_file()}
        report = fixture.run(fixture.install, expect_fail=True)
        assert {p.name: p.read_bytes() for p in victim.iterdir() if p.is_file()} == before
        assert "unsafe " in report and "destination" in report

    with tempfile.TemporaryDirectory(prefix="wifi1b-dropin-link-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        dropin = fixture.root / "etc/systemd/system/carplay_mdnsd.service.d/60-wifi1b-foreground.conf"
        dropin.parent.mkdir(parents=True)
        victim = fixture.path / "victim-dropin"
        victim.write_text("owner dropin\n")
        dropin.symlink_to(victim)
        report = fixture.run(fixture.install, expect_fail=True)
        assert victim.read_text() == "owner dropin\n"
        assert "unsafe " in report and "destination" in report

    with tempfile.TemporaryDirectory(prefix="wifi1b-restore-qml-link-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        victim = fixture.path / "victim-restore-qml"
        shutil.move(str(fixture.qml), str(victim))
        fixture.qml.symlink_to(victim, target_is_directory=True)
        advanced = victim / "HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
        before = advanced.read_bytes()
        report = fixture.run(fixture.restore, expect_fail=True)
        assert advanced.read_bytes() == before
        assert "unsafe " in report and "destination" in report, report


exercise_symlink_destinations()
print("PASS: QML/assets/service/restore symlink destinations are refused before writes with status FAIL")


def exercise_cleanup_failures() -> None:
    """Actual account/QML/runtime cleanup failures must report FAIL, not OK."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-account-fail-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        (fixture.root / "etc/passwd").chmod(0o444)
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "status=FAIL" in report, report
        assert "wifi1b:" in (fixture.root / "etc/passwd").read_text()
        assert (fixture.rw2 / "backup/latest").is_dir()

    for target, label in (("Wifi1bPage.qml", "qml"), ("config.conf", "runtime")):
        with tempfile.TemporaryDirectory(prefix=f"wifi1b-rm-{label}-", dir="/tmp/opencode") as directory:
            fixture = Fixture(Path(directory), tweaks=True)
            fixture.run(fixture.install)
            fake = fixture.path / "fault-rm"
            fake.mkdir()
            real_rm = shutil.which("rm")
            (fake / "rm").write_text(
                "#!/bin/sh\n"
                f"case \"$*\" in *\"{target}\"*) exit 1;; esac\n"
                f"exec '{real_rm}' \"$@\"\n")
            (fake / "rm").chmod(0o755)
            fixture.env["WIFI1B_PATH"] = str(fake) + ":" + fixture.env["WIFI1B_PATH"]
            report = fixture.run(fixture.restore, expect_fail=True)
            assert "status=FAIL" in report, (label, report)
            assert (fixture.rw2 / "backup/latest").is_dir()


exercise_cleanup_failures()
print("PASS: unreadable account, QML removal and runtime removal failures report FAIL and preserve backups")


def exercise_current_mask_restore() -> None:
    """A known /dev/null mask is removed only to restore a regular transaction unit."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-current-mask-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        service = fixture.root / "etc/systemd/system/wifi1b-daemon.service"
        original = b"# owner service\n[Service]\nExecStart=/bin/true\n"
        service.write_bytes(original)
        service.chmod(0o600)
        statefile = fixture.root / "state.json"
        state = json.loads(statefile.read_text())
        state["enabled"]["wifi1b-daemon.service"] = "enabled"
        state["active"]["wifi1b-daemon.service"] = False
        statefile.write_text(json.dumps(state))
        fixture.run(fixture.install)
        service.unlink()
        service.symlink_to("/dev/null")
        report = fixture.run(fixture.restore)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        assert not service.is_symlink() and service.read_bytes() == original
        assert (service.stat().st_mode & 0o777) == 0o600


exercise_current_mask_restore()
print("PASS: current /dev/null service mask is removed only for a verified regular original unit; bytes/mode restored")


def exercise_service_restore() -> None:
    """A pre-existing service file and enablement are restored from the
    install transaction; a foreign-modified service is never removed."""
    original = b"# owner service\n[Service]\nExecStart=/bin/true\n"
    for enabled in ("enabled", "enabled-runtime", "disabled", "masked"):
        with tempfile.TemporaryDirectory(prefix="wifi1b-svc-", dir="/tmp/opencode") as directory:
            fixture = Fixture(Path(directory), tweaks=True)
            service = fixture.root / "etc/systemd/system/wifi1b-daemon.service"
            if enabled == "masked":
                service.symlink_to("/dev/null")
            else:
                service.write_bytes(original)
                service.chmod(0o600)
            statefile = fixture.root / "state.json"
            state = json.loads(statefile.read_text())
            state["enabled"]["wifi1b-daemon.service"] = enabled
            state["active"]["wifi1b-daemon.service"] = False
            statefile.write_text(json.dumps(state))
            report = fixture.run(fixture.install)
            assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
            assert enabled == "masked" or service.read_bytes() != original, \
                "install should have staged its own unit"
            report = fixture.run(fixture.restore)
            assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
            if enabled == "masked":
                assert service.is_symlink() and os.readlink(service) == "/dev/null", \
                    "original mask was not restored"
            else:
                assert service.read_bytes() == original, "pre-existing service bytes not restored"
                assert (service.stat().st_mode & 0o777) == 0o600, \
                    f"pre-existing service mode changed: {oct(service.stat().st_mode & 0o777)}"
            state = json.loads(statefile.read_text())
            assert state["enabled"]["wifi1b-daemon.service"] == enabled, state
    with tempfile.TemporaryDirectory(prefix="wifi1b-svc-active-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        service = fixture.root / "etc/systemd/system/wifi1b-daemon.service"
        service.write_bytes(original)
        statefile = fixture.root / "state.json"
        state = json.loads(statefile.read_text())
        state["enabled"]["wifi1b-daemon.service"] = "enabled"
        state["active"]["wifi1b-daemon.service"] = True
        statefile.write_text(json.dumps(state))
        report = fixture.run(fixture.install)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        report = fixture.run(fixture.restore)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        assert service.read_bytes() == original
        state = json.loads(statefile.read_text())
        assert state["active"]["wifi1b-daemon.service"] is True, state
    with tempfile.TemporaryDirectory(prefix="wifi1b-svc-fail-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        service = fixture.root / "etc/systemd/system/wifi1b-daemon.service"
        service.write_bytes(original)
        statefile = fixture.root / "state.json"
        state = json.loads(statefile.read_text())
        state["enabled"]["wifi1b-daemon.service"] = "enabled"
        state["active"]["wifi1b-daemon.service"] = True
        statefile.write_text(json.dumps(state))
        fixture.run(fixture.install)
        report = fixture.run(fixture.restore, fail_point="systemd", expect_fail=True)
        assert "status=FAIL" in report, report
    with tempfile.TemporaryDirectory(prefix="wifi1b-svc-foreign-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        report = fixture.run(fixture.install)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        service = fixture.root / "etc/systemd/system/wifi1b-daemon.service"
        foreign = b"# owner changed this after install\n[Service]\nExecStart=/bin/true\n"
        service.write_bytes(foreign)
        statefile = fixture.root / "state.json"
        state = json.loads(statefile.read_text())
        state["enabled"]["wifi1b-daemon.service"] = "enabled"
        statefile.write_text(json.dumps(state))
        commands = fixture.root / "commands.log"
        commands.write_text("")
        ignored = {"restore.log", "restore-result.txt", "result.txt"}
        before = {str(p.relative_to(fixture.rw2)): p.read_bytes()
                  for p in fixture.rw2.rglob("*") if p.is_file() and p.name not in ignored}
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "status=FAIL" in report and "error=foreign_service" in report, report
        assert service.read_bytes() == foreign, "foreign-modified service was overwritten/removed"
        assert "systemctl ['stop', 'wifi1b-daemon.service']" not in commands.read_text()
        assert "systemctl ['disable', 'wifi1b-daemon.service']" not in commands.read_text()
        after = {str(p.relative_to(fixture.rw2)): p.read_bytes()
                 for p in fixture.rw2.rglob("*") if p.is_file() and p.name not in ignored}
        assert after == before, "foreign-service restore changed runtime/credentials/backups"
        assert "wifi1b:" in (fixture.root / "etc/passwd").read_text()
        state = json.loads(statefile.read_text())
        assert state["enabled"]["wifi1b-daemon.service"] == "enabled", (
            state,
            (fixture.rw2 / "backup/latest/transaction/service.state").read_text(),
            (fixture.rw2 / "restore.log").read_text())


exercise_service_restore()
print("PASS: pre-existing service bytes and enabled/disabled/masked/active state restored; restore failure reported; foreign-modified service preserves runtime and FAILs")

V21_CONNMAN = """# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
# SCOURGE_CONNMAN_NODEVICE=1
# Keep ConnMan away from the telematics modem interfaces.  The vendor unit
# already excludes rmnet_usb1..7 and ncm0; rmnet_usb0 carries the primary
# BlueLink/nav data route and must not be reconfigured by ConnMan when the
# WiFi app starts it for client or AP mode.
[Service]
ExecStart=
ExecStart=/usr/sbin/connmand -n --nodnsproxy --nobacktrace --nodevice=rmnet_usb0,rmnet_usb1,rmnet_usb2,rmnet_usb3,rmnet_usb4,rmnet_usb5,rmnet_usb6,rmnet_usb7,wwan0,usb0,dbg0,dbg1,dbg2,can0,eth1,eth2,ncmF0,usb1,eth0,tem0,br0,br0:0,ncm0
"""

with tempfile.TemporaryDirectory(prefix="wifi1b-connman-", dir="/tmp/opencode") as directory:
    fixture = Fixture(Path(directory), tweaks=True)
    dropin = (fixture.root /
              "etc/systemd/system/connman.service.d/60-scourge-nodevice.conf")
    payload_dropin = fixture.usb / "payload/systemd/60-connman-nodevice.conf"
    report = fixture.run(fixture.install)
    assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
    assert dropin.read_bytes() == payload_dropin.read_bytes()
    dropin.write_text(V21_CONNMAN)
    report = fixture.run(fixture.install)
    assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
    assert dropin.read_bytes() == payload_dropin.read_bytes(), "known V21 drop-in not upgraded"
    dropin.write_text("# owner changed this after install\n[Service]\nExecStart=/bin/true\n")
    fixture.run(fixture.install, expect_fail=True)
print("PASS: known V21 ConnMan drop-in upgrades; unknown owner override still refused")

for prior in ('identical', 'unknown'):
    with tempfile.TemporaryDirectory(prefix='wifi1b-mdns-', dir='/tmp/opencode') as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        dropin = fixture.root / 'etc/systemd/system/carplay_mdnsd.service.d/60-wifi1b-foreground.conf'
        dropin.parent.mkdir(parents=True)
        original = ((fixture.usb / 'payload/systemd/60-wifi1b-foreground.conf').read_bytes()
                    if prior == 'identical' else b'# owner override\n[Service]\nRestart=no\n')
        dropin.write_bytes(original)
        fixture.run(fixture.install, expect_fail=prior == 'unknown')
        if prior == 'identical':
            fixture.run(fixture.restore)
        assert dropin.read_bytes() == original
print('PASS: mDNS override install/rollback/removal and pre-existing override preservation')


def _install_refused_leaf(prepare, label):
    with tempfile.TemporaryDirectory(prefix=f"wifi1b-leaf-{label}-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        victim = fixture.path / f"victim-{label}"
        prepare(fixture, victim)
        report = fixture.run(fixture.install, expect_fail=True)
        assert "status=FAIL" in report and "unsafe " in report and "destination" in report, (label, report)
        if label == "run-parent":
            assert (victim / "active").read_text() == "owner-active\n"
        elif label == "shells":
            assert victim.read_text() == "owner shells\n"
        elif label == "wpa-conf":
            assert victim.read_text() == "owner-wpa\n"
        else:
            assert not victim.exists(), f"{label} victim was created"
        return fixture


def exercise_install_leaf_destinations() -> None:
    """Every checked install leaf refuses a symlink before writing through it."""
    cases = []
    cases.append(("config", lambda f, v: (f.rw2 / "config.conf").symlink_to(v)))
    cases.append(("ssh-key", lambda f, v: (
        (f.rw2 / "ssh").mkdir(parents=True, exist_ok=True),
        (f.rw2 / "ssh/authorized_keys").symlink_to(v))))
    cases.append(("home-key", lambda f, v: (
        (f.rw2 / "home/.ssh").mkdir(parents=True, exist_ok=True),
        (f.rw2 / "home/.ssh/authorized_keys").symlink_to(v))))
    cases.append(("shells", lambda f, v: (
        v.write_text("owner shells\n"),
        (f.root / "tmp/shells").unlink(),
        (f.root / "tmp/shells").symlink_to(v))))
    cases.append(("run-parent", lambda f, v: (
        v.mkdir(),
        (v / "active").write_text("owner-active\n"),
        shutil.rmtree(f.root / "run/wifi1b"),
        (f.root / "run/wifi1b").symlink_to(v, target_is_directory=True))))
    cases.append(("backup-current", lambda f, v: (
        (f.usb / "backup_current/AppHomeScreen/qml_eco3.0/HomeScreen").mkdir(parents=True),
        (f.usb / "backup_current/AppHomeScreen/qml_eco3.0/HomeScreen/"
         "QHMI_HomeScreenMain_Advanced.qml").symlink_to(v))))
    cases.append(("qml-before", lambda f, v: (
        (f.usb / "qml_before/HomeScreen").mkdir(parents=True),
        (f.usb / "qml_before/HomeScreen/QHMI_HomeScreenMain_Advanced.qml").symlink_to(v))))
    cases.append(("account-marker", lambda f, v: (
        (f.rw2 / "backup/accounts").mkdir(parents=True, exist_ok=True),
        (f.rw2 / "backup/accounts/passwd").symlink_to(v))))
    cases.append(("wpa-conf", lambda f, v: (
        v.write_text("owner-wpa\n"),
        (f.rw2 / "wpa_supplicant.conf").symlink_to(v))))

    def _leaf_link(rel):
        def prepare(f, v):
            target = f.rw2 / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(v)
        return prepare

    for rel in ("daemon.log", "wpa_supplicant.log", "wpa_supplicant.pid",
                "dhcp.leases", "dhcpd.pid", "dhcpd.log", "client.route",
                "client.owner", "client.lineage", "usb0.owner", "scan.json", "bt.state",
                "ssh.bound", "home.json", "apply.result", "ap.station-ip",
                "ssh.log", "daemon.lock", "ssh/dropbear.pid",
                "ssh/dropbear_ed25519_host_key",
                "ssh/dropbear_rsa_host_key"):
        cases.append((rel.replace("/", "-").replace(".", "-"), _leaf_link(rel)))
    cases.append(("usb0-helper", _leaf_link("bin/opi-usb0-up.sh")))
    def rule_link(f, v):
        rule = f.root / "run/udev/rules.d/99-opi-usb0.rules"
        rule.parent.mkdir(parents=True)
        rule.symlink_to(v)
    cases.append(("usb0-rule", rule_link))
    for label, prepare in cases:
        _install_refused_leaf(prepare, label)


exercise_install_leaf_destinations()
print("PASS: install refuses symlinked config, key, SSH-home, shells, run, backup, QML and account-marker leaves before writes")


def exercise_restore_leaf_destinations() -> None:
    """Restore refuses symlinked run/VBT/auth/QML-legacy destinations."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-restore-run-leaf-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        victim = fixture.path / "victim-run-restore"
        victim.mkdir()
        (victim / "active").write_text("owner-active\n")
        shutil.rmtree(fixture.root / "run/wifi1b")
        (fixture.root / "run/wifi1b").symlink_to(victim, target_is_directory=True)
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "unsafe " in report and "destination" in report, report
        assert (victim / "active").read_text() == "owner-active\n"

    with tempfile.TemporaryDirectory(prefix="wifi1b-restore-vbt-leaf-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        vbt_dir = fixture.root / "app/data/vbt"
        vbt_dir.mkdir(parents=True, exist_ok=True)
        victim = fixture.path / "victim-vbt-restore"
        victim.write_text("owner-vbt\n")
        (vbt_dir / "VBTConfig.json").symlink_to(victim)
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "unsafe " in report and "destination" in report, report
        assert victim.read_text() == "owner-vbt\n"

    with tempfile.TemporaryDirectory(prefix="wifi1b-restore-auth-leaf-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        victim = fixture.path / "victim-auth-restore"
        victim.write_text("owner-key\n")
        key = fixture.rw2 / "ssh/authorized_keys"
        if key.exists() or key.is_symlink():
            key.unlink()
        key.symlink_to(victim)
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "unsafe " in report and "destination" in report, report
        assert victim.read_text() == "owner-key\n"

    with tempfile.TemporaryDirectory(prefix="wifi1b-restore-legacy-qml-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        latest = (fixture.rw2 / "backup/latest").resolve()
        (latest / "transaction/900").write_text("owner legacy\n")
        with (latest / "transaction/files").open("a") as ledger:
            ledger.write(f"900|1|{fixture.qml.parent}/wifi/legacy.qml\n")
        victim = fixture.path / "victim-legacy-wifi"
        victim.mkdir()
        (victim / "legacy.qml").write_text("owner legacy qml\n")
        (fixture.qml.parent / "wifi").symlink_to(victim, target_is_directory=True)
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "status=FAIL" in report, report
        assert (victim / "legacy.qml").read_text() == "owner legacy qml\n"


exercise_restore_leaf_destinations()
print("PASS: restore refuses symlinked run/VBT/auth/legacy-QML destinations and preserves owner bytes")


def exercise_foreign_logs() -> None:
    with tempfile.TemporaryDirectory(prefix="wifi1b-foreign-logs-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        for path in (fixture.rw2 / "daemon.log", fixture.rw1 / "daemon.log"):
            path.write_text("OWNER-LOG-CONTENT\n")
        service = fixture.root / "etc/systemd/system/wifi1b-daemon.service"
        foreign = b"# owner changed this after install\n[Service]\nExecStart=/bin/true\n"
        service.write_bytes(foreign)
        statefile = fixture.root / "state.json"
        state = json.loads(statefile.read_text())
        state["enabled"]["wifi1b-daemon.service"] = "enabled"
        statefile.write_text(json.dumps(state))
        before = {str(p.relative_to(fixture.rw2)): p.read_bytes()
                  for p in fixture.rw2.rglob("*") if p.is_file()
                  and p.name not in {"restore.log", "restore-result.txt"}}
        commands = fixture.root / "commands.log"
        commands.write_text("")
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "status=FAIL" in report and "error=foreign_service" in report, report
        assert (fixture.rw2 / "daemon.log").read_text() == "OWNER-LOG-CONTENT\n"
        assert (fixture.rw1 / "daemon.log").read_text() == "OWNER-LOG-CONTENT\n"
        after = {str(p.relative_to(fixture.rw2)): p.read_bytes()
                 for p in fixture.rw2.rglob("*") if p.is_file()
                 and p.name not in {"restore.log", "restore-result.txt"}}
        assert after == before, "foreign-service restore changed runtime/keys/backups"
        assert "systemctl ['stop', 'wifi1b-daemon.service']" not in commands.read_text()
        assert "systemctl ['disable', 'wifi1b-daemon.service']" not in commands.read_text()


exercise_foreign_logs()
print("PASS: foreign-service restore preserves bounded product logs plus all runtime/keys/backups and FAILs")


def _vbt_fixture(directory: Path):
    fixture = Fixture(directory, standalone=True)
    fixture.run(fixture.install)
    vbt_dir = fixture.root / "app/data/vbt"
    vbt_dir.mkdir(parents=True, exist_ok=True)
    vbt = vbt_dir / "VBTConfig.json"
    vbt.write_text("applied\n")
    (fixture.rw2 / "backup/vbt-app.original").write_text("original\n")
    (fixture.rw2 / "backup/vbt-app.applied").write_text("applied\n")
    return fixture, vbt, vbt_dir


def exercise_vbt_failure() -> None:
    with tempfile.TemporaryDirectory(prefix="wifi1b-vbt-fail-", dir="/tmp/opencode") as directory:
        fixture, vbt, vbt_dir = _vbt_fixture(Path(directory))
        vbt_dir.chmod(0o555)
        try:
            report = fixture.run(fixture.restore, expect_fail=True)
            assert "status=FAIL" in report, report
            assert vbt.read_text() == "applied\n"
            assert (fixture.rw2 / "backup/vbt-app.original").read_text() == "original\n"
        finally:
            vbt_dir.chmod(0o755)

    with tempfile.TemporaryDirectory(prefix="wifi1b-vbt-mv-fail-", dir="/tmp/opencode") as directory:
        fixture, vbt, _ = _vbt_fixture(Path(directory))
        fault = fixture.path / "vbt-mv-fault"
        fault.mkdir()
        real_mv = shutil.which("mv")
        (fault / "mv").write_text(
            "#!/bin/sh\n"
            "case \"$*\" in *VBTConfig.json*) exit 1;; esac\n"
            f"exec '{real_mv}' \"$@\"\n")
        (fault / "mv").chmod(0o755)
        fixture.env["WIFI1B_PATH"] = str(fault) + ":" + fixture.env["WIFI1B_PATH"]
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "status=FAIL" in report, report
        assert vbt.read_text() == "applied\n"

    with tempfile.TemporaryDirectory(prefix="wifi1b-vbt-restart-fail-", dir="/tmp/opencode") as directory:
        fixture, vbt, _ = _vbt_fixture(Path(directory))
        fault = fixture.path / "vbt-systemctl-fault"
        fault.mkdir()
        (fault / "systemctl").write_text(
            "#!/bin/sh\n"
            "if [ \"$1\" = restart ] && [ \"$2\" = vbtd.service ]; then exit 1; fi\n"
            f"exec '{fixture.bin / 'systemctl'}' \"$@\"\n")
        (fault / "systemctl").chmod(0o755)
        fixture.env["WIFI1B_PATH"] = str(fault) + ":" + fixture.env["WIFI1B_PATH"]
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "status=FAIL" in report, report
        assert vbt.read_text() == "original\n"


exercise_vbt_failure()
print("PASS: standalone VBT copy/move failure reports FAIL, keeps the applied file and backup")


def exercise_key_import_verified() -> None:
    old_ssh = b"old-ssh-key\n"
    old_home = b"old-home-key\n"
    new_key = b"new-owner-key\n"
    with tempfile.TemporaryDirectory(prefix="wifi1b-key-ssh-ro-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        (fixture.rw2 / "ssh").mkdir(parents=True)
        (fixture.rw2 / "home/.ssh").mkdir(parents=True)
        (fixture.rw2 / "home/.ssh/authorized_keys").write_bytes(old_home)
        (fixture.rw2 / "ssh").chmod(0o555)
        (fixture.usb / "authorized_keys").write_bytes(new_key)
        try:
            report = fixture.run(fixture.install, expect_fail=True)
            assert "status=FAIL" in report, report
            log = (fixture.rw2 / "install.log").read_text()
            assert "imported authorized_keys (both destinations verified)" not in log
            assert not (fixture.rw2 / "ssh/authorized_keys").exists()
            assert (fixture.rw2 / "home/.ssh/authorized_keys").read_bytes() == old_home
            assert (fixture.usb / "authorized_keys").read_bytes() == new_key
            assert list((fixture.rw2 / "backup").glob("20*")), "no backup point retained"
        finally:
            (fixture.rw2 / "ssh").chmod(0o755)

    with tempfile.TemporaryDirectory(prefix="wifi1b-key-home-ro-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        (fixture.rw2 / "ssh").mkdir(parents=True)
        (fixture.rw2 / "ssh/authorized_keys").write_bytes(old_ssh)
        (fixture.rw2 / "home/.ssh").mkdir(parents=True)
        (fixture.usb / "authorized_keys").write_bytes(new_key)
        fault = fixture.path / "key-fault"
        fault.mkdir()
        real_cp = shutil.which("cp")
        (fault / "cp").write_text(
            "#!/bin/sh\n"
            "case \"$*\" in *home/.ssh/authorized_keys*) exit 1;; esac\n"
            f"exec '{real_cp}' \"$@\"\n")
        (fault / "cp").chmod(0o755)
        fixture.env["WIFI1B_PATH"] = str(fault) + ":" + fixture.env["WIFI1B_PATH"]
        report = fixture.run(fixture.install, expect_fail=True)
        assert "status=FAIL" in report, report
        assert (fixture.rw2 / "ssh/authorized_keys").read_bytes() == old_ssh
        assert not (fixture.rw2 / "home/.ssh/authorized_keys").exists()
        assert (fixture.usb / "authorized_keys").read_bytes() == new_key

    with tempfile.TemporaryDirectory(prefix="wifi1b-key-corrupt-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        (fixture.rw2 / "ssh").mkdir(parents=True)
        (fixture.rw2 / "ssh/authorized_keys").write_bytes(old_ssh)
        (fixture.rw2 / "home/.ssh").mkdir(parents=True)
        (fixture.rw2 / "home/.ssh/authorized_keys").write_bytes(old_home)
        (fixture.usb / "authorized_keys").write_bytes(new_key)
        fault = fixture.path / "corrupt-copy"
        fault.mkdir()
        counter = fixture.path / "corrupt-count"
        real_cp = shutil.which("cp")
        (fault / "cp").write_text(
            "#!/bin/sh\n"
            "case \"$3\" in *rw_data/wifi1b/home/.ssh/authorized_keys)\n"
            f"  n=$(cat '{counter}' 2>/dev/null || echo 0); n=$((n + 1)); printf '%s\\n' \"$n\" > '{counter}'\n"
            "  if [ \"$n\" -le 3 ]; then printf 'CORRUPT\\n' > \"$3\" 2>/dev/null || true; exit 0; fi ;;\n"
            "esac\n"
            f"exec '{real_cp}' \"$@\"\n")
        (fault / "cp").chmod(0o755)
        fixture.env["WIFI1B_PATH"] = str(fault) + ":" + fixture.env["WIFI1B_PATH"]
        report = fixture.run(fixture.install, expect_fail=True)
        assert "status=FAIL" in report, report
        assert "imported authorized_keys (both destinations verified)" not in (fixture.rw2 / "install.log").read_text()
        assert (fixture.rw2 / "ssh/authorized_keys").read_bytes() == old_ssh
        assert (fixture.rw2 / "home/.ssh/authorized_keys").read_bytes() == old_home
        assert (fixture.usb / "authorized_keys").read_bytes() == new_key


exercise_key_import_verified()
print("PASS: unverified owner-key copies fail the install, retain stick bytes and restore old keys")


def exercise_mask_source_classes() -> None:
    def prepare_and_mask(directory: Path, mutate):
        fixture = Fixture(directory, tweaks=True)
        service = fixture.root / "etc/systemd/system/wifi1b-daemon.service"
        service.write_bytes(b"# owner service\n[Service]\nExecStart=/bin/true\n")
        service.chmod(0o600)
        statefile = fixture.root / "state.json"
        state = json.loads(statefile.read_text())
        state["enabled"]["wifi1b-daemon.service"] = "enabled"
        state["active"]["wifi1b-daemon.service"] = False
        statefile.write_text(json.dumps(state))
        fixture.run(fixture.install)
        latest = (fixture.rw2 / "backup/latest").resolve()
        item = None
        for line in (latest / "transaction/files").read_text().splitlines():
            parts = line.split("|", 2)
            if len(parts) == 3 and parts[2] == str(service):
                item = parts[0]
        assert item
        mutate(latest, item)
        service.unlink()
        service.symlink_to("/dev/null")
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "status=FAIL" in report, report
        assert service.is_symlink() and os.readlink(service) == "/dev/null"

    def missing(latest, item):
        (latest / "transaction" / item).unlink()

    def directory(latest, item):
        (latest / "transaction" / item).unlink()
        (latest / "transaction" / item).mkdir()

    def fifo(latest, item):
        (latest / "transaction" / item).unlink()
        os.mkfifo(latest / "transaction" / item)

    def unreadable(latest, item):
        (latest / "transaction" / item).chmod(0)

    def hashfail(latest, item):
        (latest / "transaction/service.sha256").write_text("0" * 64 + "\n")

    for label, mutate in (("missing", missing), ("directory", directory),
                          ("fifo", fifo), ("unreadable", unreadable),
                          ("hash", hashfail)):
        with tempfile.TemporaryDirectory(prefix=f"wifi1b-mask-{label}-", dir="/tmp/opencode") as directory:
            prepare_and_mask(Path(directory), mutate)


exercise_mask_source_classes()
print("PASS: missing/directory/FIFO/unreadable/hash-mismatch masked-service sources keep the mask and FAIL")


def exercise_apphomescreen_kill_policy() -> None:
    """Only an AppHomeScreen whose exact executable matches is signalled."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-home-kill-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        expected = fixture.root / "app/bin/AppHomeScreen"
        expected.parent.mkdir(parents=True)
        shutil.copyfile(shutil.which("sleep"), expected)
        expected.chmod(0o755)
        foreign = fixture.path / "foreign-AppHomeScreen"
        shutil.copyfile(shutil.which("sleep"), foreign)
        foreign.chmod(0o755)
        owned = subprocess.Popen([str(expected), "30"])
        other = subprocess.Popen([str(foreign), "30"])
        proc_root = fixture.path / "home-proc"
        for pid, exe in ((owned.pid, expected), (other.pid, foreign)):
            entry = proc_root / str(pid)
            entry.mkdir(parents=True)
            (entry / "comm").write_text("AppHomeScreen\n")
            (entry / "exe").symlink_to(exe)
        notifier = fixture.path / "deadbeep"
        notifier.mkdir()
        (notifier / "dbus-send").write_text("#!/bin/sh\nexit 1\n")
        (notifier / "dbus-send").chmod(0o755)
        fixture.env["WIFI1B_HOME_PROC_ROOT"] = str(proc_root)
        fixture.env["WIFI1B_PATH"] = str(notifier) + ":" + fixture.env["WIFI1B_PATH"]
        try:
            report = fixture.run(fixture.install)
            assert "preserved non-owned" in report, report
            try:
                owned.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
            assert owned.poll() is not None, "owned AppHomeScreen was not signalled"
            assert other.poll() is None, "foreign AppHomeScreen was signalled"
        finally:
            for process in (owned, other):
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=10)


exercise_apphomescreen_kill_policy()
print("PASS: AppHomeScreen restart signals only the exact owned executable; foreign same-comm process survives")


def exercise_rollback_service_ownership() -> None:
    """An early rollback never stops a pre-existing service it did not install."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-rollback-svc-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        service = fixture.root / "etc/systemd/system/wifi1b-daemon.service"
        original = b"# owner service\n[Service]\nExecStart=/bin/true\n"
        service.write_bytes(original)
        fault = fixture.path / "early-copy-fault"
        fault.mkdir()
        real_cp = shutil.which("cp")
        (fault / "cp").write_text(
            "#!/bin/sh\n"
            "case \"$*\" in *rw_data/wifi1b/wifi1b_daemon*) exit 1;; esac\n"
            f"exec '{real_cp}' \"$@\"\n")
        (fault / "cp").chmod(0o755)
        fixture.env["WIFI1B_PATH"] = str(fault) + ":" + fixture.env["WIFI1B_PATH"]
        report = fixture.run(fixture.install, expect_fail=True)
        assert "status=FAIL" in report, report
        assert service.read_bytes() == original, "rollback changed a pre-existing service"
        stops = (fixture.root / "commands.log").read_text().count(
            "systemctl ['stop', 'wifi1b-daemon.service']")
        assert stops <= 1, f"rollback stopped an untracked service ({stops} stops)"


exercise_rollback_service_ownership()
print("PASS: early rollback never stops a pre-existing service this invocation did not install")


def exercise_restore_required_failures() -> None:
    """Required iptables/SMACK restoration failures aggregate to FAIL."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-iptables-fail-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        fault = fixture.path / "iptables-fault"
        fault.mkdir()
        (fault / "iptables").write_text(
            "#!/bin/sh\n"
            "case \"$1\" in -S) exit 0;; -F) exit 1;; esac\n"
            "exit 0\n")
        (fault / "iptables").chmod(0o755)
        fixture.env["WIFI1B_PATH"] = str(fault) + ":" + fixture.env["WIFI1B_PATH"]
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "status=FAIL" in report, report
        assert (fixture.rw2 / "backup/latest").is_dir()

    with tempfile.TemporaryDirectory(prefix="wifi1b-kill-survivor-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        survivor = subprocess.Popen(
            ["/bin/sh", "-c", 'trap "" TERM; while :; do sleep 1; done'])
        entry = fixture.root / "proc" / str(survivor.pid)
        entry.mkdir(parents=True)
        (entry / "stat").write_text("fixture live process\n")
        (entry / "comm").write_text("dropbearmulti\n")
        (entry / "exe").symlink_to(fixture.rw2 / "bin/dropbearmulti")
        try:
            report = fixture.run(fixture.restore, expect_fail=True)
            assert "status=FAIL" in report, report
            assert (fixture.rw2 / "backup/latest").is_dir()
        finally:
            survivor.kill()
            survivor.wait(timeout=10)

    with tempfile.TemporaryDirectory(prefix="wifi1b-smack-readback-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        fault = fixture.path / "smack-fault"
        fault.mkdir()
        counter = fixture.path / "getfattr-count"
        (fault / "getfattr").write_text(
            "#!/bin/sh\n"
            f"n=$(cat '{counter}' 2>/dev/null || echo 0)\n"
            "n=$((n + 1))\n"
            f"printf '%s\\n' \"$n\" > '{counter}'\n"
            "if [ $((n % 2)) -eq 1 ]; then printf '_\\n'; else printf 'CHANGED\\n'; fi\n")
        (fault / "getfattr").chmod(0o755)
        fixture.env["WIFI1B_PATH"] = str(fault) + ":" + fixture.env["WIFI1B_PATH"]
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "status=FAIL" in report, report

    with tempfile.TemporaryDirectory(prefix="wifi1b-smack-optional-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        fault = fixture.path / "smack-unsupported"
        fault.mkdir()
        (fault / "setfattr").write_text("#!/bin/sh\nexit 1\n")
        (fault / "setfattr").chmod(0o755)
        fixture.env["WIFI1B_PATH"] = str(fault) + ":" + fixture.env["WIFI1B_PATH"]
        report = fixture.run(fixture.restore)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report


exercise_restore_required_failures()
print("PASS: required iptables/SMACK restoration failures FAIL; unsupported optional relabel stays OK")


def exercise_manual_daemonlog_gate() -> None:
    """Manual-start redirection must not follow a symlinked daemon.log."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-daemonlog-manual-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        victim = fixture.path / "victim-daemonlog-manual"
        (fixture.rw2 / "daemon.log").symlink_to(victim)
        report = fixture.run(fixture.install, fail_point="systemd", expect_fail=True)
        assert "status=FAIL" in report and "unsafe " in report and "destination" in report, report
        assert not victim.exists(), "manual-start redirection followed daemon.log symlink"


exercise_manual_daemonlog_gate()
print("PASS: manual daemon start refuses a symlinked daemon.log before redirection")


def exercise_cache_dirs_preserved() -> None:
    """Unknown pre-existing Qt cache directories are never recursively deleted."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-cache-dirs-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        dirs = [fixture.root / "tmp/qmlcache", fixture.root / "tmp/qt-qml-cache",
                fixture.root / "root/.cache/qtshadercache",
                fixture.root / "root/.cache/AppHomeScreen",
                fixture.root / "root/.cache/qmlcache"]
        for path in dirs:
            path.mkdir(parents=True, exist_ok=True)
            (path / "owner.txt").write_text("owner cache\n")
        report = fixture.run(fixture.install)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        for path in dirs:
            assert (path / "owner.txt").read_text() == "owner cache\n", path

    with tempfile.TemporaryDirectory(prefix="wifi1b-cache-links-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        victims = []
        for index, base in enumerate((fixture.root / "tmp/qmlcache",
                                      fixture.root / "tmp/qt-qml-cache",
                                      fixture.root / "root/.cache/qtshadercache",
                                      fixture.root / "root/.cache/AppHomeScreen",
                                      fixture.root / "root/.cache/qmlcache")):
            base.parent.mkdir(parents=True, exist_ok=True)
            victim = fixture.path / f"cache-victim-{index}"
            victim.mkdir()
            (victim / "owner.txt").write_text("owner cache\n")
            base.symlink_to(victim, target_is_directory=True)
            victims.append(victim)
        report = fixture.run(fixture.install)
        assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
        for victim in victims:
            assert (victim / "owner.txt").read_text() == "owner cache\n"


exercise_cache_dirs_preserved()
print("PASS: ordinary and symlinked pre-existing Qt cache dirs retain owner bytes")


def exercise_restore_home_kill_policy() -> None:
    """Restore signals only the exact resolved AppHomeScreen executable."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-restore-home-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        expected = fixture.root / "app/bin/AppHomeScreen"
        expected.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(shutil.which("sleep"), expected)
        expected.chmod(0o755)
        foreign = fixture.path / "foreign-AppHomeScreen"
        shutil.copyfile(shutil.which("sleep"), foreign)
        foreign.chmod(0o755)
        owned = subprocess.Popen([str(expected), "30"])
        other = subprocess.Popen([str(foreign), "30"])
        for pid, exe in ((owned.pid, expected), (other.pid, foreign)):
            entry = fixture.root / "proc" / str(pid)
            entry.mkdir()
            (entry / "comm").write_text("AppHomeScreen\n")
            (entry / "exe").symlink_to(exe)
        unreadable = fixture.root / "proc/65000"
        unreadable.mkdir()
        (unreadable / "comm").write_text("AppHomeScreen\n")
        notifier = fixture.path / "deadbeep"
        notifier.mkdir()
        (notifier / "dbus-send").write_text("#!/bin/sh\nexit 1\n")
        (notifier / "dbus-send").chmod(0o755)
        fixture.env["WIFI1B_PATH"] = str(notifier) + ":" + fixture.env["WIFI1B_PATH"]
        try:
            report = fixture.run(fixture.restore)
            assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
            assert "preserved non-owned" in report, report
            try:
                owned.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
            assert owned.poll() is not None, "owned restore AppHomeScreen was not signalled"
            assert other.poll() is None, "foreign restore AppHomeScreen was signalled"
        finally:
            for process in (owned, other):
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=10)


exercise_restore_home_kill_policy()
print("PASS: restore signals only exact owned AppHomeScreen; foreign and unreadable exe are preserved")


def _make_wrong_type(path: Path, kind: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() or path.is_symlink():
        path.unlink()
    if kind == "dir":
        path.mkdir()
    elif kind == "fifo":
        os.mkfifo(path)
    elif kind == "socket":
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        old_cwd = os.getcwd()
        try:
            os.chdir(path.parent)
            sock.bind(path.name)
        finally:
            os.chdir(old_cwd)
            sock.close()
    else:
        raise AssertionError(kind)


def _assert_wrong_type(path: Path, kind: str) -> None:
    mode = path.lstat().st_mode
    if kind == "dir":
        assert path.is_dir(), path
        assert not list(path.iterdir()), f"nested write inside directory leaf {path}"
    elif kind == "fifo":
        assert stat.S_ISFIFO(mode), path
    elif kind == "socket":
        assert stat.S_ISSOCK(mode), path


def exercise_directory_and_special_leaves() -> None:
    """Every file-leaf class refuses dirs/FIFOs/sockets/hardlink aliases."""
    cases = []
    rw = ("config.conf", "daemon.log", "wpa_supplicant.conf", "dhcp.leases",
          "bin/opi-usb0-up.sh", "usb0.owner",
          "client.owner", "status.json", "ssh/authorized_keys")
    for rel in rw:
        cases.append((f"rw-{rel}", lambda f, rel=rel: f.rw2 / rel))
    cases.append(("account-marker", lambda f: f.rw2 / "backup/accounts/passwd"))
    for rel in ("HomeScreen/QHMI_HomeScreenMain_Advanced.qml",
                "HomeScreen/Wifi1bPage.qml", "component/Wifi1bTile.qml"):
        cases.append((f"qml-{rel}", lambda f, rel=rel: f.qml / rel))
    for rel in ("HomeScreen/QHMI_HomeScreenMain_Advanced.qml",):
        cases.append((f"backup-{rel}", lambda f, rel=rel: f.usb / "backup_current/AppHomeScreen/qml_eco3.0" / rel))
        cases.append((f"before-{rel}", lambda f, rel=rel: f.usb / "qml_before" / rel))
        cases.append((f"after-{rel}", lambda f, rel=rel: f.usb / "qml_after" / rel))
    cases.append(("before-hash", lambda f: f.usb / "qml_before/hashes.md5"))
    cases.append(("after-hash", lambda f: f.usb / "qml_after/hashes.md5"))
    cases.append(("service", lambda f: f.root / "etc/systemd/system/wifi1b-daemon.service"))
    cases.append(("dropin", lambda f: f.root / "etc/systemd/system/carplay_mdnsd.service.d/60-wifi1b-foreground.conf"))
    cases.append(("tmp-shells", lambda f: f.root / "tmp/shells"))
    cases.append(("install-log", lambda f: f.rw2 / "install.log"))
    cases.append(("rw-result", lambda f: f.rw2 / "result.txt"))
    cases.append(("usb0-rule", lambda f: f.root / "run/udev/rules.d/99-opi-usb0.rules"))

    for label, locate in cases:
        safe_label = label.replace("/", "-")
        for kind in ("dir", "fifo"):
            with tempfile.TemporaryDirectory(prefix=f"wifi1b-type-{safe_label}-{kind}-",
                                             dir="/tmp/opencode") as directory:
                fixture = Fixture(Path(directory), tweaks=True)
                target = locate(fixture)
                _make_wrong_type(target, kind)
                report = fixture.run(fixture.install, expect_fail=True)
                assert "status=FAIL" in report and "unsafe " in report and "destination" in report, (label, kind, report)
                _assert_wrong_type(target, kind)
        if label in ("rw-config.conf", "qml-HomeScreen/QHMI_HomeScreenMain_Advanced.qml"):
            with tempfile.TemporaryDirectory(prefix=f"wifi1b-type-{safe_label}-socket-",
                                             dir="/tmp/opencode") as directory:
                fixture = Fixture(Path(directory), tweaks=True)
                target = locate(fixture)
                _make_wrong_type(target, "socket")
                report = fixture.run(fixture.install, expect_fail=True)
                assert "status=FAIL" in report and "unsafe " in report, (label, report)
                _assert_wrong_type(target, "socket")
        if label in ("rw-config.conf", "rw-ssh/authorized_keys"):
            with tempfile.TemporaryDirectory(prefix=f"wifi1b-type-{safe_label}-hardlink-",
                                             dir="/tmp/opencode") as directory:
                fixture = Fixture(Path(directory), tweaks=True)
                target = locate(fixture)
                victim = fixture.path / "hardlink-victim"
                victim.write_text("OWNER-HARDLINK\n")
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.exists() or target.is_symlink():
                    target.unlink()
                os.link(victim, target)
                report = fixture.run(fixture.install, expect_fail=True)
                assert "status=FAIL" in report and "unsafe " in report, (label, report)
                assert victim.read_text() == "OWNER-HARDLINK\n"
                assert victim.stat().st_nlink == 2


exercise_directory_and_special_leaves()
print("PASS: install refuses directory/FIFO/socket/hardlink file leaves before any nested write")

with tempfile.TemporaryDirectory(prefix="wifi1b-type-asset-dir-", dir="/tmp/opencode") as directory:
    fixture = Fixture(Path(directory), tweaks=True)
    (fixture.usb / "payload/assets/wifi1b/icon-fixture").write_text("asset\n")
    target = fixture.qml.parent / "wifi1b/icon-fixture"
    target.mkdir(parents=True)
    report = fixture.run(fixture.install, expect_fail=True)
    assert "status=FAIL" in report and "unsafe " in report, report
    assert target.is_dir() and not list(target.iterdir())



def exercise_restore_wrong_type_leaves() -> None:
    """Restore refuses directory leaves for runtime/VBT/qml_before targets."""
    cases = []
    cases.append(("daemon-log", lambda f: f.rw2 / "daemon.log"))
    cases.append(("usb0-helper", lambda f: f.rw2 / "bin/opi-usb0-up.sh"))
    cases.append(("usb0-rule", lambda f: f.root / "run/udev/rules.d/99-opi-usb0.rules"))
    cases.append(("run-active", lambda f: f.root / "run/wifi1b/active"))
    cases.append(("vbt", lambda f: f.root / "app/data/vbt/VBTConfig.json"))
    cases.append(("qml-before", lambda f: f.usb / "qml_before/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"))
    for label, locate in cases:
        with tempfile.TemporaryDirectory(prefix=f"wifi1b-restore-type-{label}-",
                                         dir="/tmp/opencode") as directory:
            fixture = Fixture(Path(directory), tweaks=True)
            fixture.run(fixture.install)
            target = locate(fixture)
            _make_wrong_type(target, "dir")
            report = fixture.run(fixture.restore, expect_fail=True)
            assert "status=FAIL" in report and "unsafe " in report, (label, report)
            _assert_wrong_type(target, "dir")

    with tempfile.TemporaryDirectory(prefix="wifi1b-restore-type-transaction-",
                                     dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        latest = (fixture.rw2 / "backup/latest").resolve()
        (latest / "transaction/901").write_text("owner legacy\n")
        with (latest / "transaction/files").open("a") as ledger:
            ledger.write(f"901|1|{fixture.qml.parent}/wifi/legacy.qml\n")
        target = fixture.qml.parent / "wifi/legacy.qml"
        target.mkdir(parents=True)
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "status=FAIL" in report, report
        assert target.is_dir() and not list(target.iterdir())


exercise_restore_wrong_type_leaves()
print("PASS: restore refuses directory file leaves before writes/deletes")


def exercise_snapshot_parent_types() -> None:
    """Snapshot/QML-backup parent positions must be real dirs or absent."""
    install_cases = (
        ("qml-before-home", lambda f: f.usb / "qml_before/HomeScreen",
         lambda p: p.parent.mkdir(parents=True, exist_ok=True)),
        ("qml-before-component", lambda f: f.usb / "qml_before/component",
         lambda p: p.parent.mkdir(parents=True, exist_ok=True)),
        ("qml-after-home", lambda f: f.usb / "qml_after/HomeScreen",
         lambda p: p.parent.mkdir(parents=True, exist_ok=True)),
        ("backup-current-home",
         lambda f: f.usb / "backup_current/AppHomeScreen/qml_eco3.0/HomeScreen",
         lambda p: p.parent.mkdir(parents=True, exist_ok=True)),
        ("backup-current-component",
         lambda f: f.usb / "backup_current/AppHomeScreen/qml_eco3.0/component",
         lambda p: p.parent.mkdir(parents=True, exist_ok=True)),
    )
    for label, locate, prep in install_cases:
        with tempfile.TemporaryDirectory(prefix=f"wifi1b-parent-{label}-",
                                         dir="/tmp/opencode") as directory:
            fixture = Fixture(Path(directory), tweaks=True)
            target = locate(fixture)
            prep(target)
            target.write_text("OWNER-PARENT\n")
            report = fixture.run(fixture.install, expect_fail=True)
            assert "status=FAIL" in report and "unsafe " in report, (label, report)
            assert target.read_text() == "OWNER-PARENT\n"

    with tempfile.TemporaryDirectory(prefix="wifi1b-parent-restore-before-",
                                     dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        target = fixture.usb / "qml_before/HomeScreen"
        shutil.rmtree(target)
        target.write_text("OWNER-PARENT\n")
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "status=FAIL" in report and "unsafe " in report, report
        assert target.read_text() == "OWNER-PARENT\n"


exercise_snapshot_parent_types()
print("PASS: snapshot/QML-backup parent regular files are refused and preserved")


def _run_early(fixture, script, extra=None):
    env = dict(fixture.env)
    env.update(extra or {})
    return subprocess.run(["sh", str(script)], env=env, capture_output=True,
                          text=True, timeout=60)


def exercise_early_report_guard() -> None:
    """The earliest report guard never writes through an unsafe OUT."""
    cases = []
    cases.append(("out-symlink", lambda f: _make_report_symlink(f), None))
    cases.append(("out-fifo", lambda f: os.mkfifo(f.usb / "run_stuff_result.txt"), None))
    cases.append(("out-dir", lambda f: (f.usb / "run_stuff_result.txt").mkdir(), None))
    for label, setup, extra in cases:
        with tempfile.TemporaryDirectory(prefix=f"wifi1b-early-{label}-", dir="/tmp/opencode") as directory:
            fixture = Fixture(Path(directory), tweaks=True)
            owner = fixture.path / "owner-report"
            owner.write_text("OWNER-REPORT\n")
            setup(fixture)
            target = fixture.usb / "run_stuff_result.txt"
            result = _run_early(fixture, fixture.install)
            assert result.returncode != 0, (label, result.stdout, result.stderr)
            assert "status=FAIL" in result.stderr and "unsafe_report_path" in result.stderr, (label, result.stderr)
            assert owner.read_text() == "OWNER-REPORT\n"
            assert not (fixture.rw2 / "install.log").exists(), label

    with tempfile.TemporaryDirectory(prefix="wifi1b-early-hardlink-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        owner = fixture.path / "owner-report"
        owner.write_text("OWNER-REPORT\n")
        os.link(owner, fixture.usb / "run_stuff_result.txt")
        result = _run_early(fixture, fixture.install)
        assert result.returncode != 0
        assert "unsafe_report_path" in result.stderr
        assert owner.read_text() == "OWNER-REPORT\n"

    with tempfile.TemporaryDirectory(prefix="wifi1b-early-usb-link-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        real_stick = fixture.path / "real-stick"
        real_stick.mkdir()
        (real_stick / "run_stuff_result.txt").write_text("OWNER-REPORT\n")
        link = fixture.path / "usb-link"
        link.symlink_to(real_stick, target_is_directory=True)
        result = _run_early(fixture, fixture.install, {"USB_DIR": str(link)})
        assert result.returncode != 0
        assert "unsafe_report_path" in result.stderr
        assert (real_stick / "run_stuff_result.txt").read_text() == "OWNER-REPORT\n"

    with tempfile.TemporaryDirectory(prefix="wifi1b-early-rw-link-", dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        bad_rw = fixture.path / "bad-rw"
        bad_rw.symlink_to(fixture.path / "missing-rw")
        result = _run_early(fixture, fixture.install, {"WIFI1B_RW_DIR": str(bad_rw)})
        assert result.returncode != 0
        report = (fixture.usb / "run_stuff_result.txt").read_text()
        assert "status=FAIL" in report and "runtime directory is a symlink" in report
        assert not (fixture.path / "missing-rw").exists()

    for label, setup in (
            ("out-symlink", lambda f, o: (f.usb / "run_stuff_result.txt").symlink_to(o)),
            ("out-fifo", lambda f, o: os.mkfifo(f.usb / "run_stuff_result.txt")),
            ("out-dir", lambda f, o: (f.usb / "run_stuff_result.txt").mkdir())):
        with tempfile.TemporaryDirectory(prefix=f"wifi1b-early-restore-{label}-",
                                         dir="/tmp/opencode") as directory:
            fixture = Fixture(Path(directory), tweaks=True)
            owner = fixture.path / "owner-restore-report"
            owner.write_text("OWNER-RESTORE\n")
            setup(fixture, owner)
            result = _run_early(fixture, fixture.restore)
            assert result.returncode != 0, (label, result.stdout, result.stderr)
            assert "status=FAIL" in result.stderr and "unsafe_report_path" in result.stderr, (label, result.stderr)
            assert owner.read_text() == "OWNER-RESTORE\n"

    with tempfile.TemporaryDirectory(prefix="wifi1b-early-restore-hardlink-",
                                     dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        owner = fixture.path / "owner-restore-report"
        owner.write_text("OWNER-RESTORE\n")
        os.link(owner, fixture.usb / "run_stuff_result.txt")
        result = _run_early(fixture, fixture.restore)
        assert result.returncode != 0
        assert "unsafe_report_path" in result.stderr
        assert owner.read_text() == "OWNER-RESTORE\n"

    with tempfile.TemporaryDirectory(prefix="wifi1b-early-restore-rw-link-",
                                     dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        bad_rw = fixture.path / "bad-restore-rw"
        bad_rw.symlink_to(fixture.path / "missing-restore-rw")
        result = _run_early(fixture, fixture.restore, {"WIFI1B_RW_DIR": str(bad_rw)})
        assert result.returncode != 0
        report = (fixture.usb / "run_stuff_result.txt").read_text()
        assert "status=FAIL" in report and "runtime directory is a symlink" in report
        assert not (fixture.path / "missing-restore-rw").exists()


def _make_report_symlink(fixture):
    owner = fixture.path / "owner-report"
    owner.write_text("OWNER-REPORT\n")
    (fixture.usb / "run_stuff_result.txt").symlink_to(owner)


exercise_early_report_guard()
print("PASS: earliest report guard fails to stderr only for unsafe USB/report paths")


def exercise_usb0_cleanup_failure() -> None:
    """Restore cannot delete recovery data after explicit USB cleanup failure."""
    with tempfile.TemporaryDirectory(prefix="wifi1b-usb0-restore-fail-",
                                     dir="/tmp/opencode") as directory:
        fixture = Fixture(Path(directory), tweaks=True)
        fixture.run(fixture.install)
        helper = fixture.rw2 / "bin/opi-usb0-up.sh"
        rule = fixture.root / "run/udev/rules.d/99-opi-usb0.rules"
        owner = fixture.rw2 / "usb0.owner"
        rule.parent.mkdir(parents=True)
        before = {helper: b"owner helper\n", rule: b"owner rule\n",
                  owner: b"owner record\n"}
        for path, contents in before.items():
            path.write_bytes(contents)
        fixture.env["WIFI1B_STUB_USB0_FAIL"] = "1"
        report = fixture.run(fixture.restore, expect_fail=True)
        assert "usb0 cleanup failed" in report and "status=FAIL" in report, report
        assert (fixture.rw2 / "wifi1b_daemon").is_file()
        assert (fixture.rw2 / "backup/latest").is_dir()
        for path, contents in before.items():
            assert path.read_bytes() == contents, path


exercise_usb0_cleanup_failure()
print("PASS: explicit USB cleanup failure preserves helper/rule/owner and recovery runtime")

with tempfile.TemporaryDirectory(prefix="wifi1b-usb0-legacy-cli-",
                                 dir="/tmp/opencode") as directory:
    fixture = Fixture(Path(directory), tweaks=True)
    fixture.run(fixture.install)
    marker = fixture.path / "wrong-cleanup-binary"
    installed = fixture.rw2 / "wifi1b_daemon"
    installed.write_text(
        "#!/bin/sh\n"
        f"case \"$*\" in *--usb0-cleanup*) printf 'WRONG\\n' > '{marker}'; exit 1;; esac\n"
        "exit 0\n")
    installed.chmod(0o755)
    report = fixture.run(fixture.restore)
    assert "status=OK" in report or "status=REBOOT_REQUIRED" in report, report
    assert not marker.exists(), "new cleanup option was sent to an older installed daemon"
print("PASS: restore uses verified current payload for the new USB cleanup CLI")


def exercise_usb0_cleanup_prerequisites() -> None:
    """Earlier stop/active-marker failures must preserve all USB proof."""
    for kind in ("stop", "active"):
        with tempfile.TemporaryDirectory(prefix=f"wifi1b-usb0-prereq-{kind}-",
                                         dir="/tmp/opencode") as directory:
            fixture = Fixture(Path(directory), tweaks=True)
            fixture.run(fixture.install)
            helper = fixture.rw2 / "bin/opi-usb0-up.sh"
            rule = fixture.root / "run/udev/rules.d/99-opi-usb0.rules"
            owner = fixture.rw2 / "usb0.owner"
            rule.parent.mkdir(parents=True)
            proof = {helper: b"owner helper\n", rule: b"owner rule\n",
                     owner: b"owner record\n"}
            for path, data in proof.items():
                path.write_bytes(data)
            active = fixture.root / "run/wifi1b/active"
            active.write_text("owner active\n")
            called = fixture.path / "cleanup-called"
            fixture.env["WIFI1B_STUB_USB0_CALLED_FILE"] = str(called)
            fault = fixture.path / "prereq-fault"
            fault.mkdir()
            if kind == "stop":
                command = fault / "systemctl"
                command.write_text(
                    "#!/bin/sh\n"
                    "if [ \"$1\" = stop ] && [ \"$2\" = wifi1b-daemon.service ]; then exit 1; fi\n"
                    f"exec '{fixture.bin / 'systemctl'}' \"$@\"\n")
            else:
                command = fault / "rm"
                command.write_text(
                    "#!/bin/sh\n"
                    f"case \"$*\" in *'{active}'*) exit 1;; esac\n"
                    f"exec '{shutil.which('rm')}' \"$@\"\n")
            command.chmod(0o755)
            fixture.env["WIFI1B_PATH"] = str(fault) + ":" + fixture.env["WIFI1B_PATH"]
            report = fixture.run(fixture.restore, expect_fail=True)
            assert "restore prerequisites failed" in report, (kind, report)
            assert not called.exists(), "USB cleanup ran after a failed prerequisite"
            assert active.read_text() == "owner active\n"
            assert (fixture.rw2 / "wifi1b_daemon").is_file()
            assert (fixture.rw2 / "backup/latest").is_dir()
            for path, data in proof.items():
                assert path.read_bytes() == data, (kind, path)


exercise_usb0_cleanup_prerequisites()
print("PASS: failed stop/active removal blocks USB cleanup and retains ownership proof")
