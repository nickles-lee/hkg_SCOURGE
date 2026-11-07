# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Compat-kernel pin: record and verify the host kernel/initramfs recipe.

The kernel itself is never committed. `compat/kernel.lock.json` pins the
installed kernel version, `vmlinuz` hash, kernel config hash, required module
hashes, and the mkinitcpio recipe hash. `launch.py --kernel compat` and
`doctor --graphics` verify the pin before boot; a mismatch is an actionable
stop, not a best-effort boot.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

VM = Path(__file__).resolve().parent.parent
COMPAT = VM / "compat"
LOCK = COMPAT / "kernel.lock.json"
MKINITCPIO_CONF = COMPAT / "mkinitcpio.conf"
INITRAMFS = COMPAT / "initramfs-linux.img"
REQUIRED_MODULES = {
    "virtio_gpu": "virtio-gpu",
    "bochs": "bochs",
    "virtio_input": "virtio_input",
    "virtio_net": "virtio_net",
    "snd_hda_intel": "snd_hda_intel",
    "snd_hda_codec_generic": "snd_hda_codec_generic",
    "snd_hda_codec_hdmi": "snd_hda_codec_hdmi",
    "9p": "9p",
    "9pnet_virtio": "9pnet_virtio",
    "usb_storage": "usb-storage",
}


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(4 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _module_path(module: str) -> Path | None:
    try:
        out = subprocess.run(["modinfo", "-n", module], capture_output=True,
                             text=True, check=False)
    except OSError:
        return None
    value = out.stdout.strip()
    return Path(value) if value and out.returncode == 0 else None


def _config_path(kver: str) -> Path:
    if kver == os.uname().release and Path("/proc/config.gz").is_file():
        return Path("/proc/config.gz")
    candidate = Path(f"/boot/config-{kver}")
    if candidate.is_file():
        return candidate
    return Path(f"/usr/lib/modules/{kver}/config")


def make_lock(kver: str | None = None, package: str = "linux-omarchy") -> dict:
    kver = kver or os.uname().release
    vmlinuz = Path(f"/usr/lib/modules/{kver}/vmlinuz")
    if not vmlinuz.is_file():
        raise SystemExit(f"kernel image missing: {vmlinuz}")
    config = _config_path(kver)
    modules: dict[str, dict] = {}
    missing: list[str] = []
    modules_root = Path(f"/usr/lib/modules/{kver}").resolve()
    for name, module in REQUIRED_MODULES.items():
        path = _module_path(module)
        if path is None or not path.is_file():
            missing.append(module)
            continue
        resolved = path.resolve()
        try:
            relative = resolved.relative_to(modules_root)
        except ValueError:
            relative = Path(resolved.name)
        modules[name] = {
            "path": str(relative),
            "sha256": sha256_file(path),
        }
    if missing:
        raise SystemExit(f"required kernel modules missing: {', '.join(missing)}")
    lock = {
        "schema": 1,
        "kernel": {
            "package": package,
            "version": kver,
            "vmlinuz": str(vmlinuz),
            "vmlinuz_sha256": sha256_file(vmlinuz),
            "config": str(config) if config.is_file() else None,
            "config_sha256": sha256_file(config) if config.is_file() else None,
        },
        "modules": modules,
        "initramfs": {
            "builder": "compat/build-initramfs.sh",
            "config": "compat/mkinitcpio.conf",
            "config_sha256": sha256_file(MKINITCPIO_CONF),
            "image": "compat/initramfs-linux.img",
        },
    }
    return lock


def load_lock() -> dict | None:
    if not LOCK.is_file():
        return None
    try:
        value = json.loads(LOCK.read_text())
    except json.JSONDecodeError:
        return None
    return value if isinstance(value, dict) and value.get("schema") == 1 else None


def verify_kernel_pin(kver: str | None = None, require_initramfs: bool = True) -> dict:
    kver = kver or os.uname().release
    lock = load_lock()
    if lock is None:
        return {
            "ok": False,
            "state": "unpinned",
            "detail": "compat/kernel.lock.json missing or invalid; run sh compat/pin-kernel.sh",
        }
    problems: list[str] = []
    kernel = lock.get("kernel") or {}
    if kernel.get("version") != kver:
        problems.append(f"kernel version {kver} != pinned {kernel.get('version')}")
    pinned_kernel = COMPAT / "kernel"
    vmlinuz = pinned_kernel if pinned_kernel.is_file() else Path(kernel.get("vmlinuz", ""))
    if not vmlinuz.is_file():
        problems.append(f"kernel image missing: {vmlinuz}")
    elif sha256_file(vmlinuz) != kernel.get("vmlinuz_sha256"):
        problems.append(f"vmlinuz hash mismatch: {vmlinuz}")
    config_value = kernel.get("config")
    config = Path(config_value) if config_value else _config_path(kver)
    if not config.is_file():
        problems.append(f"kernel config missing: {config}")
    elif kernel.get("config_sha256") and sha256_file(config) != kernel["config_sha256"]:
        problems.append(f"kernel config hash mismatch: {config}")
    for name, expected in (lock.get("modules") or {}).items():
        path = Path(expected.get("path", ""))
        if not path.is_absolute():
            path = Path(f"/usr/lib/modules/{kver}") / path
        if not path.is_file():
            problems.append(f"module missing: {name} ({path})")
        elif sha256_file(path) != expected.get("sha256"):
            problems.append(f"module hash mismatch: {name} ({path})")
    initramfs = lock.get("initramfs") or {}
    if initramfs.get("config_sha256"):
        config_name = initramfs.get("config", "compat/mkinitcpio.conf")
        initramfs_config = Path(config_name)
        if not initramfs_config.is_absolute():
            initramfs_config = VM / initramfs_config
        if not initramfs_config.is_file():
            problems.append(f"mkinitcpio config missing: {initramfs_config}")
        elif sha256_file(initramfs_config) != initramfs["config_sha256"]:
            problems.append("mkinitcpio config differs from the pin; re-run compat/pin-kernel.sh")
    if not INITRAMFS.is_file():
        if require_initramfs:
            problems.append(f"initramfs missing: {INITRAMFS}; run sh compat/build-initramfs.sh")
    return {
        "ok": not problems,
        "state": "pinned" if not problems else "mismatch",
        "kernel": str(vmlinuz),
        "version": kver,
        "initramfs": str(INITRAMFS),
        "problems": problems,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    pin = sub.add_parser("pin-kernel", help="write compat/kernel.lock.json")
    pin.add_argument("--kver", default=None)
    pin.add_argument("--package", default="linux-omarchy")
    verify = sub.add_parser("verify-kernel", help="verify the compat kernel pin")
    verify.add_argument("--kver", default=None)
    verify.add_argument("--skip-initramfs", action="store_true")
    args = parser.parse_args(argv)
    if args.action == "pin-kernel":
        lock = make_lock(args.kver, args.package)
        LOCK.write_text(json.dumps(lock, indent=2, sort_keys=True) + "\n")
        print(LOCK)
        return 0
    status = verify_kernel_pin(args.kver, require_initramfs=not args.skip_initramfs)
    print(json.dumps(status, indent=2, sort_keys=True))
    return 0 if status["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
