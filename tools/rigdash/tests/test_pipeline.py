# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Pipeline-table and status-logic tests.

    .venv/bin/python -m unittest discover -s tools/rigdash/tests -v
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from tools.rigdash import pipeline                      # noqa: E402
from tools.rigdash.config import REPO, Settings         # noqa: E402


def settings(root: Path, secrets: dict[str, str] | None = None) -> Settings:
    values = dict(secrets or {})
    return Settings(values, root)


def fill(*keys: str) -> dict[str, str]:
    values = {
        "RIG_HOSTNAME": "rig", "RIG_USER": "pi", "RIG_PASSWORD": "pw",
        "SSH_PUBKEY": "ssh-ed25519 AAAA", "WIFI_COUNTRY": "HU",
        "WIFI_SSIDS": "home home-5G", "WIFI_PSK": "psk",
        "RIG_AP_SSID": "rig-setup", "RIG_AP_PASSPHRASE": "setup-pass",
    }
    return {k: values[k] for k in keys} if keys else values


class TableTests(unittest.TestCase):
    def test_every_script_exists(self) -> None:
        for step in pipeline.STEPS:
            if step.script is None:
                continue
            with self.subTest(step=step.id):
                self.assertTrue((REPO / step.script).is_file(),
                                f"{step.id}: missing {step.script}")

    def test_command_passes_the_device_placeholder_through(self) -> None:
        self.assertEqual(pipeline.command(pipeline.BY_ID["format_usb"]),
                         ["bash", "scripts/14_format_usb.sh", "--device", "$DEVICE"])

    def test_ids_are_unique(self) -> None:
        ids = [step.id for step in pipeline.STEPS]
        self.assertEqual(len(ids), len(set(ids)))

    def test_python_scripts_are_not_run_through_bash(self) -> None:
        self.assertEqual(pipeline.command(pipeline.BY_ID["patch"]),
                         ["scripts/03_patch_payload.py"])


class EvaluateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def touch(self, *paths: str) -> None:
        for rel in paths:
            target = self.root / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("")

    def test_blocked_lists_the_missing_inputs(self) -> None:
        result = pipeline.evaluate(pipeline.BY_ID["decrypt"],
                                   settings(self.root, fill()))
        self.assertEqual(result["status"], pipeline.BLOCKED)
        self.assertIn("input/mango-rootfs.tar.gz", result["detail"])

    def test_ready_when_inputs_present_outputs_absent(self) -> None:
        self.touch("input/mango-rootfs.tar.gz", "config/key.local")
        result = pipeline.evaluate(pipeline.BY_ID["decrypt"],
                                   settings(self.root, fill()))
        self.assertEqual(result["status"], pipeline.READY)
        self.assertIn("work/stock-plain.tar.gz", result["detail"])

    def test_done_when_outputs_present(self) -> None:
        self.touch("input/mango-rootfs.tar.gz", "config/key.local",
                   "work/stock-plain.tar.gz")
        result = pipeline.evaluate(pipeline.BY_ID["decrypt"],
                                   settings(self.root, fill()))
        self.assertEqual(result["status"], pipeline.DONE)

    def test_secrets_gate_blocks_the_sd_step(self) -> None:
        self.touch("scripts/firstrun.sh.tmpl")
        step = pipeline.BY_ID["sd_boot"]
        blocked = pipeline.evaluate(step, settings(self.root, {}))
        self.assertEqual(blocked["status"], pipeline.BLOCKED)
        self.assertIn("rig.local.env", blocked["detail"])
        ready = pipeline.evaluate(step, settings(self.root, fill()))
        self.assertEqual(ready["status"], pipeline.READY)

    def test_placeholder_secrets_do_not_count(self) -> None:
        values = fill()
        values["WIFI_PSK"] = "<your-psk>"
        self.assertFalse(settings(self.root, values).secrets_ready)

    def test_secret_status_never_returns_values(self) -> None:
        status = settings(self.root, fill()).secret_status()
        self.assertEqual(set(status.values()), {True})
        self.assertEqual(set(status), set(pipeline_secret_keys()))

    def test_remote_step_without_rig_is_not_reported_done(self) -> None:
        self.touch("out/usb.img", "out/stage1.te2", "out/regions.json")
        result = pipeline.evaluate(pipeline.BY_ID["deploy_pi"],
                                   settings(self.root, fill()), remote=None)
        self.assertNotEqual(result["status"], pipeline.DONE)
        self.assertIn("unreachable", result["detail"])

    def test_remote_step_uses_the_probe(self) -> None:
        self.touch("out/usb.img", "out/stage1.te2", "out/regions.json")
        probe = {"scourge/build/usb.img": True, "scourge/build/stage1.te2": True}
        result = pipeline.evaluate(pipeline.BY_ID["deploy_pi"],
                                   settings(self.root, fill()), remote=probe)
        self.assertEqual(result["status"], pipeline.DONE)

    def test_remote_step_reports_absent_artifacts(self) -> None:
        self.touch("out/usb.img", "out/stage1.te2", "out/regions.json")
        probe = {"scourge/build/usb.img": True, "scourge/build/stage1.te2": False}
        result = pipeline.evaluate(pipeline.BY_ID["deploy_pi"],
                                   settings(self.root, fill()), remote=probe)
        self.assertEqual(result["status"], pipeline.READY)
        self.assertIn("scourge/build/stage1.te2", result["detail"])


