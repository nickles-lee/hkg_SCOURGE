#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Policy fixtures for the standalone default bundle and BatteryTile skip.

Uses stub stages only; no vehicle installer runs and every path is under
/tmp/opencode.
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
QML = ("HomeScreen/QHMI_HomeScreenMain_Advanced.qml",
       "HomeScreen/QHMI_HomeScreenMain_Wide.qml",
       "HomeScreen/Screen_HomeScreen_AllMenus.qml",
       "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml",
       "component/QCustomizedMenuListAllowEmpty.qml",
       "component/common/Q_ThemeImage.qml")


def build_fake_packages(root: Path) -> Path:
    packages = root / "packages"
    (packages / "common").mkdir(parents=True)
    for name in ("compose.sh",):
        shutil.copyfile(PACKAGES / name, packages / name)
    for name in ("check_layout.py", "install_io.sh", "runtime_cleanup.sh"):
        shutil.copyfile(PACKAGES / "common" / name, packages / "common" / name)
    qml_fixture = root / "qml"
    for name in QML:
        target = qml_fixture / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(f"// fixture {name}\n")
    for name in ("battery", "wifi", "tweaks", "wifi1b"):
        out = packages / name / "out/usb"
        (out / "payload/systemd").mkdir(parents=True)
        (out / "backup").mkdir()
        (out / "payload/marker").write_text(name)
        (out / "payload/systemd" / f"{name}-daemon.service").write_text("[Service]\n")
        if name in ("battery", "wifi"):
            (out / "payload/shared_tile").write_text(f"{name}-copy")
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
        maps = [f"payload/marker|{root / 'installed' / name / 'file'}"]
        if name in ("battery", "wifi"):
            maps.append(f"*payload/shared_tile|{qml_fixture}/component/BatteryTile.qml"
                        if name == "battery" else
                        f"payload/shared_tile|{qml_fixture}/component/BatteryTile.qml")
        (packages / name / "skip.map").write_text("\n".join(maps) + "\n")
        (packages / name / "skip.services").write_text(f"{name}-daemon.service\n")
        script = f'''#!/bin/sh
echo '{name}:MODE' >> "$ORDER"
if [ '{name}' = battery ] && [ MODE = install ]; then
    cp -f "$USB_DIR/payload/shared_tile" "$TEST_QML_DIR/component/BatteryTile.qml"
fi
if [ '{name}' = wifi ] && [ MODE = install ]; then
    cp -f "$USB_DIR/payload/shared_tile" "$TEST_QML_DIR/component/BatteryTile.qml"
fi
if [ MODE = install ]; then
    mkdir -p "{root / 'installed' / name}"
    cp -f "$USB_DIR/payload/marker" "{root / 'installed' / name / 'file'}"
fi
printf 'STUB_{name}\\nstatus=OK\\n' > "$USB_DIR/run_stuff_result.txt"
exit 0
'''
        for target, mode in (("run_stuff.sh", "install"), ("run_stuff_restore.sh", "restore")):
            (out / target).write_text(script.replace("MODE", mode))
        (out / "manifest.md5").write_text("".join(
            f"{hashlib.md5(p.read_bytes()).hexdigest()}  {p.relative_to(out)}\n"
            for p in sorted((out / "payload").rglob("*")) if p.is_file()))
    return packages


def wrapper_from_template(digest: str) -> str:
    helper = PACKAGES / "common/runtime_cleanup.sh"
    cleanup_sha = hashlib.sha256(helper.read_bytes()).hexdigest()
    text = (PACKAGES / "flat_wrapper.sh").read_text() \
        .replace("@TARBALL_SHA256@", digest) \
        .replace("@RUNTIME_CLEANUP_SHA256@", cleanup_sha) \
        .replace("# @RUNTIME_CLEANUP_SHELL@", helper.read_text())
    assert "@RUNTIME_CLEANUP_SHELL@" not in text
    assert "@TARBALL_SHA256@" not in text
    assert f'RUNTIME_CLEANUP_DIGEST="{cleanup_sha}"' in text
    return text


def make_wrapper(root: Path, tree: Path, stick_name: str) -> Path:
    stick = root / stick_name
    stick.mkdir()
    archive = stick / "appcombo.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        for path in tree.iterdir():
            tar.add(path, arcname="./" + path.name)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    wrapper = stick / "run_stuff.sh"
    wrapper.write_text(wrapper_from_template(digest))
    assert not (stick / "runtime_cleanup.sh").exists()
    return stick


