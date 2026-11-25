#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Real composer/wrapper, stub app stages; never execute vehicle installers.

Also exercises the unchanged-stage skip: a rebuilt bundle must leave packages
whose packaged bytes and installed files/services/UI are all unchanged alone,
and must reinstall the moment anything really differs.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from qml_layout import HELPERS

PACKAGES = Path(__file__).resolve().parents[1]
UNITS = ("battery-daemon.service", "wifi-daemon.service",
         "tweaks-daemon.service", "wifi1b-daemon.service")
QML = ("HomeScreen/QHMI_HomeScreenMain_Advanced.qml",
       "HomeScreen/QHMI_HomeScreenMain_Wide.qml",
       "HomeScreen/Screen_HomeScreen_AllMenus.qml",
       "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml",
       "component/QCustomizedMenuListAllowEmpty.qml",
       "component/common/Q_ThemeImage.qml")

with tempfile.TemporaryDirectory(prefix="combo-regression-", dir="/tmp/opencode") as d:
    root = Path(d)
    packages = root / "packages"
    packages.mkdir()
    shutil.copyfile(PACKAGES / "compose.sh", packages / "compose.sh")
    (packages / "common").mkdir()
    shutil.copyfile(PACKAGES / "common/check_layout.py", packages / "common/check_layout.py")
    shutil.copyfile(PACKAGES / "common/install_io.sh", packages / "common/install_io.sh")
    shutil.copyfile(PACKAGES / "common/runtime_cleanup.sh", packages / "common/runtime_cleanup.sh")
    bin_dir = root / "bin"
    bin_dir.mkdir()
    for name in ("sleep", "chsmack", "chown", "sync"):
        tool = bin_dir / name
        tool.write_text("#!/bin/sh\nexit 0\n")
        tool.chmod(0o755)
    beep = bin_dir / "dbus-send"
    beep.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$BEEPS"\nexit 0\n')
    beep.chmod(0o755)
    active_units = root / "active-units"
    active_units.write_text("".join(u + "\n" for u in UNITS))
    systemctl = bin_dir / "systemctl"
    systemctl.write_text('''#!/bin/sh
echo "$*" >> "$SYSTEMCTL_LOG"
unit=""
for a in "$@"; do case "$a" in --*) ;; *) unit=$a ;; esac; done
grep -qx "$unit" "$ACTIVE_UNITS" 2>/dev/null && exit 0
exit 3
''')
    systemctl.chmod(0o755)
    (root / "runtime/combo/bin").mkdir(parents=True)
    beep_copy = root / "runtime/combo/bin/dbus-send"
    shutil.copyfile(beep, beep_copy)
    beep_copy.chmod(0o755)

    # Shared home-screen QML fixture (the skip check hashes exactly these).
    qml_dir = root / "qml"
    for name in QML:
        target = qml_dir / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(f"// fixture {name}\n")
    ledger = root / "ledger"

    # Construct minimal fixture payloads with production manifest verification.
    for name in ("battery", "wifi", "tweaks", "wifi1b"):
        out = packages / name / "out/usb"
        (out / "payload/systemd").mkdir(parents=True)
        (out / "payload").mkdir(parents=True, exist_ok=True)
        (out / "backup").mkdir()
        (out / "payload/marker").write_text(name)
        (out / "payload/systemd" / f"{name}-daemon.service").write_text(
            f"# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors\n"
            f"[Service]\nExecStart=/bin/true\n")
        (packages / name / "skip.map").write_text(
            f"payload/marker|{root / 'installed' / name / 'file'}\n")
        (packages / name / "skip.services").write_text(f"{name}-daemon.service\n")
        if name == "tweaks":
            qml = out / "payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile_wifi.qml"
            qml.parent.mkdir(parents=True)
            text = HELPERS
            for page in (1, 2, 3):
                text += "QMenuPageNoDrag{ }\n"
                for index, tile in enumerate(("battery", "wifi", "tweaks", "reboot")):
                    text += f'''{tile.capitalize()}Tile {{
 id: {tile}Tile{page}
  property var slot: root.firstEmptySlot({index})
  visible: slot !== null && slot !== undefined && slot.page === {page - 1}
 x: slot.index
 y: slot.index
 width: k_itemWidth
 height: k_itemHeight
}}
'''
            qml.write_text(text)
        dest = root / "installed" / name / "file"
        script = f'''#!/bin/sh
echo '{name}:MODE' >> "$ORDER"
if [ '{name}:MODE' = 'wifi:install' ]; then
    test -f "$USB_DIR/wifi.txt" || exit 7
fi
if [ '{name}' = wifi ] && [ MODE = install ]; then
    f="$TEST_QML_DIR/HomeScreen/QHMI_HomeScreenMain_Advanced.qml"
    rev="WIFI_REV_$(cat "$USB_DIR/payload/marker")"
    grep -q "$rev" "$f" 2>/dev/null || echo "// $rev" >> "$f"
fi
if [ '{name}' = tweaks ] && [ MODE = install ]; then
    f="$TEST_QML_DIR/HomeScreen/Screen_HomeScreen_AllMenus.qml"
    grep -q 'TWEAKS_MARK' "$f" 2>/dev/null || echo "// TWEAKS_MARK" >> "$f"
fi
if [ MODE = install ]; then
    mkdir -p "{dest.parent}"
    cp -f "$USB_DIR/payload/marker" "{dest}"
fi
status=OK
[ "${{FAIL_STAGE:-}}" != '{name}' ] || status=FAIL
[ "${{REBOOT_STAGE:-}}" != '{name}' ] || status=REBOOT_REQUIRED
[ -z "${{FAIL_EARLY:-}}" ] || exit 9
printf 'STUB_{name}\\nstatus=%s\\n' "$status" > "$USB_DIR/run_stuff_result.txt"
exit 0
'''
        for target, mode in (("run_stuff.sh", "install"), ("run_stuff_restore.sh", "restore")):
            (out / target).write_text(script.replace("MODE", mode))
        (out / "manifest.md5").write_text("".join(
            f"{hashlib.md5(p.read_bytes()).hexdigest()}  {p.relative_to(out)}\n"
            for p in sorted((out / "payload").rglob("*")) if p.is_file()))
    tree = root / "tree"
    tree.mkdir()
    (root / "proc").mkdir()
    (root / "run/wifi1b").mkdir(parents=True)
    (root / "run/wifi").mkdir(parents=True)
    (root / "etc/systemd/system").mkdir(parents=True)
    env = dict(os.environ, SCOURGE_SKIP_BUILD="1", COMBO_RW_DIR=str(root / "runtime/combo"),
               TMPDIR="/tmp/opencode",
               COMBO_RUNTIME_ROOT=str(root / "runtime"), COMBO_PATH=str(bin_dir) + ":/usr/bin:/bin",
               BEEPS=str(root / "beeps"), ORDER=str(root / "order"),
               ACTIVE_UNITS=str(active_units), TEST_QML_DIR=str(qml_dir),
               COMBO_QML_DIR=str(qml_dir), COMBO_LEDGER_DIR=str(ledger),
               COMBO_RUN_DIR=str(root / "run"),
               WIFI1B_SERVICE=str(root / "etc/systemd/system/wifi1b-daemon.service"),
               WIFI_SERVICE=str(root / "etc/systemd/system/wifi-daemon.service"),
               CLEANUP_WIFI_SERVICE=str(root / "etc/systemd/system/wifi-daemon.service"),
               CLEANUP_PROC_ROOT=str(root / "proc"),
               SYSTEMCTL_LOG=str(root / "systemctl.log"),
               md5sum=shutil.which("md5sum") or "md5sum")
    subprocess.run(["sh", str(packages / "compose.sh"), str(tree), "wifi1b,tweaks,wifi,battery"], env=env, check=True, capture_output=True)
    assert 'STOPS_WIFI="1"' in (tree / "run_stuff.sh").read_text()
    # A battery,tweaks-only bundle must leave a working WiFi app alone.
    tree_bt = root / "tree-bt"
    tree_bt.mkdir()
    subprocess.run(["sh", str(packages / "compose.sh"), str(tree_bt), "battery,tweaks"], env=env, check=True, capture_output=True)
    assert 'STOPS_WIFI="0"' in (tree_bt / "run_stuff.sh").read_text()
    # A wifi1b-only standalone bundle must not stop the WiFi app either.
    tree_w1b = root / "tree-w1b"
    tree_w1b.mkdir()
    subprocess.run(["sh", str(packages / "compose.sh"), str(tree_w1b), "battery,tweaks,wifi1b"],
                   env=dict(env, SCOURGE_STANDALONE="1"), check=True, capture_output=True)
    assert 'STOPS_WIFI="0"' in (tree_w1b / "run_stuff.sh").read_text()
    # Standalone preflight must leave a live WiFi owner alone.
    (root / "proc/5000").mkdir()
    (root / "runtime/wifi").mkdir(parents=True, exist_ok=True)
    (root / "runtime/wifi/wifi_daemon").write_text("#!/bin/sh\nexit 0\n")
    (root / "proc/5000/exe").symlink_to(root / "runtime/wifi/wifi_daemon")
    Path(env["SYSTEMCTL_LOG"]).write_text("")
    w1b_run = subprocess.run(["sh", str(tree_w1b / "run_stuff.sh")],
                             env=dict(env, USB_DIR=str(tree_w1b),
                                      COMBO_RW_DIR=str(root / "runtime/w1b-combo"),
                                      COMBO_LEDGER_DIR=str(root / "ledger-w1b"),
                                      ORDER=str(root / "order-w1b")),
                             capture_output=True, text=True)
    assert w1b_run.returncode == 0, (w1b_run.stdout, w1b_run.stderr)
    assert "stop wifi-daemon.service" not in Path(env["SYSTEMCTL_LOG"]).read_text()
    assert os.readlink(root / "proc/5000/exe") == str(root / "runtime/wifi/wifi_daemon")
    # Raw master must refuse a symlinked COMBO_RW_DIR before writing combo.log.
    rw_victim = root / "master-rw-victim"
    rw_victim.mkdir()
    rw_link = root / "master-rw-link"
    rw_link.symlink_to(rw_victim, target_is_directory=True)
    blocked = subprocess.run(["sh", str(tree / "run_stuff.sh")],
                             env=dict(env, USB_DIR=str(tree), COMBO_RW_DIR=str(rw_link)),
                             capture_output=True, text=True)
    assert blocked.returncode != 0
    assert not (rw_victim / "combo.log").exists()
    assert "unsafe_symlink_destination" in (tree / "run_stuff_result.txt").read_text()
    (tree / "wifi.txt").write_text("ssid=Fixture\npsk=fixture-key\n")
    stick = root / "stick"
    stick.mkdir()
    archive = stick / "appcombo.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        for path in tree.iterdir():
            tar.add(path, arcname="./" + path.name)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    cleanup_source = packages / "common/runtime_cleanup.sh"
    cleanup_sha = hashlib.sha256(cleanup_source.read_bytes()).hexdigest()
    helper_text = cleanup_source.read_text()
    wrapper = stick / "run_stuff.sh"
    wrapper.write_text((PACKAGES / "flat_wrapper.sh").read_text()
                       .replace("@TARBALL_SHA256@", digest)
                       .replace("@RUNTIME_CLEANUP_SHA256@", cleanup_sha)
                       .replace("# @RUNTIME_CLEANUP_SHELL@", helper_text))
    assert "@RUNTIME_CLEANUP_SHELL@" not in wrapper.read_text()
    assert f'RUNTIME_CLEANUP_DIGEST="{cleanup_sha}"' in wrapper.read_text()
    assert not (stick / "runtime_cleanup.sh").exists()
    env["USB_DIR"] = str(stick)
    (root / 'runtime/battery').mkdir()
    (root / 'runtime/battery/result.txt').write_text('status=FAIL\nerror=historical\n')
    (root / 'runtime/battery/restore.log').write_text('historical restore\n')
    (root / 'runtime/battery/status.json').write_text('{"ok":true}\n')

    def run(extra=None):
        beeps_path = root / "beeps"
        before = len(beeps_path.read_text().splitlines()) if beeps_path.exists() else 0
        result = subprocess.run(["sh", str(wrapper)], env=dict(env, **(extra or {})),
                                capture_output=True, text=True)
        after = beeps_path.read_text().splitlines() if beeps_path.exists() else []
        assert len(after) - before == 6, (len(after) - before, extra)
        return result

    def order():
        return (root / "order").read_text().splitlines()

    def stage_status(name):
        return (stick / f"{name}_result.txt").read_text()

    # 1. First run: every stage installs.
    assert run().returncode == 0
    assert order() == ["battery:install", "wifi:install", "tweaks:install", "wifi1b:install"]
    for name in ("battery", "wifi", "tweaks", "wifi1b"):
        assert f"STUB_{name}" in stage_status(name)
    assert (stick / "combo.log").exists()
    assert "status=OK" in (stick / "wrapper_result.txt").read_text()
    assert 'status=OK' in (stick / 'battery_result.txt').read_text(), 'historical runtime result overwrote current result'
    assert not (stick / 'battery_restore.log').exists()
    assert not (stick / 'battery_status.json').exists()
    # status.json carries cleartext psk/carplay_psk fields; it must never be
    # exported to the stick, even as a runtime snapshot.
    assert not (stick / 'runtime_snapshot_battery_status.json').exists()
    assert 'status=OK' in (stick / 'wifi1b_result.txt').read_text()

    # 2. Identical bundle re-run: every stage is skipped, nothing is rewritten.
    marker_md5 = hashlib.md5((root / "installed/wifi/file").read_bytes()).hexdigest()
    qml_before = {n: (qml_dir / n).read_bytes() for n in QML}
    assert run().returncode == 0
    assert order() == ["battery:install", "wifi:install", "tweaks:install", "wifi1b:install"]
    for name in ("battery", "wifi", "tweaks", "wifi1b"):
        assert "status=SKIPPED" in stage_status(name), (name, stage_status(name))
    assert "status=OK" in (stick / "wrapper_result.txt").read_text()
    assert {n: (qml_dir / n).read_bytes() for n in QML} == qml_before
    assert 'stage_wifi1b=status=SKIPPED' in (stick / 'run_stuff_result.txt').read_text()

    # 3. One corrupted installed file: only that stage reinstalls.
    (root / "installed/battery/file").write_text("corrupted")
    assert run().returncode == 0
    assert order()[-1] == "battery:install"
    assert "status=OK" in stage_status("battery")
    assert "status=SKIPPED" in stage_status("wifi")
    assert hashlib.md5((root / "installed/wifi/file").read_bytes()).hexdigest() == marker_md5

    # 4. A changed WiFi payload: battery stays skipped, the QML layers after
    #    the change are rebuilt (the shared QML changed).
    (packages / "wifi/out/usb/payload/marker").write_text("wifi2")
    (packages / "wifi/out/usb/manifest.md5").write_text("".join(
        f"{hashlib.md5(p.read_bytes()).hexdigest()}  {p.relative_to(packages / 'wifi/out/usb')}\n"
        for p in sorted((packages / "wifi/out/usb/payload").rglob("*")) if p.is_file()))
    # Rebuild the stick archive from the same tree (the changed payload is the
    # package source; the combo tree is staged again by the composer).
    subprocess.run(["sh", str(packages / "compose.sh"), str(tree), "wifi1b,tweaks,wifi,battery"],
                   env=env, check=True, capture_output=True)
    with tarfile.open(archive, "w:gz") as tar:
        for path in tree.iterdir():
            tar.add(path, arcname="./" + path.name)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    wrapper.write_text((PACKAGES / "flat_wrapper.sh").read_text()
                       .replace("@TARBALL_SHA256@", digest)
                       .replace("@RUNTIME_CLEANUP_SHA256@", cleanup_sha)
                       .replace("# @RUNTIME_CLEANUP_SHELL@", helper_text))
    assert "@RUNTIME_CLEANUP_SHELL@" not in wrapper.read_text()
    assert run().returncode == 0
    tail = order()[-3:]
    assert tail[0].startswith("wifi:install"), tail
    assert [n for n in tail[1:]] == ["tweaks:install", "wifi1b:install"], tail
    assert order().count("battery:install") == 2, order()

    # 5. A stopped service forces that stage to reinstall; the rest skip.
    lines = [u for u in active_units.read_text().splitlines() if u != "tweaks-daemon.service"]
    active_units.write_text("".join(u + "\n" for u in lines))
    before = len(order())
    assert run().returncode == 0
    tail = order()[before:]
    assert tail == ["tweaks:install"], tail
    assert "status=OK" in stage_status("tweaks")
    assert "status=SKIPPED" in stage_status("wifi1b")
    active_units.write_text("".join(u + "\n" for u in UNITS))

    # 6. Restore clears the install ledger and runs in reverse order.
    (root / "order").write_text("")
    (stick / "RESTORE").touch()
    assert run().returncode == 0
    assert order() == ["wifi1b:restore", "tweaks:restore", "wifi:restore", "battery:restore"]
    assert not ledger.exists(), "restore must invalidate the install ledger"
    (stick / "RESTORE").unlink()

    # 7. After restore the same bundle must reinstall everything.
    (root / "order").write_text("")
    assert run().returncode == 0
    assert order() == ["battery:install", "wifi:install", "tweaks:install", "wifi1b:install"]

    # A failed restore stage must not block the remaining apps.
    (root / "order").write_text("")
    (stick / "RESTORE").touch()
    failed_restore = run({"FAIL_STAGE": "wifi"})
    assert failed_restore.returncode != 0
    assert order() == ["wifi1b:restore", "tweaks:restore", "wifi:restore", "battery:restore"]
    (stick / "RESTORE").unlink()

    # rc=0 plus status=FAIL must not be called success, and skipping must not
    # mask a stage that actually has to run.
    shutil.rmtree(ledger, ignore_errors=True)
    failed = run({"FAIL_STAGE": "wifi"})
    assert failed.returncode != 0
    assert "status=FAIL" in (stick / "wrapper_result.txt").read_text()
    assert (root / 'order').read_text().splitlines()[-1] == 'wifi:install', 'dependent stage ran after failure'
    # A shared preflight failure must not execute any stage, even a stub.
    shutil.rmtree(ledger, ignore_errors=True)
    before_order = (root / 'order').read_bytes()
    invalid_mounts = root / 'invalid-mounts'
    invalid_mounts.write_text('/dev/root /other ext4 rw 0 0\n')
    blocked = subprocess.run(['sh', str(wrapper)], env=dict(env, INSTALL_IO_MOUNTS=str(invalid_mounts)), capture_output=True)
    assert blocked.returncode != 0 and (root / 'order').read_bytes() == before_order
    assert 'preflight=status=FAIL error=root_mount_detection' in (stick / 'run_stuff_result.txt').read_text()
    # A stage that dies before writing a result must not leave the previous
    # run's per-stage result looking current on the stick.
    shutil.rmtree(ledger, ignore_errors=True)
    (stick / "wifi_result.txt").write_text('status=OK\n')
    dead = run({"FAIL_EARLY": "wifi"})
    assert dead.returncode != 0
    assert not (stick / "wifi_result.txt").exists()
    assert "status=missing" in (stick / "run_stuff_result.txt").read_text()
    # rc=0 with status=REBOOT_REQUIRED is a successful stage (the WiFi
    # installer reports it when the UI restart is not verified on the spot).
    shutil.rmtree(ledger, ignore_errors=True)
    ok = run({"REBOOT_STAGE": "wifi"})
    assert ok.returncode == 0
    assert "stage_wifi=status=REBOOT_REQUIRED rc=0" in (stick / "run_stuff_result.txt").read_text()
    assert "status=OK" in (stick / "wrapper_result.txt").read_text()
    # Preflight service ownership requires byte-identical staged units, not
    # just an SPDX-looking header.  A modified foreign unit must not be stopped.
    service = Path(env["WIFI1B_SERVICE"])
    service.write_text("# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors\n"
                       "[Service]\nExecStart=/bin/wrong\n")
    Path(env["SYSTEMCTL_LOG"]).write_text("")
    assert run().returncode == 0
    assert "stop wifi1b-daemon.service" not in Path(env["SYSTEMCTL_LOG"]).read_text()
    exact = tree / "combo/wifi1b/payload/systemd/wifi1b-daemon.service"
    service.write_bytes(exact.read_bytes())
    Path(env["SYSTEMCTL_LOG"]).write_text("")
    assert run().returncode == 0
    assert "stop wifi1b-daemon.service" in Path(env["SYSTEMCTL_LOG"]).read_text()
    # Restore mode preserves bounded product logs at both the wrapper and the
    # inner master; only an install run may reclaim them.
    restore_log = root / "runtime/wifi1b/daemon.log"
    restore_log.parent.mkdir(parents=True, exist_ok=True)
    restore_log.write_text("OWNER-LOG-CONTENT\n")
    (stick / "MODE").write_text("restore\n")
    assert subprocess.run(["sh", str(wrapper)], env=env, capture_output=True).returncode == 0
    assert restore_log.read_text() == "OWNER-LOG-CONTENT\n", "restore mode truncated bounded product logs"
    (stick / "MODE").unlink()
    # A tarball-only replacement is rejected before executing any stage.
    before = (root / "order").read_bytes()
    archive.write_bytes(archive.read_bytes() + b"corrupt")
    assert subprocess.run(["sh", str(wrapper)], env=env, capture_output=True).returncode != 0
    assert (root / "order").read_bytes() == before
    # Raw master (no wrapper) must refuse symlinked RUN_DIR and ledger leaves
    # before any write or deletion.
    run_victim = root / "raw-run-victim"
    run_victim.mkdir()
    (run_victim / "active").write_text("owner-active\n")
    shutil.rmtree(root / "run/wifi1b", ignore_errors=True)
    (root / "run/wifi1b").symlink_to(run_victim, target_is_directory=True)
    raw_env = dict(env, USB_DIR=str(tree_w1b),
                   COMBO_RW_DIR=str(root / "raw-combo"),
                   COMBO_LEDGER_DIR=str(root / "raw-ledger"),
                   ORDER=str(root / "raw-order"))
    raw = subprocess.run(["sh", str(tree_w1b / "run_stuff.sh")],
                         env=raw_env, capture_output=True, text=True)
    assert raw.returncode != 0
    assert "unsafe_symlink_destination" in (tree_w1b / "run_stuff_result.txt").read_text()
    assert (run_victim / "active").read_text() == "owner-active\n"

    (root / "run/wifi1b").unlink()
    (root / "run/wifi1b").mkdir()
    ledger_raw = root / "raw-ledger"
    ledger_raw.mkdir()
    ledger_victims = []
    for name in ("battery", "tweaks", "wifi1b", "final-qml"):
        victim = root / f"raw-ledger-victim-{name}"
        victim.write_text("owner-digest\n")
        (ledger_raw / f"{name}.digest").symlink_to(victim)
        ledger_victims.append(victim)
    raw = subprocess.run(["sh", str(tree_w1b / "run_stuff.sh")],
                         env=raw_env, capture_output=True, text=True)
    assert raw.returncode != 0
    assert "unsafe_symlink_destination" in (tree_w1b / "run_stuff_result.txt").read_text()
    for victim in ledger_victims:
        assert victim.read_text() == "owner-digest\n"

    # Directory-at-file-leaf must also be refused, not cleaned recursively.
    ledger_dir = root / "raw-ledger-dir"
    ledger_dir.mkdir()
    (ledger_dir / "battery.digest").mkdir()
    (ledger_dir / "final-qml.digest").mkdir()
    raw = subprocess.run(["sh", str(tree_w1b / "run_stuff.sh")],
                         env=dict(raw_env, COMBO_LEDGER_DIR=str(ledger_dir)),
                         capture_output=True, text=True)
    assert raw.returncode != 0
    assert "unsafe_symlink_destination" in (tree_w1b / "run_stuff_result.txt").read_text()
    assert (ledger_dir / "battery.digest").is_dir() and not list((ledger_dir / "battery.digest").iterdir())
    assert (ledger_dir / "final-qml.digest").is_dir() and not list((ledger_dir / "final-qml.digest").iterdir())

    (root / "run/wifi1b/active").mkdir()
    raw = subprocess.run(["sh", str(tree_w1b / "run_stuff.sh")],
                         env=raw_env, capture_output=True, text=True)
    assert raw.returncode != 0
    assert (root / "run/wifi1b/active").is_dir()
    assert not list((root / "run/wifi1b/active").iterdir())
    (root / "run/wifi1b/active").rmdir()
    print("PASS: raw master refuses symlinked RUN_DIR and ledger digest leaves before writes")

    def raw_restore(ledger_path):
        return subprocess.run(["sh", str(tree_w1b / "run_stuff.sh")],
                              env=dict(raw_env, COMBO_LEDGER_DIR=str(ledger_path),
                                       SCOURGE_MODE="restore"),
                              capture_output=True, text=True)

    # Unknown ledger entries preserve every entry and FAIL; only a fully
    # product-format ledger is removed, non-recursively.
    for label, setup in (
            ("owner-file", lambda d: (d / "owner.txt").write_text("OWNER-LEDGER\n")),
            ("unknown-subdir", lambda d: (d / "unknown").mkdir()),
            ("known-mixed", lambda d: ((d / "owner.txt").write_text("OWNER-LEDGER\n"),
                                       (d / "battery.digest").write_text("a" * 32 + "\n")))):
        ledger_case = root / f"ledger-case-{label}"
        ledger_case.mkdir()
        setup(ledger_case)
        case_result = raw_restore(ledger_case)
        assert case_result.returncode != 0, (label, case_result.stdout, case_result.stderr)
        assert "status=FAIL" in (tree_w1b / "run_stuff_result.txt").read_text()
        assert ledger_case.exists(), f"{label} ledger was removed"
        if label != "unknown-subdir":
            assert (ledger_case / "owner.txt").read_text() == "OWNER-LEDGER\n"
        if label == "known-mixed":
            assert (ledger_case / "battery.digest").exists(), "known digest removed from mixed ledger"

    ledger_ok = root / "ledger-case-valid"
    ledger_ok.mkdir()
    for name in ("battery", "tweaks", "wifi1b", "final-qml"):
        (ledger_ok / f"{name}.digest").write_text("b" * 32 + "\n")
    valid_result = raw_restore(ledger_ok)
    assert valid_result.returncode == 0, (valid_result.stdout, valid_result.stderr)
    assert "status=OK" in (tree_w1b / "run_stuff_result.txt").read_text()
    assert not ledger_ok.exists(), "verified product ledger was not cleaned"

    ledger_ro = root / "ledger-case-readonly"
    ledger_ro.mkdir()
    (ledger_ro / "battery.digest").write_text("c" * 32 + "\n")
    ledger_ro.chmod(0o555)
    try:
        ro_result = raw_restore(ledger_ro)
        assert ro_result.returncode != 0
        assert "status=FAIL" in (tree_w1b / "run_stuff_result.txt").read_text()
        assert (ledger_ro / "battery.digest").read_text() == "c" * 32 + "\n"
    finally:
        ledger_ro.chmod(0o755)
    print("PASS: raw master restore preserves unknown ledger state and removes only verified product digests")

print("PASS: real combo/wrapper order, credentials, per-stage results, beeps, reverse restore, false-success rejection, stale-result cleanup, REBOOT_REQUIRED, archive binding, unchanged-stage skip and revalidation")