class ImageTests(unittest.TestCase):
    """Image listing and the atomic serve pointer."""

    def setUp(self) -> None:
        from tools.rigdash.pi import rig_wifi_agent as agent
        self.agent = agent
        self._tmp = tempfile.TemporaryDirectory()
        root = Path(self._tmp.name)
        self.dir = root / "build"          # the depot
        self.elsewhere = root / "elsewhere"  # not a depot, not a mount
        self.dir.mkdir()
        self.elsewhere.mkdir()
        self._saved = (agent.BUILD_DIR, agent.SERVE_LINK, agent.HASH_CACHE,
                       agent._usb_mounts)
        agent.BUILD_DIR = str(self.dir)
        agent.SERVE_LINK = str(self.dir / "serve.img")
        agent.HASH_CACHE = str(self.dir / ".hashes.json")
        agent._usb_mounts = lambda: []      # the host's own mounts are not the rig
        (self.dir / "build-a.img").write_bytes(b"a" * 2048)
        (self.dir / "stock-recovery.img").write_bytes(b"b" * 1024)
        (self.dir / "notes.txt").write_text("not an image")

    def tearDown(self) -> None:
        (self.agent.BUILD_DIR, self.agent.SERVE_LINK, self.agent.HASH_CACHE,
         self.agent._usb_mounts) = self._saved
        self._tmp.cleanup()

    def test_lists_only_images_and_flags_recovery(self) -> None:
        names = {row["name"] for row in self.agent.images()}
        self.assertEqual(names, {"build-a.img", "stock-recovery.img"})
        recovery = {r["name"]: r["recovery"] for r in self.agent.images()}
        self.assertTrue(recovery["stock-recovery.img"])
        self.assertFalse(recovery["build-a.img"])

    def test_rejects_path_traversal(self) -> None:
        for bad in ("../etc/passwd", "/etc/passwd", "", ".hashes.json"):
            with self.subTest(name=bad):
                ok, _message = self.agent.image_serve(bad)
                self.assertFalse(ok)

    def test_serve_refuses_while_the_link_is_up(self) -> None:
        self.agent.usb_state = lambda: {"udc": ["fe980000.usb"], "driver": ["dwc2"],
                                        "connected": True, "mode": "peripheral"}
        ok, message = self.agent.image_serve("build-a.img")
        self.assertFalse(ok)
        self.assertIn("Normal mode", message)

    def test_serve_moves_the_pointer(self) -> None:
        self.agent.usb_state = lambda: {"udc": [], "driver": [], "connected": False,
                                        "mode": "host"}
        ok, _message = self.agent.image_serve("build-a.img")
        self.assertTrue(ok)
        self.assertTrue(os.path.islink(self.agent.SERVE_LINK))
        served = [r["name"] for r in self.agent.images() if r["served"]]
        self.assertEqual(served, ["build-a.img"])
        ok, _message = self.agent.image_serve("stock-recovery.img")
        self.assertTrue(ok)
        served = [r["name"] for r in self.agent.images() if r["served"]]
        self.assertEqual(served, ["stock-recovery.img"])

    def test_media_images_are_listed_but_not_deletable(self) -> None:
        usb = self.elsewhere / "stick"
        (usb / "sub").mkdir(parents=True)
        (usb / "stick-image.img").write_bytes(b"c" * 512)
        (usb / "sub" / "nested.img").write_bytes(b"d" * 256)
        (usb / "sub" / "a" / "b").mkdir(parents=True)
        (usb / "sub" / "a" / "b" / "deep.img").write_bytes(b"e")
        self.agent._usb_mounts = lambda: [{"target": str(usb), "fstype": "exfat",
                                           "source": "/dev/sda1"}]
        rows = {row["name"]: row for row in self.agent.images()}
        self.assertIn("stick-image.img", rows)
        self.assertIn("nested.img", rows)
        self.assertNotIn("deep.img", rows)       # two levels deep, no deeper
        self.assertFalse(rows["stick-image.img"]["deletable"])
        ok, _message = self.agent.image_delete("stick-image.img")
        self.assertFalse(ok)
        self.assertIn("external media", _message)

    def test_serve_by_absolute_path_only_for_listed_files(self) -> None:
        self.agent.usb_state = lambda: {"udc": [], "driver": [], "connected": False,
                                        "mode": "host"}
        outside = self.elsewhere / "outside.img"
        outside.write_bytes(b"x")
        ok, _message = self.agent.image_serve(str(outside))
        self.assertFalse(ok)                     # not in the scan, not servable
        ok, _message = self.agent.image_serve(str(self.dir / "build-a.img"))
        self.assertTrue(ok)

    def test_hash_is_cached_with_the_size_it_was_taken_at(self) -> None:
        ok, _message = self.agent.image_hash("build-a.img")
        self.assertTrue(ok)
        row = next(r for r in self.agent.images() if r["name"] == "build-a.img")
        self.assertEqual(len(row["sha256"]), 64)
        self.assertTrue(Path(self.agent.HASH_CACHE).is_file())