def fixture_env(root: Path, qml: Path) -> dict:
    bin_dir = root / "bin"
    bin_dir.mkdir(exist_ok=True)
    for name in ("sleep", "chsmack", "chown", "sync", "date"):
        tool = bin_dir / name
        tool.write_text("#!/bin/sh\nexit 0\n")
        tool.chmod(0o755)
    active_units = root / "active-units"
    active_units.write_text("battery-daemon.service\nwifi-daemon.service\n"
                            "tweaks-daemon.service\nwifi1b-daemon.service\n")
    (root / "run/wifi1b").mkdir(parents=True)
    (root / "run/wifi").mkdir(parents=True)
    (root / "etc/systemd/system").mkdir(parents=True)
    beep = root / "runtime/combo/bin/dbus-send"
    beep.parent.mkdir(parents=True, exist_ok=True)
    beep.write_text("#!/bin/sh\nexit 0\n")
    beep.chmod(0o755)
    systemctl = bin_dir / "systemctl"
    systemctl.write_text('''#!/bin/sh
unit=""
for a in "$@"; do case "$a" in --*) ;; *) unit=$a ;; esac; done
grep -qx "$unit" "$ACTIVE_UNITS" 2>/dev/null && exit 0
exit 3
''')
    systemctl.chmod(0o755)
    (root / "proc").mkdir()
    (root / "proc/mounts").write_text(f"device {root} ext4 rw 0 0\n")
    (root / "runtime").mkdir(exist_ok=True)
    return dict(os.environ,
                SCOURGE_SKIP_BUILD="1",
                TMPDIR="/tmp/opencode",
                COMBO_RW_DIR=str(root / "runtime/combo"),
                COMBO_RUNTIME_ROOT=str(root / "runtime"),
                COMBO_LEDGER_DIR=str(root / "ledger"),
                COMBO_QML_DIR=str(qml),
                COMBO_PATH=str(bin_dir) + ":/usr/bin:/bin",
                CLEANUP_PROC_ROOT=str(root / "proc"),
                COMBO_RUN_DIR=str(root / "run"),
                WIFI1B_SERVICE=str(root / "etc/systemd/system/wifi1b-daemon.service"),
                WIFI_SERVICE=str(root / "etc/systemd/system/wifi-daemon.service"),
                CLEANUP_WIFI_SERVICE=str(root / "etc/systemd/system/wifi-daemon.service"),
                INSTALL_IO_ROOT=str(root),
                INSTALL_IO_MOUNTS=str(root / "proc/mounts"),
                ACTIVE_UNITS=str(active_units),
                ORDER=str(root / "order"),
                TEST_QML_DIR=str(qml))


def run_wrapper(stick: Path, env: dict, extra=None):
    order = Path(env["ORDER"])
    before = order.read_text().splitlines() if order.exists() else []
    result = subprocess.run(["sh", str(stick / "run_stuff.sh")],
                            env=dict(env, USB_DIR=str(stick), **(extra or {})),
                            capture_output=True, text=True, timeout=90)
    after = order.read_text().splitlines() if order.exists() else []
    return result, before, after


