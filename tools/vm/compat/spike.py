#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Mode C compat-kernel graphics spike.

Boots the decrypted vendor rootfs with the host kernel (the pinned compat
kernel comes later) and a custom initramfs. No gdbstub, no vendor kernel, no
i915. The rootfs is attached as virtio-blk and overlaid with qcow2, so the
backing image is never written.

This is deliberately a standalone spike: P1-1 folds the same setup into
`launch.py --kernel compat`. It proves the graphics chain first:
virtio-vga -> /dev/dri/card0 -> Weston 1.10 DRM backend -> fullscreen-shell
probe client -> QMP 1280x720 screendump.

Fidelity delta: this probe uses Weston's fullscreen-shell, not the vendor
IVI shell. The IVI surface is created correctly, but a window manager must
place it; UI content fidelity is P1-3's job. The compatibility claim here is
only that the vendor rootfs, Weston and a Wayland client render under the
compat kernel.
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

VM = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(VM))

from harness.images import cleanup_overlays, create_overlays  # noqa: E402
from harness.qmp import QmpError, screendump  # noqa: E402
from launch import Serial, execute_guest, wait_shell  # noqa: E402

COMPAT = VM / "compat"
ROOTFS = VM / "rootfs.img"
INITRAMFS = COMPAT / "initramfs-linux.img"
HOST_KERNEL = Path(f"/usr/lib/modules/{os.uname().release}/vmlinuz")

CMDLINE = ("root=/dev/vda rw rootwait rootfstype=ext4 init=/init "
           "console=ttyS0,115200 panic=-1 loglevel=6")

WESTON_CONFIG = (
    "[core]\\n"
    "shell=fullscreen-shell.so\\n"
)

SETUP = f"""set -x
mkdir -p /rw_data/var/run/user/root /rw_data/var/log /rw_data/var/run/dbus
chmod 700 /rw_data/var/run/user/root
cp /etc/passwd.legacy /tmp/passwd
ln -sf /etc/shadow.Global /tmp/shadow
printf '{WESTON_CONFIG}' >/tmp/weston.ini
id -u
uname -r
ls -l /dev/fb0 /dev/dri/card0 /dev/dri/renderD128 /dev/tty1 2>&1
"""

START_WESTON = """set -x
export XDG_RUNTIME_DIR=/var/run/user/root
export LD_LIBRARY_PATH=/app/lib:/usr/lib
/usr/bin/weston --backend=drm-backend.so --use-pixman --tty=1 --config=/tmp/weston.ini \
  --socket=wayland-0 --idle-time=0 --debug </dev/tty1 >/tmp/weston.log 2>&1 &
sleep 6
if pidof weston >/dev/null 2>&1; then
  echo WESTON_OK drm
else
  echo WESTON_FAIL
fi
tail -50 /tmp/weston.log
"""

START_PROBE = """set -x
export XDG_RUNTIME_DIR=/var/run/user/root
export WAYLAND_DISPLAY=wayland-0
export LD_LIBRARY_PATH=/app/lib:/usr/lib
setsid /usr/bin/weston-simple-shm </dev/null >/tmp/probe.log 2>&1 &
sleep 5
echo PROBE_PID=$(pidof weston-simple-shm)
tail -20 /tmp/probe.log
ps -ef | grep -E 'weston|simple-shm' | grep -v grep
"""


def qmp_shot(run_dir: Path, name: str) -> dict:
    return screendump(run_dir / "qmp.sock", run_dir / name, timeout=20)