class WifiAgentTests(unittest.TestCase):
    """Input validation rejects before nmcli is ever invoked."""

    def setUp(self) -> None:
        from tools.rigdash.pi import rig_wifi_agent as agent
        self.agent = agent

    def test_bad_ssid_refused(self) -> None:
        for bad in ("", "x" * 33, "two\nlines"):
            with self.subTest(ssid=bad):
                ok, message = self.agent.join(bad, "password123")
                self.assertFalse(ok)
                self.assertIn("SSID", message)

    def test_short_passphrase_refused(self) -> None:
        ok, message = self.agent.join("home", "short")
        self.assertFalse(ok)
        self.assertIn("passphrase", message)

    def test_accepts_a_normal_pair_and_an_open_network(self) -> None:
        for psk in ("correct horse", "a" * 63, "0" * 64, ""):
            with self.subTest(psk=psk[:8]):
                self.assertRegex(psk, self.agent.PSK_RE) if psk else None
                self.assertTrue(self.agent.PSK_RE.match(psk) or psk == "")

    def _state(self, ap_active: bool, connected: bool = True) -> dict:
        return {
            "wifi": {"ap_active": ap_active, "connected": connected, "ssid": "home",
                     "ip": "10.42.0.5", "state": "100 (connected)"},
            "usb": {"udc": ["fe980000.usb"], "driver": ["dwc2"], "connected": True,
                    "gadget": "", "mode": "peripheral"},
            "hostname": "rig", "kernel": "6.18.50+rpi", "uptime_h": 1.5,
            "free_gb": 20.0, "files": [],
        }

    def test_body_offers_no_wifi_form_when_not_in_ap_mode(self) -> None:
        body = self.agent.render_body(self._state(ap_active=False), [])
        self.assertNotIn('action="/connect"', body)
        self.assertIn("Connected to home", body)

    def test_body_offers_the_join_form_in_ap_mode(self) -> None:
        self.agent.scan = lambda timeout=25: []       # no radio in a unit test
        body = self.agent.render_body(self._state(ap_active=True), [])
        self.assertIn('action="/connect"', body)
        self.assertIn('name="token"', body)

    def test_body_lists_steps_and_escapes_them(self) -> None:
        steps = [{"id": "x", "title": "<script>", "status": "todo", "detail": "a & b"}]
        body = self.agent.render_body(self._state(False), steps)
        self.assertIn("&lt;script&gt;", body)
        # exactly two <script> tags in the page, both ours: the upload helper and
        # the pass poller. Step text that came through a title never adds one.
        self.assertEqual(body.count("<script>"), 2)
        self.assertIn("a &amp; b", body)

    def test_usb_mode_is_written_and_validated(self) -> None:
        ok, message = self.agent.usb_set_mode("sideways")
        self.assertFalse(ok)
        self.assertIn("peripheral", message)


