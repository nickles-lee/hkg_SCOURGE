# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Host capability probes for `launch.py doctor`."""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

from .compat import verify_kernel_pin

QEMU = "qemu-system-x86_64"
MIN_QEMU = (8, 0, 0)
REQUIRED_DEVICES = ("virtio-vga", "sdhci-pci", "usb-storage",
                    "ich9-usb-ehci1", "virtio-blk-pci")
REQUIRED_COMPAT_MODULES = (
    "kernel/drivers/gpu/drm/virtio/virtio-gpu.ko",
    "kernel/drivers/gpu/drm/tiny/bochs.ko",
    "kernel/drivers/virtio/virtio_input.ko",
    "kernel/drivers/net/virtio_net.ko",
)


def version_tuple(text: str) -> tuple[int, int, int]:
    match = re.search(r"(\d+)\.(\d+)\.(\d+)", text)
    if not match:
        return (0, 0, 0)
    return tuple(int(part) for part in match.groups())


def _qemu_help(args: list[str]) -> str:
    try:
        out = subprocess.run([QEMU, *args], capture_output=True, text=True, check=False)
        return out.stdout + out.stderr
    except OSError:
        return ""


def qemu_devices() -> set[str]:
    return set(re.findall(r'name "([^"]+)"', _qemu_help(["-device", "help"])))


def qemu_displays() -> set[str]:
    text = _qemu_help(["-display", "help"])
    return {line.strip() for line in text.splitlines()
            if line.strip() and not line.startswith(("Available", "Some", "For"))}


def running_vm_pids() -> list[int]:
    """PIDs of harness QEMUs identified by the deterministic -name vm-lab."""
    pids: list[int] = []
    try:
        out = subprocess.run(["ps", "-eo", "pid=,args="], capture_output=True,
                             text=True, check=False).stdout
    except OSError:
        return pids
    for line in out.splitlines():
        fields = line.strip().split(None, 1)
        if len(fields) != 2:
            continue
        pid, args = fields
        if "qemu-system-x86_64" in args and "-name vm-lab" in args:
            try:
                pids.append(int(pid))
            except ValueError:
                pass
    return pids


def compat_state(vm_dir: Path) -> dict:
    pinned = vm_dir / "compat" / "kernel"
    kernel = pinned if pinned.is_file() else Path(f"/usr/lib/modules/{os.uname().release}/vmlinuz")
    initrd = vm_dir / "compat" / "initramfs-linux.img"
    modules_root = Path(f"/usr/lib/modules/{os.uname().release}")
    missing_modules = []
    for relative in REQUIRED_COMPAT_MODULES:
        module = modules_root / relative
        if not (module.is_file() or module.with_suffix(".ko.zst").is_file()
                or module.with_suffix(".ko.xz").is_file()):
            missing_modules.append(relative)
    return {
        "host_kernel": str(kernel),
        "host_kernel_ok": kernel.is_file(),
        "kernel_source": "pinned" if pinned.is_file() else "host",
        "initramfs": str(initrd),
        "initramfs_ok": initrd.is_file(),
        "missing_modules": missing_modules,
        "modules_ok": not missing_modules,
        "spike": str(vm_dir / "compat" / "spike.py"),
        "spike_ok": (vm_dir / "compat" / "spike.py").is_file(),
        "qemu": shutil.which(QEMU),
        "pin": verify_kernel_pin(),
    }


def runs_stats(vm_dir: Path) -> dict:
    root = vm_dir / "runs"
    count = 0
    try:
        for entry in os.scandir(root):
            if entry.is_dir(follow_symlinks=False):
                count += 1
    except OSError:
        pass
    return {"count": count}
