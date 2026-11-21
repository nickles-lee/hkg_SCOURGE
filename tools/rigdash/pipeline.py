# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""The build and provisioning pipeline, as data.

Every step names the script that performs it, what it needs on disk and what it
leaves behind. Status is then a pure function of those paths — the same
"newer than its inputs" idea make uses, without a build file to maintain. The
table is deliberately plain data: when a script changes its output path, fix it
here, not in the code that renders it.
"""
from __future__ import annotations

import shutil
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path

DONE = "done"
READY = "ready"
BLOCKED = "blocked"
MANUAL = "manual"

_STATUS_ORDER = {DONE: 0, READY: 1, BLOCKED: 2, MANUAL: 3}


@dataclass(frozen=True)
class Step:
    id: str
    title: str
    script: str | None = None            # relative to the repo root
    args: tuple[str, ...] = ()
    requires: tuple[str, ...] = ()       # repo-relative paths that must exist
    produces: tuple[str, ...] = ()       # repo-relative paths the script writes
    remote_produces: tuple[str, ...] = ()  # paths under $HOME on the rig
    on_rig: bool = False                 # run this one ON the rig, as root
    device: bool = False                 # touches a removable device
    needs_secrets: bool = False          # needs config/rig.local.env filled in
    note: str = ""


STEPS: tuple[Step, ...] = (
    Step("prep", "Prepare everything", "scripts/00_toctou.sh",
         args=("prep",),
         produces=("out/stage1.te2", "out/usb.img"),
         note="venv -> pinned deps -> inputs -> decrypt -> image, in one go; "
              "steps whose outputs exist are skipped."),
    Step("env", "Python venv", "scripts/00_env.sh",
         produces=(".venv/bin/python",),
         note="pycryptodome, deflate, cryptography; PyQt5 optional."),
    Step("third_party", "Pinned third-party tools", "scripts/00_fetch_third_party.sh",
         produces=("third_party/ioniq5-usb-toctou/pizero/system", "config/key.local"),
         note="Clones the pinned upstreams and extracts the TE2 content key."),
    Step("stage_inputs", "Stage the stock inputs", "scripts/01_stage_inputs.sh",
         requires=("third_party/nudl/nudl",),
         produces=("input/mango-rootfs.tar.gz", "input/AppUpgrade.usb"),
         note="Downloads the official package and gates it against the locks."),
    Step("decrypt", "Decrypt the stock rootfs", "scripts/02_decrypt_rootfs.sh",
         requires=("input/mango-rootfs.tar.gz", "config/key.local"),
         produces=("work/stock-plain.tar.gz",)),
    Step("patch", "Patch the payload binaries", "scripts/03_patch_payload.py",
         requires=("work/stock-plain.tar.gz", ".venv/bin/python"),
         produces=("work/payload/AppUpgrade.patched",),
         note="Eleven stubs across four binaries, every byte hash-gated."),
    Step("repack", "Repack stage-1 rootfs", "scripts/04_repack_rootfs.sh",
         requires=("work/payload/AppUpgrade.patched",
                   "work/payload/AppScomoDM.patched",
                   "work/payload/AppDMClient.patched",
                   "work/payload/AppLauncher.patched"),
         produces=("out/stage1.te2",)),
    Step("wrapper", "Patch the USB wrapper", "scripts/07_build_patched_usb_appupgrade.sh",
         requires=("input/AppUpgrade.usb", ".venv/bin/python"),
         produces=("out/AppUpgrade.patched.te2",)),
    Step("packages", "Build the update packages", "scripts/06_build_package.sh",
         requires=("out/stage1.te2", "input/package_stock"),
         produces=("out/package_second/MANIFEST.sha256",
                   "out/package_wrapper/MANIFEST.sha256"),
         note="Run once per variant: second, wrapper."),
    Step("image", "Build the USB image", "scripts/08_build_usb_image.sh",
         requires=("out/stage1.te2", "input/package_stock"),
         produces=("out/usb.img", "out/regions.json")),
    Step("preflight", "Preflight the image", "scripts/09_preflight.sh",
         requires=("out/stage1.te2",),
         note="Read-only gate; no artifacts of its own."),
    Step("provision_pi", "Provision the rig", "scripts/10_provision_pi.sh",
         requires=("third_party/ioniq5-usb-toctou/pizero/system",),
         on_rig=True, needs_secrets=True,
         note="dwc2 peripheral mode, watchdog off, nbdkit — reboot afterwards."),
    Step("build_on_rig", "Build stage-1 on the rig", "scripts/19_build_on_rig.sh",
         requires=("input/mango-rootfs.tar.gz", "work/stock-plain.tar.gz"),
         remote_produces=("scourge/build/stage1.te2",),
         note="payload patch -> repack -> verify on the board: ~880 MB instead of "
              "the 27 GB usb.img, which never changes."),
    Step("deploy_pi", "Deploy the image to the rig", "scripts/11_deploy_pi.sh",
         requires=("out/usb.img", "out/stage1.te2", "out/regions.json"),
         remote_produces=("scourge/build/usb.img", "scourge/build/stage1.te2"),
         note="rsync over WiFi; the 27 GB image takes hours."),
    Step("verify_pi", "Verify the rig", "scripts/12_verify_pi.sh",
         note="UDC, dwc2 binding, watchdog, packages, deployed artifacts."),
    Step("wifi_fallback", "WiFi fallback (setup AP)", "scripts/17_provision_wifi_fallback.sh",
         requires=("tools/rigdash/pi/rig_wifi_agent.py",),
         remote_produces=("/etc/rig/wifi.env", "/usr/local/sbin/rig-ap-fallback"),
         on_rig=True, needs_secrets=True,
         note="Raises its own WPA2 AP after 90 s without a known network; join it "
              "and pick a network at http://10.42.0.1/."),
    Step("deploy_agent", "Deploy the rig control page", "scripts/18_deploy_rig_agent.sh",
         requires=("tools/rigdash/pi/rig_wifi_agent.py",),
         remote_produces=("/usr/local/lib/rig/rig_wifi_agent.py",),
         note="tools/rigdash/pi/* → /usr/local/{lib/rig,sbin} and /etc/systemd/system."),
    Step("sd_boot", "Provision the SD card boot", "scripts/16_provision_sd_boot.sh",
         requires=("scripts/firstrun.sh.tmpl",),
         needs_secrets=True, device=True,
         note="Renders firstrun.sh from config/rig.local.env and installs the "
              "systemd.run hook — the headless first boot."),
    Step("copy_to_sd", "Copy the image to the SD card", "scripts/13_copy_image_to_sd.sh",
         requires=("out/usb.img", "out/stage1.te2"),
         device=True,
         note="Destroys the card. Power the rig off and move the card here."),
    Step("format_usb", "Format the update USB stick", "scripts/14_format_usb.sh",
         requires=("out/package_second",),
         device=True, args=("--device", "$DEVICE"),
         note="Needs --device; destroys the stick."),
    Step("car_preflight", "Preflight against the car", "scripts/15_car_preflight.sh",
         requires=("out/package_second", "payload/patch_validators.py"),
         note="Read-only checklist for the head unit."),
)

BY_ID = {step.id: step for step in STEPS}


def command(step: Step) -> list[str]:
    """The argv that runs a step, from the repo root."""
    if step.script is None:
        raise ValueError(f"{step.id} has no script")
    interpreter = ["bash"] if step.script.endswith(".sh") else []
    return interpreter + [step.script, *step.args]


def _missing(paths: tuple[str, ...], settings) -> list[str]:
    repo = settings.repo
    return [p for p in paths if not (repo / p).exists()]


def evaluate(step: Step, settings, remote: dict[str, bool] | None = None) -> dict:
    """Status of one step: done / ready / blocked / manual.

    `remote` maps rig-relative paths to their existence, as probed over ssh; it
    is None when the rig could not be reached.
    """
    missing_requires = _missing(step.requires, settings)
    missing_produces = _missing(step.produces, settings)

    def secrets_gate() -> dict | None:
        """Blocked waiting for config/rig.local.env — but only if not done yet:
        a step already performed by other means must not look unfinished."""
        if not step.needs_secrets or settings.secrets_ready:
            return None
        missing = ", ".join(k for k, ok in settings.secret_status().items() if not ok)
        return {"status": BLOCKED,
                "detail": f"fill in {settings.secrets_path.name} (missing: {missing})",
                "missing": []}

    if step.remote_produces:
        absent: list[str] | None = None
        if remote is not None:
            absent = [p for p in step.remote_produces if not remote.get(p)]
            if not absent and not missing_requires:
                return {"status": DONE, "detail": "present on the rig", "missing": []}
        gate = secrets_gate()
        if gate:
            return gate
        if missing_requires:
            return {"status": BLOCKED,
                    "detail": "missing locally: " + ", ".join(missing_requires),
                    "missing": missing_requires}
        if absent is None:
            return {"status": READY, "detail": "rig unreachable — status unknown",
                    "missing": []}
        return {"status": READY, "detail": "absent on the rig: " + ", ".join(absent),
                "missing": []}

    if missing_requires:
        return {"status": BLOCKED,
                "detail": "missing: " + ", ".join(missing_requires),
                "missing": missing_requires}
    if step.produces and not missing_produces:
        return {"status": DONE, "detail": "outputs present", "missing": []}
    gate = secrets_gate()
    if gate:
        return gate
    if step.script is None:
        return {"status": MANUAL, "detail": step.note or "manual step", "missing": []}
    if step.produces:
        return {"status": READY,
                "detail": "will write " + ", ".join(missing_produces), "missing": []}
    return {"status": READY, "detail": step.note or "ready", "missing": []}


def ssh_argv(settings, remote_command: str) -> list[str]:
    return [
        "ssh", "-i", str(Path(settings.rig_key).expanduser()),
        "-o", "BatchMode=yes", "-o", "ConnectTimeout=6",
        f"{settings.rig_user}@{settings.rig_host}", remote_command,
    ]


#: The browser polls /api/state; without a cache every poll would open an ssh
#: session on the rig. One shared result per PROBE_TTL seconds is plenty for
#: "did the deploy land" and keeps sshd on the Pi quiet.
PROBE_TTL = 20.0
_probe_cache: dict[str, tuple[float, dict[str, bool] | None]] = {}
_probe_lock = threading.Lock()


def probe_remote(settings, *, fresh: bool = False) -> dict[str, bool] | None:
    """Existence of every remote artifact in one round trip; None if unreachable."""
    paths: set[str] = set()
    for step in STEPS:
        paths.update(step.remote_produces)
    if not paths:
        return {}
    if not shutil.which("ssh"):
        return None
    key = f"{settings.rig_user}@{settings.rig_host}"
    now = time.monotonic()
    with _probe_lock:
        cached = _probe_cache.get(key)
        if cached and not fresh and now - cached[0] < PROBE_TTL:
            return cached[1]
    listing = " ".join(sorted(paths))
    script = ('for p in ' + listing + '; do '
              'case "$p" in /*) t="$p";; *) t="$HOME/$p";; esac; '
              '[ -e "$t" ] && echo "yes $p" || echo "no $p"; done')
    try:
        proc = subprocess.run(ssh_argv(settings, script), capture_output=True,
                              text=True, timeout=20)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    found = {p: False for p in paths}
    for line in proc.stdout.splitlines():
        state, _, path = line.partition(" ")
        if path in found:
            found[path] = state == "yes"
    with _probe_lock:
        _probe_cache[key] = (time.monotonic(), found)
    return found


def snapshot(settings) -> tuple[list[dict], dict[str, bool] | None]:
    """Evaluated steps in pipeline order, plus the remote probe for reuse."""
    remote = probe_remote(settings)
    rows = []
    for step in STEPS:
        result = evaluate(step, settings, remote)
        rows.append({
            "id": step.id,
            "title": step.title,
            "script": step.script,
            "note": step.note,
            "on_rig": step.on_rig,
            "device": step.device,
            "args": list(step.args),
            **result,
        })
    return rows, remote