def probe_bbox(path: Path) -> dict | None:
    try:
        from PIL import Image
    except ImportError:
        return None
    image = Image.open(path).convert("RGB")
    width, height = image.size
    pixels = image.load()
    xs: list[int] = []
    ys: list[int] = []
    for y in range(height):
        for x in range(width):
            red, green, blue = pixels[x, y]
            if red + green + blue > 30:
                xs.append(x)
                ys.append(y)
    if not xs:
        return {"count": 0, "bbox": None, "width": width, "height": height}
    return {
        "count": len(xs),
        "bbox": [min(xs), min(ys), max(xs), max(ys)],
        "width": width,
        "height": height,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--keep", action="store_true", help="keep the qcow2 overlay")
    parser.add_argument("--kernel", type=Path, default=HOST_KERNEL)
    parser.add_argument("--initrd", type=Path, default=INITRAMFS)
    args = parser.parse_args()

    for path in (args.kernel, args.initrd, ROOTFS):
        if not path.is_file():
            raise SystemExit(f"missing input: {path}")
    if not os.path.exists("/dev/kvm"):
        raise SystemExit("missing /dev/kvm")

    run_dir = VM / "runs" / f"compat-spike-{time.time_ns()}"
    run_dir.mkdir(parents=True)
    overlays = create_overlays(run_dir, {"rootfs": ROOTFS})
    serial_path = run_dir / "serial.log"
    qmp_socket = run_dir / "qmp.sock"

    argv = [
        "qemu-system-x86_64",
        "-machine", "q35,hpet=off",
        "-accel", "kvm", "-cpu", "host",
        "-m", "4096", "-smp", "1",
        "-nic", "none", "-display", "none",
        "-device", "virtio-vga,xres=1280,yres=720",
        "-kernel", str(args.kernel),
        "-initrd", str(args.initrd),
        "-append", CMDLINE,
        "-drive", f"if=none,id=rootfs,format=qcow2,file={overlays['rootfs']['path']}",
        "-device", "virtio-blk-pci,drive=rootfs",
        "-serial", "stdio", "-monitor", "none", "-no-reboot",
        "-qmp", f"unix:{qmp_socket},server=on,wait=off",
    ]
    (run_dir / "command.json").write_text(json.dumps(argv, indent=2) + "\n")
    print(f"run: {run_dir}", flush=True)
    proc = subprocess.Popen(argv, cwd=run_dir, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            start_new_session=True)
    ser = Serial(proc, serial_path)
    result: dict = {
        "kernel": str(args.kernel),
        "initrd": str(args.initrd),
        "display": "virtio-vga 1280x720",
        "compositor": "weston 1.10 drm-backend + fullscreen-shell",
        "gdbstub": False,
        "screenshots": {},
        "verdict": "FAIL",
    }
    try:
        started = time.monotonic()
        ser.read_until(b"IONIQ_VM_READY", args.timeout)
        wait_shell(ser, args.timeout)
        result["boot_seconds"] = round(time.monotonic() - started, 2)
        print(f"boot_seconds={result['boot_seconds']}", flush=True)

        _, output, rc, seconds = execute_guest(ser, SETUP, args.timeout)
        print(output, flush=True)
        result["setup_rc"] = rc
        result["setup_seconds"] = seconds
        result["dev_fb0"] = "/dev/fb0" in output
        result["dev_dri_card0"] = "/dev/dri/card0" in output
        if rc != 0:
            return 1
        result["screenshots"]["setup"] = qmp_shot(run_dir, "qmp-setup.png")

        _, output, rc, seconds = execute_guest(ser, START_WESTON, args.timeout)
        print(output, flush=True)
        result["weston_rc"] = rc
        result["weston_seconds"] = seconds
        result["weston_ok"] = "WESTON_OK drm" in output
        result["screenshots"]["weston"] = qmp_shot(run_dir, "qmp-weston.png")
        if not result["weston_ok"]:
            return 1

        _, output, rc, seconds = execute_guest(ser, START_PROBE, args.timeout)
        print(output, flush=True)
        result["probe_rc"] = rc
        result["probe_seconds"] = seconds
        result["probe_pid"] = "PROBE_PID=" in output and "PROBE_PID=\n" not in output
        result["screenshots"]["probe"] = qmp_shot(run_dir, "qmp-probe.png")
        bbox = probe_bbox(run_dir / "qmp-probe.png")
        result["probe_pixels"] = bbox
        result["probe_visible"] = bool(bbox and bbox.get("bbox"))
        result["verdict"] = "PASS" if (result["weston_ok"] and result["probe_pid"]
                                      and result["probe_visible"]) else "FAIL"
        return 0 if result["verdict"] == "PASS" else 1
    except (TimeoutError, EOFError, QmpError, OSError) as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
        print(f"ERROR: {result['error']}", file=sys.stderr, flush=True)
        tail = serial_path.read_bytes()[-4000:].decode(errors="replace")
        print(f"--- serial tail ---\n{tail}", file=sys.stderr, flush=True)
        return 1
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        ser.close()
        if not args.keep:
            cleanup_overlays(run_dir, overlays)
        (run_dir / "spike.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"result: {run_dir / 'spike.json'}", flush=True)


if __name__ == "__main__":
    sys.exit(main())
