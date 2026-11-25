#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Fixture-only safety tests for packages/common/runtime_cleanup.sh.

Every path lives under /tmp/opencode.  Nothing here touches the host /etc,
/proc, /rw_data or /app, and no real process is signalled.
"""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import socket
import stat
import subprocess
import tarfile
import tempfile

PACKAGES = Path(__file__).resolve().parents[1]
HELPER = PACKAGES / "common/runtime_cleanup.sh"
QML_FILES = (
    "HomeScreen/QHMI_HomeScreenMain_Advanced.qml",
    "HomeScreen/QHMI_HomeScreenMain_Wide.qml",
    "HomeScreen/Screen_HomeScreen_AllMenus.qml",
    "HomeScreen/Screen_HomeScreen_Submenu_Setup.qml",
    "component/QCustomizedMenuListAllowEmpty.qml",
)


class Fixture:
    def __init__(self, base: Path):
        self.base = base
        self.root = base / "root"
        self.runtime = self.root / "rw_data"
        self.apphome = self.root / "app/share/AppHomeScreen"
        self.qml = self.apphome / "qml_eco3.0"
        self.proc = self.root / "proc"
        self.service = self.root / "etc/systemd/system/wifi-daemon.service"
        self.stick = base / "stick"
        self.wifi = self.runtime / "wifi"
        self.victim = base / "victim"
        self.victim.mkdir(parents=True)
        (self.victim / "keep.txt").write_text("owner data\n")
        (self.victim / "daemon.log").write_text("owner log\n")
        for name in QML_FILES:
            target = self.qml / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"// fixture {name}\n")
        self.proc.mkdir(parents=True)
        (self.proc / "424242").mkdir()
        self.service.parent.mkdir(parents=True, exist_ok=True)
        self.wifi.mkdir(parents=True)
        (self.wifi / "config.conf").write_text("owner-credential\n")
        (self.wifi / "ssh").mkdir()
        (self.wifi / "ssh/authorized_keys").write_text("owner-key\n")
        (self.wifi / "backup").mkdir()
        (self.wifi / "backup/old-state").write_text("backup\n")

    def shell(self, body: str, env_extra=None) -> subprocess.CompletedProcess:
        env = dict(os.environ,
                   CLEANUP_PROC_ROOT=str(self.proc),
                   RUNTIME_CLEANUP_LOG="",
                   HELPER=str(HELPER),
                   RUNTIME=str(self.runtime),
                   QML=str(self.qml),
                   STICK=str(self.stick),
                   SERVICE=str(self.service))
        env.update(env_extra or {})
        script = (
            '. "$HELPER"\n'
            'RUNTIME_CLEANUP_NOTES=""\n'
            f'{body}\n'
            'printf "NOTES=%s\\n" "$RUNTIME_CLEANUP_NOTES"\n'
        )
        return subprocess.run(["sh", "-c", script], env=env,
                              capture_output=True, text=True, timeout=90)


def make_bundle(fixture: Fixture, corrupt=False) -> Path:
    source = fixture.base / "bundle/wifi-state"
    source.mkdir(parents=True, exist_ok=True)
    (source / "config.conf").write_text("archived-copy\n")
    directory = fixture.stick / "backup"
    directory.mkdir(parents=True, exist_ok=True)
    bundle = directory / "wifi-pre-migration-20260101T000000Z.tar.gz"
    with tarfile.open(bundle, "w:gz") as output:
        output.add(source, arcname="wifi")
    if corrupt:
        bundle.write_bytes(bundle.read_bytes() + b"corrupt")
    return bundle


def make_empty_bundle(fixture: Fixture) -> Path:
    directory = fixture.stick / "backup"
    directory.mkdir(parents=True, exist_ok=True)
    bundle = directory / "wifi-pre-migration-empty.tar.gz"
    empty = fixture.base / "empty/wifi"
    empty.mkdir(parents=True)
    with tarfile.open(bundle, "w:gz") as output:
        output.add(empty, arcname="wifi")
    return bundle


def marker_absent(fixture: Fixture) -> None:
    for name in QML_FILES:
        (fixture.qml / name).write_text(f"// no WiFi marker {name}\n")


def remove_call(fixture: Fixture, expected: str = "", archive: str = "") -> str:
    return (f'cleanup_removed_wifi "$RUNTIME" "$QML" "$STICK" '
            f'{shlex.quote(archive)} {shlex.quote(expected)} "$SERVICE"; '
            'printf "RC=%s\\n" "$?"')


def bundle_call(fixture: Fixture, bundle: Path) -> str:
    return remove_call(fixture, hashlib.sha256(bundle.read_bytes()).hexdigest(), str(bundle))


def assert_wifi_preserved(fixture: Fixture) -> None:
    assert (fixture.wifi / "config.conf").read_text() == "owner-credential\n"
    assert (fixture.wifi / "ssh/authorized_keys").read_text() == "owner-key\n"


def test_bounded_logs() -> None:
    with tempfile.TemporaryDirectory(prefix="cleanup-logs-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        wifi1b = fixture.runtime / "wifi1b"
        wifi1b.mkdir()
        (wifi1b / "daemon.log").write_text("wifi1b log\n")
        (fixture.wifi / "daemon.log").symlink_to(fixture.victim / "daemon.log")
        (fixture.runtime / "battery").symlink_to(fixture.victim)
        (fixture.wifi / "owner.log").write_text("keep me\n")
        result = fixture.shell('cleanup_bounded_logs "$RUNTIME" || true')
        assert result.returncode == 0, result
        assert (wifi1b / "daemon.log").read_text() == "", "wifi1b log not truncated"
        assert (fixture.wifi / "daemon.log").is_symlink(), "WiFi log symlink followed"
        assert (fixture.victim / "daemon.log").read_text() == "owner log\n"
        assert (fixture.wifi / "owner.log").read_text() == "keep me\n"
        assert "refused symlink log" in result.stdout
        assert "refused symlink runtime dir" in result.stdout
    print("PASS: bounded cleanup truncates known logs (including removed WiFi), refuses symlink files/dirs and keeps unknown logs")


def test_stale_temps() -> None:
    with tempfile.TemporaryDirectory(prefix="cleanup-temps-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        wifi1b = fixture.runtime / "wifi1b"
        wifi1b.mkdir()
        dead = wifi1b / "status.json.tmp.999999"
        dead.write_text("stale\n")
        unknown = wifi1b / "owner.tmp.999999"
        unknown.write_text("unknown\n")
        live = wifi1b / f"status.json.tmp.{os.getpid()}"
        live.write_text("live pid\n")
        (fixture.proc / str(os.getpid())).mkdir()
        invalid = wifi1b / "status.json.tmp.1"
        invalid.write_text("pid one\n")
        nonnumeric = wifi1b / "config.conf.tmp.abc"
        nonnumeric.write_text("unproven\n")
        linked = wifi1b / "wpa_supplicant.conf.tmp.999998"
        linked.symlink_to(fixture.victim / "daemon.log")
        wifi_temp = fixture.wifi / "status.json.tmp.999997"
        wifi_temp.write_text("wifi temp\n")
        result = fixture.shell('cleanup_stale_temps "$RUNTIME" || true')
        assert dead.exists(), "temps must be deferred before radios are verified stopped"
        assert "deferred stale temp cleanup" in result.stdout

        # Standalone scope: wifi1b dead, WiFi alive -> only wifi1b temps.
        (fixture.proc / "5000").mkdir()
        (fixture.proc / "5000/exe").symlink_to(fixture.runtime / "wifi/wifi_daemon")
        result = fixture.shell(
            'cleanup_stale_temps "$RUNTIME" || true',
            {"RUNTIME_CLEANUP_RADIOS_STOPPED": "1",
             "RUNTIME_CLEANUP_SCAN_WIFI": "0"})
        assert not dead.exists(), result.stdout
        assert wifi_temp.exists(), "WiFi temp deleted in standalone scope"
        assert unknown.exists() and live.exists() and invalid.exists() and nonnumeric.exists()
        assert linked.is_symlink()
        assert (fixture.victim / "daemon.log").read_text() == "owner log\n"
        assert "kept unknown temp" in result.stdout
        assert "kept live-pid temp" in result.stdout
        assert "kept invalid-pid temp" in result.stdout
        assert "refused symlink temp" in result.stdout

        # WiFi scope requires WiFi dead too.
        dead.write_text("stale again\n")
        result = fixture.shell(
            'cleanup_stale_temps "$RUNTIME" || true',
            {"RUNTIME_CLEANUP_RADIOS_STOPPED": "1",
             "RUNTIME_CLEANUP_SCAN_WIFI": "1"})
        assert dead.exists(), "temps deleted while WiFi owner was alive"
        assert "WiFi process not verifiably stopped" in result.stdout

        # Unreadable process table is not evidence of death.
        result = fixture.shell(
            'cleanup_stale_temps "$RUNTIME" || true',
            {"RUNTIME_CLEANUP_RADIOS_STOPPED": "1",
             "RUNTIME_CLEANUP_SCAN_WIFI": "1",
             "CLEANUP_PROC_ROOT": str(fixture.base / "missing-proc")})
        assert dead.exists()
        assert "not verifiably stopped" in result.stdout
    print("PASS: stale temp cleanup requires both in-scope radios verifiably stopped, excludes WiFi temps in standalone scope and fails closed on unreadable /proc")


def test_removed_wifi_evidence() -> None:
    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-empty-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        result = fixture.shell(remove_call(fixture))
        assert_wifi_preserved(fixture)
        assert "caller bundle proof missing" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-ok-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        result = fixture.shell(bundle_call(fixture, bundle))
        assert not fixture.wifi.exists(), result.stdout
        receipts = list((fixture.stick / "backup").glob("wifi-pre-migration-current-*.tar.gz"))
        assert len(receipts) == 1, result.stdout
        assert (receipts[0].stat().st_mode & 0o777) == 0o600
        with tarfile.open(receipts[0]) as archive:
            assert archive.extractfile("wifi/config.conf").read() == b"owner-credential\n"
            assert archive.extractfile("wifi/ssh/authorized_keys").read() == b"owner-key\n"
        receipt = fixture.runtime / "combo/wifi-removed.receipt"
        text = receipt.read_text()
        assert text.splitlines()[0] == "SCOURGE_WIFI_REMOVAL_RECEIPT"
        assert f"archive_sha256={hashlib.sha256(receipts[0].read_bytes()).hexdigest()}" in text
        assert "state_sha256=" in text
        assert "removed owned WiFi runtime after verified current-state archive" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-old-empty-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        empty = make_empty_bundle(fixture)
        result = fixture.shell(bundle_call(fixture, empty))
        assert not fixture.wifi.exists(), result.stdout
        fresh = list((fixture.stick / "backup").glob("wifi-pre-migration-current-*.tar.gz"))
        assert len(fresh) == 1
        with tarfile.open(fresh[0]) as archive:
            assert archive.extractfile("wifi/config.conf").read() == b"owner-credential\n"

    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-stale-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        stale = make_bundle(fixture)
        (fixture.wifi / "config.conf").write_text("new-credential\n")
        result = fixture.shell(bundle_call(fixture, stale))
        assert not fixture.wifi.exists(), result.stdout
        fresh = list((fixture.stick / "backup").glob("wifi-pre-migration-current-*.tar.gz"))
        with tarfile.open(fresh[0]) as archive:
            assert archive.extractfile("wifi/config.conf").read() == b"new-credential\n"

    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-bundles-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        good = make_bundle(fixture)
        corrupt = make_bundle(fixture, corrupt=True)
        result = fixture.shell(bundle_call(fixture, corrupt))
        assert_wifi_preserved(fixture)
        assert "bundle archive hash mismatch" in result.stdout or "unreadable" in result.stdout
        result = fixture.shell(remove_call(fixture, "0" * 64, str(good)))
        assert_wifi_preserved(fixture)
        assert "bundle archive hash mismatch" in result.stdout
        result = fixture.shell(remove_call(fixture))
        assert_wifi_preserved(fixture)
        assert "caller bundle proof missing" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-gates-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        make_bundle(fixture)
        (fixture.qml / QML_FILES[0]).write_text("// WIFI_PAYLOAD_260128\n")
        (fixture.qml / "HomeScreen/WifiPage.qml").write_text("// page\n")
        fixture.service.write_text("[Service]\nExecStart=/bin/true\n")
        result = fixture.shell(bundle_call(fixture, make_bundle(fixture)))
        assert_wifi_preserved(fixture)
        assert "WiFi marker" in result.stdout or "WiFi UI still present" in result.stdout

    for label, make in (
            ("wifi-asset", lambda f: (f.apphome / "wifi").mkdir()),
            ("hello-asset", lambda f: (f.apphome / "hello").mkdir()),
            ("hello-tile", lambda f: (f.qml / "component/HelloTile.qml").write_text("// hello\n")),
            ("dropbear-live", lambda f: (f.proc / "5001").mkdir() or
                (f.proc / "5001/exe").symlink_to(f.runtime / "wifi/bin/dropbearmulti")),
    ):
        with tempfile.TemporaryDirectory(prefix=f"cleanup-wifi-{label}-", dir="/tmp/opencode") as d:
            fixture = Fixture(Path(d))
            marker_absent(fixture)
            bundle = make_bundle(fixture)
            make(fixture)
            result = fixture.shell(bundle_call(fixture, bundle))
            assert_wifi_preserved(fixture)
            assert "preserved WiFi state" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-special-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        os.mkfifo(fixture.wifi / "pipe")
        result = fixture.shell(bundle_call(fixture, bundle))
        assert_wifi_preserved(fixture)
        assert "symlink or special file" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-perms-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        (fixture.wifi / "config.conf").chmod(0o666)
        result = fixture.shell(bundle_call(fixture, bundle))
        assert_wifi_preserved(fixture)
        assert "unsafe permissions" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-unreadable-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        advanced = fixture.qml / QML_FILES[0]
        advanced.chmod(0)
        result = fixture.shell(bundle_call(fixture, bundle))
        assert_wifi_preserved(fixture)
        assert "unreadable marker" in result.stdout
        advanced.chmod(0o644)

    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-symlink-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        target = fixture.victim / "wifi-real"
        shutil.copytree(fixture.wifi, target)
        shutil.rmtree(fixture.wifi)
        fixture.wifi.symlink_to(target)
        result = fixture.shell(bundle_call(fixture, bundle))
        assert (target / "config.conf").read_text() == "owner-credential\n"
        assert "refused symlink WiFi runtime dir" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-proc-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        result = fixture.shell(bundle_call(fixture, bundle),
                               {"CLEANUP_PROC_ROOT": str(fixture.base / "missing-proc")})
        assert_wifi_preserved(fixture)
        assert "not verifiably stopped" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-old-receipt-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        receipt = fixture.runtime / "combo"
        receipt.mkdir(parents=True)
        (receipt / "wifi-removed.receipt").write_text(
            "SCOURGE_WIFI_REMOVAL_RECEIPT\narchive_sha256=" + "0" * 64 + "\n")
        result = fixture.shell(remove_call(fixture))
        assert_wifi_preserved(fixture)
        assert "caller bundle proof missing" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-wifi-dangling-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        fixture.service.symlink_to(fixture.base / "missing-service")
        result = fixture.shell(bundle_call(fixture, bundle))
        assert_wifi_preserved(fixture)
        assert "service still present" in result.stdout or "unsafe WiFi service path" in result.stdout
        fixture.service.unlink()
        (fixture.qml / "HomeScreen/WifiPage.qml").symlink_to(fixture.base / "missing-page")
        result = fixture.shell(bundle_call(fixture, bundle))
        assert_wifi_preserved(fixture)
        assert "WiFi UI still present" in result.stdout
    print("PASS: removed-WiFi deletion requires caller bundle proof plus a fresh, current-state archive with member/hash readback; stale/empty old archives, assets, live processes, symlinks, special files, unsafe modes and unreadable /proc preserve owner state")


def make_wrapper_bundle(fixture: Fixture, corrupt=False, extra_member=None,
                        extra_link=None, master_text=None) -> Path:
    stick = fixture.stick
    stick.mkdir(parents=True, exist_ok=True)
    source = fixture.base / "bundle/run"
    source.mkdir(parents=True)
    (source / "run_stuff.sh").write_text(master_text or (
        '#!/bin/sh\nprintf "dummy master\\nstatus=OK\\n" > "$USB_DIR/run_stuff_result.txt"\nexit 0\n'))
    archive = stick / "appcombo.tar.gz"
    with tarfile.open(archive, "w:gz") as output:
        output.add(source / "run_stuff.sh", arcname="./run_stuff.sh")
        if extra_member:
            output.add(source / "run_stuff.sh", arcname=extra_member)
        if extra_link:
            info = tarfile.TarInfo(extra_link)
            info.type = tarfile.SYMTYPE
            info.linkname = "/etc/passwd"
            output.addfile(info)
    if corrupt:
        archive.write_bytes(archive.read_bytes() + b"corrupt")
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    cleanup_sha = hashlib.sha256(HELPER.read_bytes()).hexdigest()
    wrapper = stick / "run_stuff.sh"
    wrapper.write_text((PACKAGES / "flat_wrapper.sh").read_text()
                       .replace("@TARBALL_SHA256@", digest)
                       .replace("@RUNTIME_CLEANUP_SHA256@", cleanup_sha)
                       .replace("# @RUNTIME_CLEANUP_SHELL@", HELPER.read_text()))
    assert "@RUNTIME_CLEANUP_SHELL@" not in wrapper.read_text()
    assert not (stick / "runtime_cleanup.sh").exists()
    beep = fixture.runtime / "combo/bin/dbus-send"
    beep.parent.mkdir(parents=True, exist_ok=True)
    beep.write_text("#!/bin/sh\nexit 0\n")
    beep.chmod(0o755)
    return wrapper


def run_wrapper(fixture: Fixture, wrapper: Path, extra=None) -> subprocess.CompletedProcess:
    env = dict(os.environ,
               TMPDIR="/tmp/opencode",
               USB_DIR=str(fixture.stick),
               COMBO_RW_DIR=str(fixture.runtime / "combo"),
               COMBO_RUNTIME_ROOT=str(fixture.runtime),
               COMBO_QML_DIR=str(fixture.qml),
               CLEANUP_PROC_ROOT=str(fixture.proc),
               CLEANUP_WIFI_SERVICE=str(fixture.service))
    env.update(extra or {})
    return subprocess.run(["sh", str(wrapper)], env=env, capture_output=True,
                          text=True, timeout=90)


def test_wrapper_cleanup() -> None:
    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        wrapper = make_wrapper_bundle(fixture)
        (fixture.stick / "wifi_result.txt").write_text("status=OK\n")
        (fixture.stick / "run_stuff_result.txt").write_text("status=OK\n")
        result = run_wrapper(fixture, wrapper)
        assert result.returncode == 0, (result.stdout, result.stderr,
                                        (fixture.stick / "appcombo_install.log").read_text())
        assert not fixture.wifi.exists(), "fresh current-state archive did not authorise cleanup"
        assert not (fixture.stick / "wifi_result.txt").exists()
        assert "status=OK" in (fixture.stick / "wrapper_result.txt").read_text()
        assert list((fixture.stick / "backup").glob("wifi-pre-migration-current-*.tar.gz"))
    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-corrupt-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_bundle(fixture)
        wrapper = make_wrapper_bundle(fixture, corrupt=True)
        (fixture.stick / "wifi_result.txt").write_text("status=OK\n")
        result = run_wrapper(fixture, wrapper)
        assert result.returncode != 0
        assert_wifi_preserved(fixture)
        assert not (fixture.stick / "wifi_result.txt").exists(), "stale stage result survived early failure"
        assert "status=FAIL" in (fixture.stick / "wrapper_result.txt").read_text()
    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-marker-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_bundle(fixture)
        (fixture.qml / QML_FILES[0]).chmod(0)
        wrapper = make_wrapper_bundle(fixture)
        result = run_wrapper(fixture, wrapper)
        assert result.returncode == 0, result.stderr
        assert_wifi_preserved(fixture)
    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-traversal-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_bundle(fixture)
        wrapper = make_wrapper_bundle(fixture, extra_member="../escape")
        result = run_wrapper(fixture, wrapper)
        assert result.returncode != 0
        assert (fixture.wifi / "config.conf").exists(), "traversal archive authorised deletion"
        assert not (fixture.base / "escape").exists(), "archive traversal member was extracted"
        assert "member path rejected" in (fixture.stick / "wrapper_result.txt").read_text()
    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-dotdot-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_bundle(fixture)
        wrapper = make_wrapper_bundle(fixture, extra_member="..")
        result = run_wrapper(fixture, wrapper)
        assert result.returncode != 0
        assert (fixture.wifi / "config.conf").exists(), "bare .. archive authorised deletion"
        assert "member path rejected" in (fixture.stick / "wrapper_result.txt").read_text()
    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-link-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_bundle(fixture)
        wrapper = make_wrapper_bundle(fixture, extra_link="./link-member")
        result = run_wrapper(fixture, wrapper)
        assert result.returncode != 0
        assert (fixture.wifi / "config.conf").exists(), "link archive authorised deletion"
        assert "member type/permission rejected" in (fixture.stick / "wrapper_result.txt").read_text()
    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-rwlink-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        make_bundle(fixture)
        wrapper = make_wrapper_bundle(fixture)
        victim = fixture.base / "rw-victim"
        victim.mkdir()
        link = fixture.base / "rw-link"
        link.symlink_to(victim, target_is_directory=True)
        result = run_wrapper(fixture, wrapper, {"COMBO_RW_DIR": str(link)})
        assert result.returncode != 0
        assert not (victim / "combo.log").exists()
        assert "unsafe_symlink_destination" in (fixture.stick / "wrapper_result.txt").read_text()
    print("PASS: flat wrapper uses only its inline helper, creates a fresh current-state archive, preserves state on corrupt bundle/unreadable marker/traversal archive and clears stale stage reports before early failure")


def fake_tool(fixture: Fixture, name: str, body: str, directory_name: str) -> Path:
    directory = fixture.base / directory_name
    directory.mkdir(parents=True, exist_ok=True)
    tool = directory / name
    tool.write_text("#!/bin/sh\n" + body)
    tool.chmod(0o755)
    return directory


def test_fault_injection() -> None:
    real_tar = shutil.which("tar")
    real_chmod = shutil.which("chmod")
    assert real_tar and real_chmod

    with tempfile.TemporaryDirectory(prefix="cleanup-fault-extract-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        (fixture.wifi / "config.conf").write_text("")  # empty original file
        fake = fake_tool(fixture, "tar",
                         'if [ "$1" = "-xOzf" ]; then exit 1; fi\n'
                         'exec "$TAR_REAL" "$@"\n', "faultbin")
        result = fixture.shell(bundle_call(fixture, bundle),
                               {"PATH": str(fake) + ":" + os.environ["PATH"],
                                "TAR_REAL": real_tar})
        assert (fixture.wifi / "config.conf").exists(), "failed extraction authorised deletion"
        assert "private archive extraction failed" in result.stdout, result.stdout
        assert not list((fixture.stick / "backup").glob("wifi-pre-migration-current-*.tar.gz"))

    with tempfile.TemporaryDirectory(prefix="cleanup-fault-chmod-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        fake = fake_tool(fixture, "chmod",
                         'if [ "$1" = "700" ]; then exit 1; fi\n'
                         'exec "$CHMOD_REAL" "$@"\n', "faultbin")
        result = fixture.shell(bundle_call(fixture, bundle),
                               {"PATH": str(fake) + ":" + os.environ["PATH"],
                                "CHMOD_REAL": real_chmod})
        assert (fixture.wifi / "config.conf").exists(), "chmod denial authorised deletion"
        assert "cannot make stick backup directory private" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-fault-chmod600-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        fake = fake_tool(fixture, "chmod",
                         'if [ "$1" = "600" ]; then exit 1; fi\n'
                         'exec "$CHMOD_REAL" "$@"\n', "faultbin")
        result = fixture.shell(bundle_call(fixture, bundle),
                               {"PATH": str(fake) + ":" + os.environ["PATH"],
                                "CHMOD_REAL": real_chmod})
        assert (fixture.wifi / "config.conf").exists(), "chmod 600 denial authorised deletion"
        assert "private archive mode 600" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-fault-mode-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        fake = fake_tool(fixture, "stat", 'printf "777\\n"\n', "faultbin")
        result = fixture.shell(bundle_call(fixture, bundle),
                               {"PATH": str(fake) + ":" + os.environ["PATH"]})
        assert (fixture.wifi / "config.conf").exists(), "wrong private mode authorised deletion"
        assert "cannot make stick backup directory private" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-fault-type-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        (fixture.wifi / "config.conf").write_text("")
        fake = fake_tool(fixture, "tar", r'''if [ "$1" = "-czf" ]; then
exec python3 - "$2" "$4" "$5" <<'PY'
import os, sys, tarfile
out, root, base = sys.argv[1:4]
with tarfile.open(out, "w:gz") as archive:
    for dirpath, dirnames, filenames in os.walk(os.path.join(root, base)):
        rel = os.path.relpath(dirpath, root)
        info = tarfile.TarInfo(rel + "/")
        info.type = tarfile.DIRTYPE
        info.mode = 0o700
        archive.addfile(info)
        for name in filenames:
            member = os.path.join(rel, name)
            path = os.path.join(dirpath, name)
            if member == "wifi/config.conf":
                info = tarfile.TarInfo(member)
                info.type = tarfile.SYMTYPE
                info.linkname = "/etc/passwd"
                info.mode = 0o600
                archive.addfile(info)
            else:
                archive.add(path, arcname=member)
PY
fi
exec "$TAR_REAL" "$@"
''', "faultbin")
        result = fixture.shell(bundle_call(fixture, bundle),
                               {"PATH": str(fake) + ":" + os.environ["PATH"],
                                "TAR_REAL": real_tar})
        assert (fixture.wifi / "config.conf").exists(), "link/type substitution authorised deletion"
        assert "private archive member type rejected" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-fault-type-dir-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        (fixture.wifi / "config.conf").write_text("")
        fake = fake_tool(fixture, "tar", r'''if [ "$1" = "-czf" ]; then
exec python3 - "$2" "$4" "$5" <<'PY'
import os, sys, tarfile
out, root, base = sys.argv[1:4]
with tarfile.open(out, "w:gz") as archive:
    for dirpath, dirnames, filenames in os.walk(os.path.join(root, base)):
        rel = os.path.relpath(dirpath, root)
        info = tarfile.TarInfo(rel + "/")
        info.type = tarfile.DIRTYPE
        info.mode = 0o700
        archive.addfile(info)
        for name in filenames:
            member = os.path.join(rel, name)
            path = os.path.join(dirpath, name)
            if member == "wifi/config.conf":
                info = tarfile.TarInfo(member)
                info.type = tarfile.DIRTYPE
                info.mode = 0o700
                archive.addfile(info)
            else:
                archive.add(path, arcname=member)
PY
fi
exec "$TAR_REAL" "$@"
''', "faultbin")
        result = fixture.shell(bundle_call(fixture, bundle),
                               {"PATH": str(fake) + ":" + os.environ["PATH"],
                                "TAR_REAL": real_tar})
        assert (fixture.wifi / "config.conf").exists(), "directory/type substitution authorised deletion"
        assert "private archive member type rejected" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-fault-collision-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        probe = fixture.shell(
            'runtime_cleanup_archive_current "$RUNTIME" "$STICK"; '
            'printf "SHA=%s\\n" "$RUNTIME_CLEANUP_ARCHIVE_SHA"')
        sha = [line.split("=", 1)[1] for line in probe.stdout.splitlines()
               if line.startswith("SHA=")][0]
        final = fixture.stick / f"backup/wifi-pre-migration-current-{sha}.tar.gz"
        assert final.is_file()
        final.unlink()
        final.symlink_to(fixture.victim / "keep.txt")
        result = fixture.shell(bundle_call(fixture, bundle))
        assert (fixture.wifi / "config.conf").exists(), "symlink collision authorised deletion"
        assert "collision" in result.stdout
        final.unlink()
        final.mkdir()
        result = fixture.shell(bundle_call(fixture, bundle))
        assert (fixture.wifi / "config.conf").exists(), "directory collision authorised deletion"
        assert "collision" in result.stdout

    with tempfile.TemporaryDirectory(prefix="cleanup-fault-proc-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bundle = make_bundle(fixture)
        unreadable = fixture.proc / "7000"
        unreadable.mkdir()
        (unreadable / "exe").symlink_to(fixture.runtime / "wifi/wifi_daemon")
        unreadable.chmod(0)
        result = fixture.shell(
            'runtime_cleanup_process_alive "$RUNTIME" wifi wifi_daemon; '
            'printf "RC=%s\\n" "$?"')
        assert "RC=2" in result.stdout, result.stdout
        result = fixture.shell(bundle_call(fixture, bundle))
        assert (fixture.wifi / "config.conf").exists(), "unreadable extant PID authorised deletion"
        assert "not verifiably stopped" in result.stdout
        unreadable.chmod(0o755)
        (unreadable / "exe").unlink()
        unreadable.rmdir()
        result = fixture.shell(
            'runtime_cleanup_process_alive "$RUNTIME" wifi wifi_daemon; '
            'printf "RC=%s\\n" "$?"')
        assert "RC=1" in result.stdout, result.stdout

    print("PASS: fault injection preserves credentials for failed/partial tar extraction, chmod/stat denial, archived link/type substitution and unreadable extant PID evidence; vanished PID is absent")


test_bounded_logs()
test_stale_temps()
test_removed_wifi_evidence()
test_wrapper_cleanup()
test_fault_injection()
print("PASS: runtime cleanup safety fixtures are host-isolated and preserve owner data")


def run_wrapper_with_pid(fixture: Fixture, wrapper: Path, preseed: str = "",
                         extra=None) -> subprocess.CompletedProcess:
    env = dict(os.environ,
               TMPDIR="/tmp/opencode",
               USB_DIR=str(fixture.stick),
               COMBO_RW_DIR=str(fixture.runtime / "combo"),
               COMBO_RUNTIME_ROOT=str(fixture.runtime),
               COMBO_QML_DIR=str(fixture.qml),
               CLEANUP_PROC_ROOT=str(fixture.proc),
               CLEANUP_WIFI_SERVICE=str(fixture.service),
               WRAPPER=str(wrapper),
               STICK=str(fixture.stick),
               VICTIM=str(fixture.victim))
    env.update(extra or {})
    script = (
        'pid=$$\n'
        'printf "%s\\n" "$pid" > "$STICK/wrapper.pid"\n'
        f'{preseed}\n'
        'exec sh "$WRAPPER"\n'
    )
    return subprocess.run(["sh", "-c", script], env=env, capture_output=True,
                          text=True, timeout=120)


def test_wrapper_readback() -> None:
    master = (
        '#!/bin/sh\n'
        'mkdir -p "$USB_DIR/combo/battery/qml_before"\n'
        'printf "SNAP-INNER\\n" > "$USB_DIR/combo/battery/qml_before/snap.txt"\n'
        'printf "SCOURGE_COMBO\\nstatus=OK\\n" > "$USB_DIR/run_stuff_result.txt"\n'
        'exit 0\n')
    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-readback-link-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        wrapper = make_wrapper_bundle(fixture, master_text=master)
        victim = fixture.base / "readback-victim"
        victim.write_text("OWNER-READBACK\n")
        preseed = (
            'rb="$STICK/qml_readback_$pid"\n'
            'mkdir -p "$rb/battery/qml_before"\n'
            'ln -s "$VICTIM" "$rb/battery/qml_before/snap.txt"\n')
        result = run_wrapper_with_pid(fixture, wrapper, preseed)
        assert result.returncode != 0
        assert victim.read_text() == "OWNER-READBACK\n"
        assert "status=FAIL" in (fixture.stick / "wrapper_result.txt").read_text()
        pid = (fixture.stick / "wrapper.pid").read_text().strip()
        assert (fixture.stick / f"qml_readback_{pid}").exists()

    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-readback-dir-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        wrapper = make_wrapper_bundle(fixture, master_text=master)
        preseed = (
            'rb="$STICK/qml_readback_$pid"\n'
            'mkdir -p "$rb"\n'
            'printf "OWNER-READBACK\\n" > "$rb/owner.txt"\n')
        result = run_wrapper_with_pid(fixture, wrapper, preseed)
        assert result.returncode != 0
        root = fixture.stick / ("qml_readback_" + (fixture.stick / "wrapper.pid").read_text().strip())
        assert (root / "owner.txt").read_text() == "OWNER-READBACK\n"
        assert "status=FAIL" in (fixture.stick / "wrapper_result.txt").read_text()

    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-readback-ok-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        wrapper = make_wrapper_bundle(fixture, master_text=master)
        result = run_wrapper_with_pid(fixture, wrapper)
        assert result.returncode == 0, (result.stdout, result.stderr)
        pid = (fixture.stick / "wrapper.pid").read_text().strip()
        snap = fixture.stick / f"qml_readback_{pid}/battery/qml_before/snap.txt"
        assert snap.read_text() == "SNAP-INNER\n"
        assert "status=OK" in (fixture.stick / "wrapper_result.txt").read_text()

    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-readback-fail-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        bad_master = (
            '#!/bin/sh\n'
            'mkdir -p "$USB_DIR/combo/battery"\n'
            'mkfifo "$USB_DIR/combo/battery/qml_before"\n'
            'printf "SCOURGE_COMBO\\nstatus=OK\\n" > "$USB_DIR/run_stuff_result.txt"\n'
            'exit 0\n')
        wrapper = make_wrapper_bundle(fixture, master_text=bad_master)
        result = run_wrapper_with_pid(fixture, wrapper)
        assert result.returncode != 0
        assert "status=FAIL" in (fixture.stick / "wrapper_result.txt").read_text()

    def snapshot_master(extra: str = "", nested: bool = False) -> str:
        nested_setup = 'mkdir -p "$USB_DIR/combo/battery/qml_before/nested"\n' if nested else ""
        return ("#!/bin/sh\n"
                'mkdir -p "$USB_DIR/combo/battery/qml_before"\n' + nested_setup +
                'printf "SNAP-INNER\\n" > "$USB_DIR/combo/battery/qml_before/snap.txt"\n' +
                extra + "\n"
                'printf "SCOURGE_COMBO\\nstatus=OK\\n" > "$USB_DIR/run_stuff_result.txt"\n'
                'exit 0\n')

    def assert_no_secret(stick: Path) -> None:
        for root in stick.glob("qml_readback_*"):
            for path in root.rglob("*"):
                mode = path.lstat().st_mode if path.exists() or path.is_symlink() else 0
                if not stat.S_ISREG(mode) or path.is_symlink():
                    continue
                assert b"OWNER-SECRET" not in path.read_bytes(), path

    invalid_setups = (
        ("symlink", 'ln -s "$OWNER_SECRET" "$USB_DIR/combo/battery/qml_before/link.txt"'),
        ("fifo", 'mkfifo "$USB_DIR/combo/battery/qml_before/fifo"'),
        ("hardlink", 'ln "$OWNER_SECRET" "$USB_DIR/combo/battery/qml_before/leak.txt"'),
        ("socket", 'mv "$SOURCE_SOCKET" "$USB_DIR/combo/battery/qml_before/source.sock"'),
        ("unreadable", 'printf "NO\\n" > "$USB_DIR/combo/battery/qml_before/bad.txt" && chmod 000 "$USB_DIR/combo/battery/qml_before/bad.txt"'),
        ("newline-empty-dir", "name=$(printf 'bad\\nname')\nmkdir \"$USB_DIR/combo/battery/qml_before/$name\""),
        ("tab-empty-dir", "name=$(printf 'bad\\tname')\nmkdir \"$USB_DIR/combo/battery/qml_before/$name\""),
        ("newline-filedir", "mkdir -p \"$USB_DIR/combo/battery/qml_before/sub\"\nname=$(printf 'sub\\nname')\nmkdir \"$USB_DIR/combo/battery/qml_before/sub/$name\""),
        ("tab-filedir", "mkdir -p \"$USB_DIR/combo/battery/qml_before/sub\"\nname=$(printf 'sub\\tname')\nmkdir \"$USB_DIR/combo/battery/qml_before/sub/$name\""),
        ("newline-file", "name=$(printf 'bad\\nname')\nprintf 'DATA\\n' > \"$USB_DIR/combo/battery/qml_before/$name\""),
        ("tab-file", "name=$(printf 'bad\\tname')\nprintf 'DATA\\n' > \"$USB_DIR/combo/battery/qml_before/$name\""),
    )
    for label, setup in invalid_setups:
        with tempfile.TemporaryDirectory(prefix=f"cleanup-wrapper-nested-{label}-", dir="/tmp/opencode") as d:
            fixture = Fixture(Path(d))
            marker_absent(fixture)
            fixture.stick.mkdir(parents=True, exist_ok=True)
            secret = fixture.stick / "owner-secret"
            secret.write_text("OWNER-SECRET\n")
            if label == "socket":
                short = fixture.stick
                old_cwd = os.getcwd()
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                try:
                    os.chdir(short)
                    sock.bind("source.sock")
                finally:
                    os.chdir(old_cwd)
                    sock.close()
            wrapper = make_wrapper_bundle(fixture, master_text=snapshot_master(setup))
            result = run_wrapper_with_pid(
                fixture, wrapper,
                extra={"OWNER_SECRET": str(secret),
                       "SOURCE_SOCKET": str(fixture.stick / "source.sock")})
            assert result.returncode != 0, (label, result.stdout, result.stderr)
            assert secret.read_text() == "OWNER-SECRET\n"
            assert "status=FAIL" in (fixture.stick / "wrapper_result.txt").read_text()
            assert_no_secret(fixture.stick)

    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-nested-valid-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        wrapper = make_wrapper_bundle(
            fixture,
            master_text=snapshot_master(
                'printf "DEEP\\n" > "$USB_DIR/combo/battery/qml_before/nested/deep.qml"',
                nested=True))
        result = run_wrapper_with_pid(fixture, wrapper)
        assert result.returncode == 0, (result.stdout, result.stderr)
        pid = (fixture.stick / "wrapper.pid").read_text().strip()
        root = fixture.stick / f"qml_readback_{pid}/battery/qml_before"
        assert (root / "snap.txt").read_text() == "SNAP-INNER\n"
        assert (root / "nested/deep.qml").read_text() == "DEEP\n"
        assert "status=OK" in (fixture.stick / "wrapper_result.txt").read_text()

    with tempfile.TemporaryDirectory(prefix="cleanup-wrapper-copy-partial-", dir="/tmp/opencode") as d:
        fixture = Fixture(Path(d))
        marker_absent(fixture)
        wrapper = make_wrapper_bundle(fixture, master_text=snapshot_master())
        fault = fixture.base / "faultbin"
        fault.mkdir()
        real_cp = shutil.which("cp")
        (fault / "cp").write_text(
            "#!/bin/sh\n"
            f"case \"$*\" in *'{fixture.stick}/qml_readback_'*) exit 1;; esac\n"
            f"exec '{real_cp}' \"$@\"\n")
        (fault / "cp").chmod(0o755)
        result = run_wrapper_with_pid(
            fixture, wrapper, extra={"COMBO_PATH": str(fault) + ":" + os.environ["PATH"]})
        assert result.returncode != 0
        assert "status=FAIL" in (fixture.stick / "wrapper_result.txt").read_text()
        assert_no_secret(fixture.stick)

    print("PASS: wrapper readback validates full source trees, rejects nested links/FIFO/socket/hardlink/unreadable/special names and reports FAIL")


test_wrapper_readback()