class ToctouTests(unittest.TestCase):
    """Pass parsing: the rate on the page comes from the plugin's own numbers."""

    SESSION = """\
[toctou] region [5505024,884631203) mode=benign_then_evil switch_after_pass=2 evil=/media/RIGIMG/stage1.te2
[toctou] open baseline=/media/RIGIMG/usb.img size=28894603492 regions=1
[toctou] ===== SESSION START sess=aa11 boot=x pid=875 switch_after_pass=2 =====
[toctou] region@5505024 PASS #1 END reads=879 bytes_genuine=879126179 bytes_evil=0 served=GENUINE span=[5505024,884631203] dur=32.50s
[toctou] *** region@5505024 SWITCH at pass #2 (mode=benign_then_evil) ***
[toctou] region@5505024 PASS #2 END reads=879 bytes_genuine=0 bytes_evil=879126179 served=EVIL span=[5505024,884631203] dur=40.00s
"""

    def setUp(self) -> None:
        from tools.rigdash.pi import rig_wifi_agent as agent
        self.agent = agent
        self._tmp = tempfile.TemporaryDirectory()
        self.log = Path(self._tmp.name) / "toctou.log"
        self._saved = agent.TOCTOU_LOG
        agent.TOCTOU_LOG = str(self.log)

    def tearDown(self) -> None:
        self.agent.TOCTOU_LOG = self._saved
        self._tmp.cleanup()

    def _state(self, text: str) -> dict:
        self.log.write_text(text)
        return self.agent.toctou_state()

    def test_missing_log_is_reported_not_raised(self) -> None:
        state = self.agent.toctou_state()
        self.assertFalse(state["exists"])
        self.assertIn("No plugin log yet", self.agent.render_toctou(state))

    def test_region_and_passes_are_parsed_with_rates(self) -> None:
        state = self._state(self.SESSION)
        self.assertTrue(state["exists"])
        self.assertEqual(state["serving"], "usb.img")
        self.assertEqual(state["size"], 28894603492)
        self.assertEqual(state["regions"][0]["off"], 5505024)
        self.assertEqual(state["regions"][0]["len"], 884631203 - 5505024)
        self.assertEqual(state["regions"][0]["evil"], "stage1.te2")
        self.assertTrue(state["switched"])
        self.assertEqual([p["served"] for p in state["passes"]], ["GENUINE", "EVIL"])
        # 879 126 179 bytes over the plugin's own dur
        self.assertAlmostEqual(state["passes"][0]["mbps"], 27.1, places=1)
        self.assertAlmostEqual(state["passes"][1]["mbps"], 22.0, places=1)
        html = self.agent.render_toctou(state)
        self.assertIn("27.1 MB/s", html)
        self.assertIn("switch after pass 2", html)

    def test_pass_closed_by_session_end_is_flagged_not_rated(self) -> None:
        """A real run served 862 MB over 3205 s because the plugin kept the pass
        open until shutdown; that is idle time, not a 0.3 MB/s transfer."""
        text = self.SESSION + "[toctou] ===== SESSION END sess=aa11 =====\n"
        state = self._state(text)
        self.assertTrue(state["passes"][-1]["closed_at_session_end"])
        self.assertNotIn("closed_at_session_end", state["passes"][0])
        html = self.agent.render_toctou(state)
        self.assertIn("until session end", html)
        self.assertIn("lower bound", html)

    def test_only_the_newest_session_counts(self) -> None:
        stale = ("[toctou] *** region@5505024 SWITCH at pass #2 (mode=benign_then_evil) ***\n"
                 "[toctou] ===== SESSION START sess=bb22 boot=x pid=900 switch_after_pass=2 =====\n"
                 "[toctou] region@5505024 PASS #1 END reads=879 bytes_genuine=879126179 "
                 "bytes_evil=0 served=GENUINE span=[5505024,884631203] dur=32.50s\n")
        state = self._state(stale)
        self.assertFalse(state["switched"])
        self.assertEqual([p["served"] for p in state["passes"]], ["GENUINE"])