with tempfile.TemporaryDirectory(prefix="standalone-policy-", dir="/tmp/opencode") as d:
    root = Path(d)
    packages = build_fake_packages(root)
    qml = root / "qml"
    env = fixture_env(root, qml)
    tree = root / "tree"
    tree.mkdir()
    subprocess.run(["sh", str(packages / "compose.sh"), str(tree), "battery,tweaks,wifi1b"],
                   env=dict(env, SCOURGE_STANDALONE="1"), check=True, capture_output=True)
    marker = tree / "combo/standalone"
    assert marker.is_file() and "standalone=1" in marker.read_text()
    assert not (tree / "combo/wifi").exists() and not (tree / "combo/migrate").exists()
    master = (tree / "run_stuff.sh").read_text()
    assert 'BUNDLE_LAYOUT="standalone"' in master
    assert 'STAGE_ORDER="battery tweaks wifi1b"' in master
    assert 'wifi)    run_stage' not in master and 'migrate)    run_stage' not in master
    assert "runtime_cleanup.sh" in (tree / "combo/preflight.md5").read_text()
    readme = (tree / "README.txt").read_text()
    assert "does NOT install or reinstall the WiFi app" in readme
    assert "UID 0" in readme and "not car-validated" in readme
    assert "migrate" not in readme.lower()

    stick = make_wrapper(root, tree, "stick")
    result, _, after = run_wrapper(stick, env)
    assert result.returncode == 0, (result.stdout, result.stderr,
                                    (stick / "appcombo_install.log").read_text())
    assert after == ["battery:install", "tweaks:install", "wifi1b:install"], after
    assert not any(line.startswith("wifi:") for line in after)
    assert "status=OK" in (stick / "wrapper_result.txt").read_text()

    # Standalone builds have no WiFi stage, so Battery must verify its own
    # BatteryTile copy: identical bytes skip; changed bytes reinstall.
    assert (qml / "component/BatteryTile.qml").read_text() == "battery-copy"
    result, before, after = run_wrapper(stick, env)
    assert result.returncode == 0, result.stderr
    assert after == before, after
    assert "status=SKIPPED" in (stick / "battery_result.txt").read_text()
    (qml / "component/BatteryTile.qml").write_text("owner changed tile")
    result, before, after = run_wrapper(stick, env)
    assert result.returncode == 0, result.stderr
    assert after[len(before):] == ["battery:install"], after[len(before):]

    # Unreadable standalone marker: the raw master fails closed before any
    # stage executes.  (The wrapper only unpacks; the master owns the gate.)
    marker.chmod(0o644)
    broken = root / "broken"
    broken.mkdir()
    with tarfile.open(broken / "appcombo.tar.gz", "w:gz") as tar:
        for path in tree.iterdir():
            def unreadable_marker(info):
                if info.name.endswith("combo/standalone"):
                    info.mode = 0
                return info
            tar.add(path, arcname="./" + path.name, filter=unreadable_marker)
    digest = hashlib.sha256((broken / "appcombo.tar.gz").read_bytes()).hexdigest()
    (broken / "run_stuff.sh").write_text(wrapper_from_template(digest))
    order = Path(env["ORDER"])
    before = order.read_text()
    result = subprocess.run(["sh", str(broken / "run_stuff.sh")],
                            env=dict(env, USB_DIR=str(broken)),
                            capture_output=True, text=True, timeout=90)
    assert result.returncode != 0
    assert order.read_text() == before, "a stage ran despite an unreadable standalone marker"
    assert "status=FAIL" in (broken / "wrapper_result.txt").read_text()
    marker.chmod(0o644)

    # An unexpanded build placeholder must fail closed before the first
    # runtime write; the two-file pair never relies on an external helper.
    unexpanded = root / "unexpanded"
    unexpanded.mkdir()
    shutil.copyfile(stick / "appcombo.tar.gz", unexpanded / "appcombo.tar.gz")
    digest = hashlib.sha256((unexpanded / "appcombo.tar.gz").read_bytes()).hexdigest()
    cleanup_sha = hashlib.sha256((PACKAGES / "common/runtime_cleanup.sh").read_bytes()).hexdigest()
    text = (PACKAGES / "flat_wrapper.sh").read_text() \
        .replace("@TARBALL_SHA256@", digest) \
        .replace("@RUNTIME_CLEANUP_SHA256@", cleanup_sha)
    assert "@RUNTIME_CLEANUP_SHELL@" in text
    (unexpanded / "run_stuff.sh").write_text(text)
    before = order.read_text()
    result = subprocess.run(["sh", str(unexpanded / "run_stuff.sh")],
                            env=dict(env, USB_DIR=str(unexpanded)),
                            capture_output=True, text=True, timeout=90)
    assert result.returncode != 0
    assert order.read_text() == before, "an unexpanded placeholder ran a stage"
    assert "unexpanded_cleanup_helper" in (unexpanded / "wrapper_result.txt").read_text()
    assert not (unexpanded / "runtime_cleanup.sh").exists()

