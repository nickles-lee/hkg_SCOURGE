#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Real composer/wrapper, stub app stages; never execute vehicle installers."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

PACKAGES = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="combo-regression-", dir="/tmp/opencode") as d:
    root = Path(d)
    packages = root / "packages"
    packages.mkdir()
    shutil.copyfile(PACKAGES / "compose.sh", packages / "compose.sh")
    (packages / "common").mkdir()
    shutil.copyfile(PACKAGES / "common/check_layout.py", packages / "common/check_layout.py")
    bin_dir = root / "bin"
    bin_dir.mkdir()
    for name in ("sleep", "chsmack", "chown", "sync"):
        tool = bin_dir / name
        tool.write_text("#!/bin/sh\nexit 0\n")
        tool.chmod(0o755)
    beep = bin_dir / "dbus-send"
    beep.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$BEEPS"\nexit 0\n')
    beep.chmod(0o755)
    (root / "runtime/combo/bin").mkdir(parents=True)
    (root / "runtime/combo/bin/dbus-send").symlink_to(beep)
    # Construct minimal fixture payloads with production manifest verification.
    for name in ("battery", "wifi", "tweaks"):
        out = packages / name / "out/usb"
        (out / "payload").mkdir(parents=True)
        (out / "backup").mkdir()
        (out / "payload/marker").write_text(name)
        if name == "tweaks":
            qml = out / "payload/qml/AppHomeScreen/qml_eco3.0/HomeScreen/QHMI_HomeScreenMain_Advanced.battery_tile_wifi.qml"
            qml.parent.mkdir(parents=True)
            text = ""
            for page in (1, 2, 3):
                text += "QMenuPageNoDrag{ }\n"
                for index, tile in enumerate(("battery", "wifi", "tweaks", "reboot")):
                    text += f'''{tile.capitalize()}Tile {{
 id: {tile}Tile{page}
 property var slot: firstEmptySlot({index})
 visible: slot !== null && slot.page === {page - 1}
 x: slot.index
 y: slot.index
 width: k_itemWidth
 height: k_itemHeight
}}
'''
            qml.write_text(text)
        for script, mode in (("run_stuff.sh", "install"), ("run_stuff_restore.sh", "restore")):
            (out / script).write_text(f'''#!/bin/sh
echo '{name}:{mode}' >> "$ORDER"
if [ '{name}:{mode}' = wifi:install ]; then
    test -f "$USB_DIR/wifi.txt" || exit 7
fi
status=OK
[ "${{FAIL_STAGE:-}}" != '{name}' ] || status=FAIL
[ "${{REBOOT_STAGE:-}}" != '{name}' ] || status=REBOOT_REQUIRED
[ -z "${{FAIL_EARLY:-}}" ] || exit 9
printf 'STUB_{name}\\nstatus=%s\\n' "$status" > "$USB_DIR/run_stuff_result.txt"
exit 0
''')
        (out / "manifest.md5").write_text("".join(
            f"{hashlib.md5(p.read_bytes()).hexdigest()}  {p.relative_to(out)}\n"
            for p in sorted((out / "payload").rglob("*")) if p.is_file()))
    tree = root / "tree"
    tree.mkdir()
    env = dict(os.environ, SCOURGE_SKIP_BUILD="1", COMBO_RW_DIR=str(root / "runtime/combo"),
               COMBO_RUNTIME_ROOT=str(root / "runtime"), COMBO_PATH=str(bin_dir) + ":/usr/bin:/bin",
               BEEPS=str(root / "beeps"), ORDER=str(root / "order"))
    subprocess.run(["sh", str(packages / "compose.sh"), str(tree), "tweaks,wifi,battery"], env=env, check=True, capture_output=True)
    (tree / "wifi.txt").write_text("ssid=Fixture\npsk=fixture-key\n")
    stick = root / "stick"
    stick.mkdir()
    archive = stick / "appcombo.tar.gz"
    with tarfile.open(archive, "w:gz") as tar:
        for path in tree.iterdir():
            tar.add(path, arcname="./" + path.name)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    wrapper = stick / "run_stuff.sh"
    wrapper.write_text((PACKAGES / "flat_wrapper.sh").read_text().replace("@TARBALL_SHA256@", digest))
    env["USB_DIR"] = str(stick)
    subprocess.run(["sh", str(wrapper)], env=env, check=True, capture_output=True)
    assert (root / "order").read_text().splitlines() == ["battery:install", "wifi:install", "tweaks:install"]
    for name in ("battery", "wifi", "tweaks"):
        assert f"STUB_{name}" in (stick / f"{name}_result.txt").read_text()
    assert (stick / "combo.log").exists()
    assert "status=OK" in (stick / "wrapper_result.txt").read_text()
    calls = (root / "beeps").read_text().splitlines()
    assert len(calls) == 6, calls
    assert all("array:byte:0x00" in c and "--reply-timeout=1000" in c and "--print-reply" in c for c in calls)
    # Restore selects the real reverse-order dispatcher, not an absent script.
    (root / "order").write_text("")
    (stick / "RESTORE").touch()
    subprocess.run(["sh", str(wrapper)], env=env, check=True, capture_output=True)
    assert (root / "order").read_text().splitlines() == ["tweaks:restore", "wifi:restore", "battery:restore"]
    (stick / "RESTORE").unlink()
    # rc=0 plus status=FAIL must not be called success.
    failed = subprocess.run(["sh", str(wrapper)], env=dict(env, FAIL_STAGE="wifi"), capture_output=True)
    assert failed.returncode != 0
    assert "status=FAIL" in (stick / "wrapper_result.txt").read_text()
    # A stage that dies before writing a result must not leave the previous
    # run's per-stage result looking current on the stick.
    assert (stick / "wifi_result.txt").exists()
    dead = subprocess.run(["sh", str(wrapper)], env=dict(env, FAIL_EARLY="wifi"), capture_output=True)
    assert dead.returncode != 0
    assert not (stick / "wifi_result.txt").exists()
    assert "status=missing" in (stick / "run_stuff_result.txt").read_text()
    # rc=0 with status=REBOOT_REQUIRED is a successful stage (the WiFi
    # installer reports it when the UI restart is not verified on the spot).
    ok = subprocess.run(["sh", str(wrapper)], env=dict(env, REBOOT_STAGE="wifi"), capture_output=True)
    assert ok.returncode == 0
    assert "stage_wifi=status=REBOOT_REQUIRED rc=0" in (stick / "run_stuff_result.txt").read_text()
    assert "status=OK" in (stick / "wrapper_result.txt").read_text()
    # A tarball-only replacement is rejected before executing any stage.
    before = (root / "order").read_bytes()
    archive.write_bytes(archive.read_bytes() + b"corrupt")
    assert subprocess.run(["sh", str(wrapper)], env=env, capture_output=True).returncode != 0
    assert (root / "order").read_bytes() == before
print("PASS: real combo/wrapper order, credentials, per-stage results, beeps, reverse restore, false-success rejection, stale-result cleanup, REBOOT_REQUIRED, archive binding")