class TweaksTests(unittest.TestCase):
    """The disclaimer stick on the rig page: id, payload, manifest, last result."""

    def setUp(self) -> None:
        from tools.rigdash.pi import rig_wifi_agent as agent
        self.agent = agent
        self._tmp = tempfile.TemporaryDirectory()
        self.stick = Path(self._tmp.name) / "stick"
        self.stick.mkdir()
        self._saved = agent._usb_mounts
        agent._usb_mounts = lambda: [{"target": str(self.stick), "fstype": "exfat",
                                      "source": "/dev/sdz1"}]
        (self.stick / "run_stuff.sh").write_text(
            "#!/bin/sh\n# TWEAKS_DISCLAIMER_ID=abc123def4567890\nset -u\n")
        for base in ("qml_eco3.0", "new_qml"):
            target = self.stick / "payload" / "qml" / "AppStandbyClock" / base
            target.mkdir(parents=True)
            for name in ("main.qml", "Screen_AppStandbyClock_Confirm.qml"):
                (target / name).write_text("// TWEAKS_UNGATED_CONFIRM\n")
        lines = []
        for path in sorted(self.stick.joinpath("payload").rglob("*.qml")):
            import hashlib
            rel = path.relative_to(self.stick)
            lines.append(f"{hashlib.md5(path.read_bytes()).hexdigest()}  {rel.as_posix()}")
        (self.stick / "manifest.md5").write_text("\n".join(lines) + "\n")

    def tearDown(self) -> None:
        self.agent._usb_mounts = self._saved
        self._tmp.cleanup()

    def test_reads_id_payload_and_manifest(self) -> None:
        state = self.agent.tweaks_state()
        self.assertTrue(state["found"])
        stick = state["sticks"][0]
        self.assertEqual(stick["id"], "abc123def4567890")
        self.assertEqual(stick["bases"], ["new_qml", "qml_eco3.0"])
        self.assertEqual(stick["files"], 4)
        self.assertEqual(stick["manifest_failed"], 0)
        self.assertEqual(stick["result"], {})
        html = self.agent.render_tweaks(state)
        self.assertIn("manifest ok", html)
        self.assertIn("not run yet", html)
        self.assertIn("next boot", html)

    def test_tampered_payload_is_reported(self) -> None:
        target = (self.stick / "payload" / "qml" / "AppStandbyClock" /
                  "qml_eco3.0" / "main.qml")
        target.write_text("// changed after the manifest was written\n")
        stick = self.agent.tweaks_state()["sticks"][0]
        self.assertEqual(stick["manifest_failed"], 1)
        self.assertTrue(any("mismatch" in p for p in stick["problems"]))
        self.assertIn("problem", self.agent.render_tweaks({"found": True, "sticks": [stick]}))

    def test_last_result_is_shown(self) -> None:
        (self.stick / "run_stuff_result.txt").write_text(
            "SCOURGE_DISCLAIMER\ntime=2026-11-21 14:00:00 UTC\nuid=0\n"
            "bases=qml_eco3.0,new_qml\ndisclaimer=next-boot\nstatus=OK\n")
        stick = self.agent.tweaks_state()["sticks"][0]
        self.assertEqual(stick["result"]["status"], "OK")
        html = self.agent.render_tweaks({"found": True, "sticks": [stick]})
        self.assertIn("status", html)
        self.assertIn("disclaimer", html)

    def test_nothing_plugged_in_is_quiet(self) -> None:
        self.agent._usb_mounts = lambda: []
        state = self.agent.tweaks_state()
        self.assertFalse(state["found"])
        self.assertIn("No disclaimer stick", self.agent.render_tweaks(state))


def pipeline_secret_keys() -> tuple[str, ...]:
    from tools.rigdash.config import SECRET_KEYS
    return SECRET_KEYS


if __name__ == "__main__":
    unittest.main()