# WiFi-present combo: the WiFi stage owns the final BatteryTile bytes, so the
# Battery "*" entry is deferred even though its own copy differs.
with tempfile.TemporaryDirectory(prefix="combo-policy-", dir="/tmp/opencode") as d:
    root = Path(d)
    packages = build_fake_packages(root)
    qml = root / "qml"
    env = fixture_env(root, qml)
    tree = root / "tree"
    tree.mkdir()
    subprocess.run(["sh", str(packages / "compose.sh"), str(tree),
                    "battery,wifi,tweaks,wifi1b"], env=env, check=True, capture_output=True)
    assert not (tree / "combo/standalone").exists()
    stick = make_wrapper(root, tree, "stick")
    result, _, after = run_wrapper(stick, env)
    assert result.returncode == 0, (result.stdout, result.stderr,
                                    (stick / "appcombo_install.log").read_text())
    assert after == ["battery:install", "wifi:install", "tweaks:install", "wifi1b:install"], after
    assert (qml / "component/BatteryTile.qml").read_text() == "wifi-copy"
    result, before, after = run_wrapper(stick, env)
    assert result.returncode == 0, result.stderr
    assert after == before, after
    assert "status=SKIPPED" in (stick / "battery_result.txt").read_text()
    assert "status=SKIPPED" in (stick / "wifi_result.txt").read_text()

print("PASS: standalone default selects Battery/Tweaks/Wifi1b with combo/standalone marker, no WiFi or migration stage")
print("PASS: BatteryTile is checked in standalone builds and deferred to the WiFi stage in combo builds")
print("PASS: unreadable standalone marker fails closed before any stage")
print("PASS: unexpanded cleanup placeholder fails closed before any stage/runtime helper write")

def test_build_flat_stale_helper() -> None:
    """build_flat removes a stale top-level helper only when it is ours."""
    with tempfile.TemporaryDirectory(prefix="buildflat-stale-", dir="/tmp/opencode") as d:
        base = Path(d)
        repo = base / "repo"
        packages = repo / "packages"
        (packages / "common").mkdir(parents=True)
        shutil.copyfile(PACKAGES / "build_flat.sh", packages / "build_flat.sh")
        shutil.copyfile(PACKAGES / "flat_wrapper.sh", packages / "flat_wrapper.sh")
        shutil.copyfile(PACKAGES / "common/runtime_cleanup.sh", packages / "common/runtime_cleanup.sh")
        (packages / "compose.sh").write_text(
            "#!/bin/sh\n"
            "printf '%s|%s|%s\\n' \"${SCOURGE_STANDALONE:-0}\" \"$1\" \"$2\" "
            f"> '{repo}/compose.args'\n"
            "target=$1\n"
            "mkdir -p \"$target/combo\"\n"
            "printf 'standalone=1\\n' > \"$target/combo/standalone\"\n"
            "printf '#!/bin/sh\\nexit 0\\n' > \"$target/run_stuff.sh\"\n")
        for rel in ("wifi1b/scripts/check_combined.py", "common/check_skip_maps.py",
                    "common/test_layout_qt.py"):
            path = packages / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("raise SystemExit(0)\n")
        out = base / "out"
        out.mkdir()
        stale = out / "runtime_cleanup.sh"
        stale.write_text("owner file\n")
        result = subprocess.run(["sh", str(packages / "build_flat.sh"), str(out)],
                                capture_output=True, text=True, timeout=90)
        assert result.returncode != 0
        assert stale.read_text() == "owner file\n", "unknown top-level file was unlinked"
        assert "refusing to overwrite unknown file" in result.stderr
        shutil.copyfile(packages / "common/runtime_cleanup.sh", stale)
        result = subprocess.run(["sh", str(packages / "build_flat.sh"), str(out)],
                                capture_output=True, text=True, timeout=90)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert not stale.exists()
        assert (out / "run_stuff.sh").is_file() and (out / "appcombo.tar.gz").is_file()
        compose_args = (repo / "compose.args").read_text().strip()
        assert compose_args.startswith("1|") and compose_args.endswith("|battery,tweaks,wifi1b"), compose_args
        wrapper = (out / "run_stuff.sh").read_text()
        assert "cleanup_bounded_logs()" in wrapper
        assert "@RUNTIME_CLEANUP_SHELL@" not in wrapper
        readme = (out / "README.txt").read_text()
        assert "Copy BOTH files" in readme and "Copy ALL THREE" not in readme
        assert "UID 0" in readme and "not car-validated" in readme
        assert not (out / "runtime_cleanup.sh").exists()
    print("PASS: build_flat preserves unknown top-level files and removes only a byte-identical stale helper")


test_build_flat_stale_helper()
