#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Local QEMU harness for the decrypted EU 260128 vendor rootfs.

Boots the vendor 4.1.27-abl bzImage with the vendor rootfs image. QEMU's
sdhci-pci controller is not in the kernel's PCI ID table, so the harness
stops at sdhci_driver_init and replaces exactly one table entry in guest RAM.
The base image and kernel are never modified (snapshot disk, no writes).

Usage:
  ./launch.py run --cmd "uname -r; id" --timeout 240
  ./launch.py run --interactive
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import os
import random
import re
import select
import signal
import shutil
import socket
import stat
import struct
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

from patch_kernel import COMPAT, TABLE_ENTRY, vaddr_to_offset
from harness.audit import audit_script, parse_audit
from harness.artifacts import extract_artifacts, wrap_command
from harness.compat import verify_kernel_pin
from harness.config import SCENARIO_NAMES, load_profile, load_scenario, scenario_command
from harness.doctor import (MIN_QEMU, REQUIRED_DEVICES, compat_state, qemu_devices,
                            qemu_displays, running_vm_pids, runs_stats, version_tuple)
from harness.images import cleanup_overlays, create_overlays, drive_path, validate_output
from harness.lifecycle import parse_lifecycle
from harness.manager import parse_manager_snapshots
from harness.protocol import command_frame, command_output, completion_pattern
from harness.qmp import QmpError
from harness.qmp import click as qmp_click
from harness.qmp import key as qmp_key
from harness.qmp import screendump as qmp_screendump
from harness.runs import collect_runs, extract as extract_run, filter_runs, prune as prune_runs
from harness.share import MOUNTPOINT, mount_script as share_mount_script
from harness.share import qemu_option as share_qemu_option
from harness.state import StateBusy, create_state_overlays
from harness.results import evaluate, phase_summary
from harness.storage import VOLUMES, cleanup_storage, create_storage, mount_script, parse_sizes
from harness.tools import check_tools

VM = Path(__file__).resolve().parent
KERNEL = VM / "bzImage"
KERNEL_PATCHED = VM / "vmlinux.patched"
ROOTFS = VM / "rootfs.img"
TOOLS = VM / "tools.img"
COMPAT_DIR = VM / "compat"
COMPAT_INITRAMFS = COMPAT_DIR / "initramfs-linux.img"
COMPAT_KERNEL = COMPAT_DIR / "kernel"
HOST_KERNEL = Path(f"/usr/lib/modules/{os.uname().release}/vmlinuz")
COMPAT_CMDLINE = ("root=/dev/vda rw rootwait rootfstype=ext4 init=/init "
                  "console=ttyS0,115200 panic=-1 loglevel=6 lsm=smack")
# Fixed guest identity: repeat runs must not acquire a random SMBIOS UUID.
VM_UUID = "b3f5c8d2-4b2a-5f13-9a7e-6d0c1e8f4a21"
UPDATE = VM / "update-k.img"
HASH_CACHE = VM / ".cache" / "hashes.json"

DRIVER_INIT = 0xFFFFFFFF81B24BAC
TABLE_ENTRY = 0xFFFFFFFF8164D100
EXPECTED = bytes.fromhex(
    "22100000ffffffffffffffffffffffff0005080000ffff0040d16481ffffffff")
COMPAT = struct.pack("<6IQ", 0x1B36, 7, 0xFFFFFFFF, 0xFFFFFFFF,
                     0x080500, 0xFFFF00, 0)

CMDLINE = (
    "root=/dev/mmcblk0 rw rootwait rootdelay=2 rootfstype=ext4 "
    "init=/init console=ttyS0,115200 panic=-1 loglevel=7 noapic"
)

_LIBC = ctypes.CDLL("libc.so.6", use_errno=True)


def pdeathsig() -> None:
    """Make QEMU die if the Python launcher is killed by an outer timeout."""
    _LIBC.prctl(1, 9, 0, 0, 0)  # PR_SET_PDEATHSIG, SIGKILL


class RSP:
    def __init__(self, sock: socket.socket, deadline: float | None = None):
        self.sock = sock
        self.buf = b""
        self.deadline = deadline

    def _budget(self) -> None:
        if self.deadline is not None:
            remaining = self.deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('SDHCI patch deadline exceeded')
            self.sock.settimeout(remaining)

    def _send(self, payload: bytes) -> None:
        self._budget()
        csum = sum(payload) & 0xFF
        self.sock.sendall(b"$" + payload + b"#" + f"{csum:02x}".encode())
        # QEMU may send '+' immediately, or only with the reply. _recv skips it.

    def _recv(self) -> bytes:
        while b"$" not in self.buf:
            self._budget()
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("QEMU gdb socket closed")
            self.buf += chunk
        self.buf = self.buf[self.buf.index(b"$") + 1:]
        while b"#" not in self.buf or len(self.buf) < self.buf.index(b"#") + 3:
            self._budget()
            chunk = self.sock.recv(4096)
            if not chunk:
                raise EOFError("QEMU gdb socket closed")
            self.buf += chunk
        i = self.buf.index(b"#")
        payload, csum = self.buf[:i], self.buf[i + 1:i + 3]
        self.buf = self.buf[i + 3:]
        if int(csum, 16) != sum(payload) & 0xFF:
            raise RuntimeError("RSP checksum mismatch")
        self.sock.sendall(b"+")
        return payload

    def cmd(self, payload: str, stop: bool = False) -> bytes:
        self._send(payload.encode())
        data = self._recv()
        if stop and not (data.startswith(b"T") or data.startswith(b"S")):
            raise RuntimeError(f"expected stop packet, got {data!r}")
        return data


def patch_sdhci(run_dir: Path, proc: subprocess.Popen, timeout: float = 30) -> dict:
    sock_path = run_dir / "gdb.sock"
    deadline = time.monotonic() + min(timeout, 30)
    while not sock_path.exists():
        if proc.poll() is not None:
            raise RuntimeError(f"QEMU exited before gdb socket (rc={proc.returncode})")
        if time.monotonic() > deadline:
            raise TimeoutError("QEMU gdb socket did not appear")
        time.sleep(0.05)
    # The socket is created before QEMU's gdbstub is necessarily servicing it.
    print(f"[vm] gdb socket present: {sock_path}", flush=True)
    time.sleep(min(0.75, max(0, deadline - time.monotonic())))
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(max(0.001, deadline - time.monotonic()))
    print("[vm] connecting to gdbstub", flush=True)
    sock.connect(str(sock_path))
    print("[vm] gdb connected; querying status", flush=True)
    rsp = RSP(sock, deadline)
    try:
        rsp.cmd("?")
        print("[vm] status ok; setting driver-init breakpoint", flush=True)
        rsp.cmd(f"Z1,{DRIVER_INIT:x},1")
        print("[vm] breakpoint set; continuing", flush=True)
        stop = rsp.cmd("c", stop=True)
        print(f"[vm] stopped at driver init ({stop.decode(errors='replace')})", flush=True)
        actual = bytes.fromhex(rsp.cmd(f"m{TABLE_ENTRY:x},20").decode())
        if actual != EXPECTED:
            raise RuntimeError(
                "SDHCI table bytes differ; refusing guest-memory write\n"
                f"expected {EXPECTED.hex()}\nactual   {actual.hex()}")
        rsp.cmd(f"M{TABLE_ENTRY:x},20:{COMPAT.hex()}")
        check = bytes.fromhex(rsp.cmd(f"m{TABLE_ENTRY:x},20").decode())
        if check != COMPAT:
            raise RuntimeError("guest-memory readback mismatch")
        rsp.cmd(f"z1,{DRIVER_INIT:x},1")
        rsp.cmd("D")
        return {"driver_init": hex(DRIVER_INIT), "table_entry": hex(TABLE_ENTRY),
                "stop": stop.decode(errors="replace"),
                "original": EXPECTED.hex(), "replacement": COMPAT.hex()}
    finally:
        sock.close()


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(4 * 1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def cached_sha256(path: Path, refresh: bool = False, cache_only: bool = False) -> str | None:
    st = path.stat()
    key = f"{path.resolve()}:{st.st_dev}:{st.st_ino}:{st.st_size}:{st.st_mtime_ns}:{st.st_ctime_ns}"
    cache: dict[str, str] = {}
    try:
        cache = json.loads(HASH_CACHE.read_text())
    except Exception:
        pass
    if key in cache and not refresh:
        return cache[key]
    if cache_only:
        return None
    digest = sha256_file(path)
    HASH_CACHE.parent.mkdir(parents=True, exist_ok=True)
    cache[key] = digest
    temporary = HASH_CACHE.with_name(f"hashes-{uuid.uuid4().hex}.tmp")
    temporary.write_text(json.dumps(cache, indent=2) + "\n")
    temporary.replace(HASH_CACHE)
    return digest


def image_identity(path: Path, verify: bool = False) -> dict:
    st = path.stat()
    if not stat.S_ISREG(st.st_mode):
        raise ValueError(f'input must be a regular image file, not a device or pipe: {path}')
    return {'path': str(path.resolve()), 'size': st.st_size,
            'mtime_ns': st.st_mtime_ns, 'ctime_ns': st.st_ctime_ns,
            'sha256': cached_sha256(path, refresh=verify,
                                    cache_only=not verify and st.st_size > 256 * 1024 ** 2),
            'fresh_digest': verify, 'trusted_reference_verified': False}


def source_identities() -> dict:
    paths = [VM / 'launch.py', VM / 'patch_kernel.py']
    for directory, patterns in (
        ('harness', ('*.py',)),
        ('profiles', ('*.json',)),
        ('scenarios', ('*.json',)),
        ('guest', ('*.c', '*.sh')),
        ('probes', ('*.py', '*.sh', '*.md')),
        ('toolsroot', ('*.sh',)),
        ('compat', ('*.py', '*.sh', '*.conf')),
    ):
        for pattern in patterns:
            paths.extend(sorted((VM / directory).glob(pattern)))
    return {str(path.relative_to(VM)): sha256_file(path)
            for path in paths
            if path.is_file() and path.name != 'manifest.json'}


def kernel_is_patched(path: Path) -> bool:
    try:
        blob = path.read_bytes()
        off = vaddr_to_offset(blob, TABLE_ENTRY)
        return blob[off:off + 32] == COMPAT
    except (Exception, SystemExit):
        return False


def qemu_version() -> str:
    try:
        out = subprocess.run(["qemu-system-x86_64", "--version"],
                             capture_output=True, text=True, check=True)
        return out.stdout.splitlines()[0]
    except Exception as exc:  # noqa: BLE001
        return f"unavailable: {exc}"


def build_payload_image(src: Path, out: Path) -> Path:
    """Build a small ext4 image from a directory for the third USB stick."""
    if not src.is_dir():
        raise SystemExit(f"payload is not a directory: {src}")
    total = sum(p.stat().st_size for p in src.rglob("*") if p.is_file())
    size_mb = max(8, (total + 4 * 1024 * 1024 + 1024 * 1024 - 1) // (1024 * 1024))
    validate_output(out, (ROOTFS, KERNEL, TOOLS, UPDATE))
    subprocess.run(["truncate", "-s", f"{size_mb}M", str(out)], check=True)
    subprocess.run([
        "fakeroot", "mkfs.ext4", "-F", "-b", "4096", "-I", "256",
        "-O", "none,has_journal,ext_attr,resize_inode,dir_index,filetype,"
              "extent,flex_bg,sparse_super,large_file,huge_file,dir_nlink,"
              "extra_isize",
        "-E", "lazy_itable_init=0,lazy_journal_init=0,root_owner=0:0",
        "-d", str(src), str(out),
    ], check=True, stdout=subprocess.DEVNULL)
    return out


def compat_kernel_path() -> Path:
    return COMPAT_KERNEL if COMPAT_KERNEL.is_file() else HOST_KERNEL


def qemu_argv(run_dir: Path, usb: Path | None = None,
              tools: Path | None = None,
              patched_kernel: bool = False,
              payload: Path | None = None,
              overlays: dict[str, dict] | None = None,
              artifacts_serial: Path | None = None,
              qmp_socket: Path | None = None,
              display_device: str = "none",
              smp: int = 1, memory_mb: int = 4096,
              noapic: bool = True,
              kernel_target: str = "vendor",
              share: Path | None = None,
              display: str = "none",
              vnc_display: str = ":1",
              network: bool = False) -> list[str]:
    kvm = os.path.exists("/dev/kvm") and os.access("/dev/kvm", os.R_OK | os.W_OK)
    if kvm:
        accel, cpu = "kvm", "host"
    else:
        accel, cpu = "tcg,thread=single", "max,vendor=GenuineIntel"
    if kernel_target == "compat":
        kernel = compat_kernel_path()
        initrd = COMPAT_INITRAMFS
    elif kernel_target == "vendor":
        kernel = KERNEL_PATCHED if patched_kernel else KERNEL
        initrd = None
    else:
        raise ValueError(f"unknown kernel target: {kernel_target}")

    def drive(name: str, source: Path) -> str:
        if overlays is None or name not in overlays:
            raise ValueError(f"missing explicit overlay: {name}")
        cache = ",cache=writethrough" if overlays[name].get('persistent') else ""
        return (f"if=none,id={name},format=qcow2,"
                f"file={drive_path(Path(overlays[name]['path']))}{cache}")

    argv = [
        "qemu-system-x86_64",
        "-no-user-config",
        "-nodefaults",
        "-name", "vm-lab",
        "-uuid", VM_UUID,
        "-machine", "q35,hpet=off",
        "-accel", accel,
        "-cpu", cpu,
        "-m", str(memory_mb), "-smp", str(smp),
        "-nic", "none",
        "-rtc", "base=utc",
    ]
    if display == "none":
        argv += ["-display", "none"]
    elif display == "gtk":
        argv += ["-display", "gtk"]
    elif display == "vnc":
        argv += ["-vnc", vnc_display]
    else:
        raise ValueError(f"unknown display: {display}")
    if network:
        argv += [
            "-netdev", "user,id=net0,restrict=on,hostfwd=tcp:127.0.0.1:2222-:22",
            "-device", "virtio-net-pci,netdev=net0",
        ]
    if kernel_target == "compat":
        argv += [
            "-kernel", str(kernel), "-initrd", str(initrd),
            "-drive", drive('rootfs', ROOTFS),
            "-device", "virtio-blk-pci,drive=rootfs",
            "-device", "virtio-tablet-pci",
            "-device", "virtio-keyboard-pci",
            "-append", COMPAT_CMDLINE + (" noapic" if noapic else ""),
        ]
        if share is not None:
            argv += ["-virtfs", share_qemu_option(share)]
    else:
        argv += [
            "-kernel", str(kernel),
            "-drive", drive('rootfs', ROOTFS),
            "-device", "sdhci-pci,id=sdhci0",
            "-device", "sd-card,drive=rootfs",
            "-append", CMDLINE if noapic else CMDLINE.replace(" noapic", ""),
        ]
    argv += ["-serial", "stdio", "-monitor", "none", "-no-reboot"]
    if display_device == "std":
        argv += ["-vga", "std"]
    elif display_device == "virtio-vga":
        argv += ["-vga", "none", "-device", "virtio-vga,xres=1280,yres=720"]
    elif display_device == "none":
        argv += ["-vga", "none"]
    else:
        raise ValueError(f"unknown display device: {display_device}")
    if qmp_socket is not None:
        argv += ["-qmp", f"unix:{qmp_socket},server=on,wait=off"]
    if artifacts_serial is not None:
        argv += [
            "-chardev", f"file,id=artifacts,path={artifacts_serial}",
            "-serial", "chardev:artifacts",
        ]
    if kernel_target == "vendor" and not patched_kernel:
        argv += ["-gdb", "unix:gdb.sock,server=on,wait=off", "-S"]
    storage_names = [f'storage_{name}' for name in VOLUMES if f'storage_{name}' in (overlays or {})]
    if usb is not None or tools is not None or payload is not None or storage_names:
        argv += ["-device", "ich9-usb-ehci1,id=ehci0"]
    if usb is not None:
        argv += [
            "-drive", drive('usb', usb),
            "-device", "usb-storage,drive=usb,bus=ehci0.0",
        ]
    if tools is not None:
        argv += [
            "-drive", drive('tools', tools),
            "-device", "usb-storage,drive=tools,bus=ehci0.0",
        ]
    if payload is not None:
        argv += [
            "-drive", drive('payload', payload),
            "-device", "usb-storage,drive=payload,bus=ehci0.0",
        ]
    for name in storage_names:
        argv += ['-drive', drive(name, Path(overlays[name]['backing'])),
                 '-device', f'usb-storage,drive={name},bus=ehci0.0,serial=hns-{name}']
    return argv


class Serial:
    """Drain QEMU stdio continuously so a full pipe can never block QEMU."""

    def __init__(self, proc: subprocess.Popen, log_path: Path | None = None):
        self.proc = proc
        self.log = bytearray()
        self.cond = threading.Condition()
        self.eof = False
        self.fh = open(log_path, "ab") if log_path else None
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def _reader(self) -> None:
        try:
            while True:
                chunk = os.read(self.proc.stdout.fileno(), 65536)
                if not chunk:
                    break
                with self.cond:
                    self.log += chunk
                    if self.fh:
                        self.fh.write(chunk)
                        self.fh.flush()
                    self.cond.notify_all()
        finally:
            with self.cond:
                self.eof = True
                self.cond.notify_all()

    def close(self) -> None:
        self.thread.join(timeout=2)
        if self.thread.is_alive():
            raise RuntimeError('serial reader did not stop; retaining its log handle')
        if self.fh:
            self.fh.close()
            self.fh = None

    def read_until(self, needle: bytes, timeout: float) -> bytes:
        deadline = time.monotonic() + timeout
        with self.cond:
            while needle not in self.log:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(
                        f"timed out waiting for {needle!r}; tail:\n"
                        + self.log[-2000:].decode(errors="replace"))
                if self.eof:
                    raise EOFError("QEMU serial closed")
                self.cond.wait(min(remaining, 1.0))
        return bytes(self.log)

    def read_match(self, pattern: bytes, timeout: float, offset: int = 0) -> bytes:
        deadline = time.monotonic() + timeout
        with self.cond:
            while not re.search(pattern, self.log[offset:]):
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"command completion timed out; tail:\n{self.log[-2000:].decode(errors='replace')}")
                if self.eof:
                    raise EOFError('QEMU serial closed before command completion')
                self.cond.wait(min(remaining, 1))
            return bytes(self.log[offset:])

    def send(self, text: str) -> None:
        self.proc.stdin.write(text.encode())
        self.proc.stdin.flush()


def wait_shell(ser: "Serial", timeout: float) -> None:
    """Probe until the guest shell actually executes something."""
    deadline = time.monotonic() + min(timeout, 60)
    while time.monotonic() < deadline:
        a, b = random.randint(100000, 900000), random.randint(100000, 900000)
        token = f"VM_PROBE_{a + b}"
        ser.send(f"echo VM_PROBE_$(({a}+{b}))\n")
        try:
            ser.read_until(token.encode(), min(5, max(0, deadline - time.monotonic())))
            return
        except TimeoutError:
            continue
    raise TimeoutError("guest shell did not respond to probe")


def execute_guest(ser: Serial, command: str, timeout: float) -> tuple[str, str, int, float]:
    with ser.cond:
        offset = len(ser.log)
    token, framed = command_frame(command)
    started = time.monotonic()
    ser.send(framed)
    text = ser.read_match(completion_pattern(token), timeout, offset).decode(errors='replace')
    output, guest_rc = command_output(text, token)
    return token, output, guest_rc, round(time.monotonic() - started, 3)


def run(cmd: str | None, interactive: bool, timeout: float,
        usb: Path | None = None, tools: Path | None = None,
        kernel_mode: str = "auto", payload: Path | None = None,
        profile_name: str = 'research', scenario_name: str | None = None,
        keep_overlay: bool = False, verify_images: bool = False,
        graphics: str = 'patched-offscreen', storage_mode: str = 'flat',
        storage_sizes: dict[str, int] | None = None, collect_audit: bool = False,
        allow_stale_tools: bool = False, screenshot: bool = False,
        qmp_screenshot: bool = False, display_device: str = "none",
        smp: int = 1, memory_mb: int = 4096, apic: bool = False,
        failure_bundle: bool = True, kernel_target: str = "vendor",
        allow_kernel_mismatch: bool = False, share: Path | None = None,
        display: str = "none", vnc_display: str = ":1",
        label: str = "", state: Path | None = None, reset: bool = False,
        network: bool = False) -> int:
    profile = load_profile(profile_name)
    scenario = load_scenario(scenario_name) if scenario_name else None
    if profile_name == 'conformance' and (scenario is None or interactive):
        raise ValueError('conformance requires a managed --scenario; arbitrary commands are research only')
    if interactive and screenshot:
        raise ValueError('--screenshot requires noninteractive execution')
    if display_device not in ('none', 'std', 'virtio-vga'):
        raise ValueError(f'unknown display device: {display_device}')
    if qmp_screenshot and display_device == 'none':
        display_device = 'virtio-vga' if kernel_target == 'compat' else 'std'
    if display not in ('none', 'gtk', 'vnc'):
        raise ValueError(f'unknown display backend: {display}')
    if display != 'none' and display_device == 'none':
        display_device = 'virtio-vga' if kernel_target == 'compat' else 'std'
    if smp not in (1, 2, 4):
        raise ValueError('smp must be 1, 2 or 4 (vendor NR_CPUS=4)')
    if not 1024 <= memory_mb <= 8192:
        raise ValueError('memory_mb must be between 1024 and 8192')
    if kernel_target not in ('vendor', 'compat'):
        raise ValueError(f'unknown kernel target: {kernel_target}')
    if kernel_target == 'compat':
        if not COMPAT_INITRAMFS.is_file():
            raise ValueError('compat initramfs missing; run sh compat/build-initramfs.sh')
        if not compat_kernel_path().is_file():
            raise ValueError(f'compat kernel missing: {compat_kernel_path()}')
        compat_pin_status = verify_kernel_pin()
        if not compat_pin_status['ok'] and not allow_kernel_mismatch:
            detail = '; '.join(compat_pin_status.get('problems', [])) or compat_pin_status.get('detail', '')
            raise ValueError(f'compat kernel pin mismatch: {detail} '
                             '(re-pin with sh compat/pin-kernel.sh or pass --allow-kernel-mismatch)')
    else:
        compat_pin_status = None
    if share is not None:
        if kernel_target != 'compat':
            raise ValueError('--share requires --kernel compat (the vendor kernel has no 9p/virtio)')
        share = Path(share).resolve()
        if not share.is_dir():
            raise ValueError(f'share directory missing: {share}')
    if reset and state is None:
        raise ValueError('--reset requires --state')
    if state is not None:
        state = Path(state).resolve()
        if storage_mode != 'flat':
            raise ValueError('--state currently supports flat storage only')
    if network:
        if kernel_target != 'compat':
            raise ValueError('--network requires --kernel compat (Mode C first)')
        if profile_name == 'conformance':
            raise ValueError('--network is forbidden by the conformance profile')
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError('timeout must be finite and positive')
    if storage_mode not in ('flat', 'split') or storage_mode == 'flat' and storage_sizes:
        raise ValueError('volume sizes require --storage split; unknown storage mode is not allowed')
    if interactive and (storage_mode != 'flat' or collect_audit):
        raise ValueError('split storage and audits currently require noninteractive execution')
    run_dir = VM / 'runs' / f'{time.time_ns()}'
    run_dir.mkdir(parents=True)
    print(f'run: {run_dir}', flush=True)
    result = {'schema_version': 3, 'run_dir': str(run_dir), 'profile': profile,
              'label': label,
              'scenario': scenario, 'cmd': cmd, 'interactive': interactive,
              'timeout': timeout, 'verdict': 'FAIL', 'rc': 1, 'guest_rc': None,
              'error': None, 'boot_seconds': None, 'readiness_seconds': None,
              'scenario_seconds': None, 'phase': None, 'images': {}, 'overlays': {},
              'keep_overlay': keep_overlay, 'qemu': qemu_version(),
              'applied_substitutions': [], 'output_streams': 'stdout/stderr merged on serial'}
    result['graphics_mode'] = graphics
    result['mode'] = kernel_target
    result['kernel_target'] = kernel_target
    result['fidelity'] = (
        'compat kernel: graphics/UI substrate only; security and policy claims require Mode V'
        if kernel_target == 'compat' else
        'vendor 4.1.27-abl kernel: policy/security ground truth; headless by kernel config'
    )
    result['compat_pin'] = compat_pin_status
    result['share'] = ({'path': str(share), 'mountpoint': MOUNTPOINT}
                       if share is not None else None)
    result['state'] = ({'path': str(state), 'reset': reset} if state is not None else None)
    result['network'] = ({'enabled': True, 'mode': 'user-restrict',
                          'hostfwd': 'tcp:127.0.0.1:2222-:22'} if network else None)
    result['tools_manifest'] = None
    result['display_device'] = display_device
    result['display'] = display
    result['vnc_display'] = vnc_display if display == 'vnc' else None
    result['smp'] = smp
    result['memory_mb'] = memory_mb
    result['apic'] = apic
    result['failure_bundle'] = None
    result['screenshot'] = {'requested': screenshot or qmp_screenshot, 'ok': None,
                            'artifacts': [], 'qmp': None, 'qmp_error': None}
    result['harness_sources'] = source_identities()
    storage = {'mode': 'flat', 'volumes': {}}
    result['storage'] = storage
    result['audit_requested'] = collect_audit or scenario_name == 'environment-audit'
    result['audit'] = {'status': 'not-collected', 'reason': 'disabled' if not result['audit_requested'] else 'run did not reach collection'}
    proc = None
    ser = None
    overlays = {}
    artifact_serial = None
    qmp_socket = None
    state_lock = None
    t0 = time.monotonic()
    old_handlers = {}

    def remaining() -> float:
        value = timeout - (time.monotonic() - t0)
        if value <= 0:
            raise TimeoutError('run deadline exceeded')
        return value

    def interrupted(signum, frame):
        raise InterruptedError(f'launcher received signal {signum}')

    try:
        for signum in (signal.SIGTERM, signal.SIGINT):
            old_handlers[signum] = signal.signal(signum, interrupted)
        if scenario:
            if scenario.get('requires_mode') and scenario['requires_mode'] != kernel_target:
                result.update(verdict='BLOCKED',
                              blockers=[f"requires kernel mode {scenario['requires_mode']}"],
                              rc=77)
                return 77
            if scenario.get('requires_share') and share is None:
                result.update(verdict='BLOCKED',
                              blockers=['requires --share DIR (host payload)'], rc=77)
                return 77
            blockers = scenario['blockers'].get(profile_name, [])
            if blockers:
                result.update(verdict='BLOCKED', blockers=blockers, rc=77)
                return 77
            for requirement in scenario['requires']:
                if {'usb': usb, 'tools': tools, 'payload': payload}.get(requirement) is None:
                    raise ValueError(f'scenario {scenario_name} requires --{requirement}')
            if scenario.get('requires_storage') and scenario['requires_storage'] != storage_mode:
                raise ValueError(f'{scenario_name} requires --storage {scenario["requires_storage"]}')
        if tools is not None:
            tools_status = check_tools(tools)
            result['tools_manifest'] = tools_status
            if not tools_status['ok']:
                message = (f"tools image {tools} is {tools_status['state']}: "
                           f"{tools_status['detail']}")
                if scenario_name is not None and not allow_stale_tools:
                    raise ValueError(message + ' (or pass --allow-stale-tools for research)')
                print(f"[vm] WARNING: {message}", flush=True)
        if not ROOTFS.is_file():
            raise ValueError('missing vm/rootfs.img; prepare inputs explicitly')
        if kernel_target == 'vendor' and not KERNEL.is_file():
            raise ValueError('missing vm/bzImage; prepare inputs explicitly')
        patched = (kernel_target == 'vendor' and kernel_mode == 'vmlinux'
                   and kernel_is_patched(KERNEL_PATCHED))
        if kernel_target == 'vendor' and kernel_mode == 'vmlinux' and not patched:
            raise ValueError('vmlinux mode requested but patched kernel is invalid')
        inputs = {'rootfs': ROOTFS}
        for name, path in (('usb', usb), ('tools', tools)):
            if path is not None:
                inputs[name] = path.resolve(strict=True)
        if payload is not None:
            payload = payload.resolve(strict=True)
            inputs['payload'] = (build_payload_image(payload, run_dir / 'payload.img')
                                 if payload.is_dir() else payload)
        if storage_mode == 'split':
            storage = create_storage(run_dir, storage_sizes)
            result['storage'] = storage
            for name, item in storage['volumes'].items():
                inputs[item['overlay_name']] = Path(item['backing'])
        if scenario:
            cmd = scenario_command(scenario, profile, graphics, storage, kernel_target)
            result['cmd'] = cmd
        if screenshot:
            if cmd is None:
                raise ValueError('--screenshot requires a command or scenario')
            cmd = wrap_command(cmd, True)
            result['cmd'] = cmd
        artifact_serial = run_dir / 'artifact-serial.bin' if screenshot else None
        if artifact_serial is not None:
            result['screenshot']['serial'] = str(artifact_serial)
        qmp_socket = run_dir / 'qmp.sock' if qmp_screenshot else None
        if qmp_socket is not None:
            result['screenshot']['qmp_socket'] = str(qmp_socket)
        kernel = compat_kernel_path() if kernel_target == 'compat' else (
            KERNEL_PATCHED if patched else KERNEL)
        identities = {**inputs, 'kernel': kernel}
        if kernel_target == 'compat':
            identities['initramfs'] = COMPAT_INITRAMFS
        result['images'] = {name: image_identity(path, verify_images)
                            for name, path in identities.items()}
        # Populate incrementally so a failed qemu-img call still leaves enough
        # ownership information for cleanup and post-mortem inspection.
        if state is not None:
            try:
                overlays, state_lock = create_state_overlays(state, inputs, reset=reset)
            except StateBusy as exc:
                raise RuntimeError(str(exc)) from exc
            result['state'].update({
                'overlays': {name: item['path'] for name, item in overlays.items()},
                'persistent': True,
            })
        else:
            for name, path in inputs.items():
                overlays.update(create_overlays(run_dir, {name: path}))
        result['overlays'] = overlays
        argv = qemu_argv(run_dir, usb, tools, patched, inputs.get('payload'), overlays,
                         artifact_serial, qmp_socket, display_device,
                         smp, memory_mb, not apic, kernel_target, share,
                         display, vnc_display, network)
        (run_dir / 'command.json').write_text(json.dumps(argv, indent=2) + '\n')
        if kernel_target == 'compat':
            result['kernel_mode'] = 'compat'
        else:
            result['kernel_mode'] = 'vmlinux.patched' if patched else 'bzImage+gdb'
        boot_start = time.monotonic()
        proc = subprocess.Popen(argv, cwd=run_dir, env={**os.environ, 'TMPDIR': str(run_dir)},
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, preexec_fn=pdeathsig)
        result['qemu_pid'] = proc.pid
        ser = Serial(proc, run_dir / 'serial.log')
        if kernel_target == 'compat':
            result['patch'] = {'mode': 'compat-no-gdb',
                               'kernel': str(kernel),
                               'initramfs': str(COMPAT_INITRAMFS)}
        elif patched:
            result['patch'] = {'mode': 'offline_vmlinux', 'kernel': str(kernel)}
        else:
            result['patch'] = patch_sdhci(run_dir, proc, remaining())
        if interactive:
            written = 0
            while proc.poll() is None:
                with ser.cond:
                    chunk = bytes(ser.log[written:])
                    written = len(ser.log)
                if chunk:
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.buffer.flush()
                ready, _, _ = select.select([sys.stdin], [], [], 0.2)
                if ready:
                    data = os.read(sys.stdin.fileno(), 4096)
                    if data:
                        proc.stdin.write(data)
                        proc.stdin.flush()
            result.update(rc=proc.returncode, verdict='PASS' if proc.returncode == 0 else 'FAIL')
            return result['rc']
        ser.read_until(b'IONIQ_VM_READY', remaining())
        result['boot_seconds'] = round(time.monotonic() - boot_start, 3)
        wait_shell(ser, remaining())
        result['readiness_seconds'] = round(time.monotonic() - boot_start, 3)
        # Disable echo and canonical line-length limits before sending scripts.
        setup_token, setup = command_frame('stty -echo -icanon min 1 time 0')
        ser.send(setup)
        setup_text = ser.read_match(completion_pattern(setup_token), remaining()).decode(errors='replace')
        if command_output(setup_text, setup_token)[1]:
            raise RuntimeError('failed to configure serial command channel')
        if storage_mode == 'split':
            _, setup_output, setup_rc, setup_seconds = execute_guest(
                ser, mount_script(storage, kernel_target), remaining())
            (run_dir / 'storage-setup.log').write_text(setup_output + '\n')
            required = {'checks': [f'storage-{name}' for name in VOLUMES]}
            storage['setup'] = evaluate(setup_output, setup_rc, required, profile)
            storage['setup_seconds'] = setup_seconds
            if storage['setup']['verdict'] != 'PASS':
                raise RuntimeError(f'storage setup failed; see {run_dir / "storage-setup.log"}')
        if share is not None:
            _, share_output, share_rc, share_seconds = execute_guest(
                ser, share_mount_script(), remaining())
            (run_dir / 'share-setup.log').write_text(share_output + '\n')
            result['share'].update({'setup_rc': share_rc, 'seconds': share_seconds})
            if share_rc != 0:
                raise RuntimeError(f'host share mount failed; see {run_dir / "share-setup.log"}')
        token, output, guest_rc, scenario_seconds = execute_guest(
            ser, cmd or 'uname -a; id; cat /proc/partitions', remaining())
        result['command_id'] = token
        (run_dir / 'guest-output.log').write_text(output + '\n')
        result.update(evaluate(output, guest_rc, scenario, profile,
                               {'storage': storage_mode}))
        result['guest_rc'] = guest_rc
        result['scenario_seconds'] = scenario_seconds
        if 'MANAGER_SNAPSHOT ' in output:
            manager = parse_manager_snapshots(output)
            (run_dir / 'manager-initialization.json').write_text(json.dumps(manager, indent=2) + '\n')
            result['manager_initialization'] = manager
        if scenario_name == 'vendor-lifecycle' and 'LIFECYCLE_CLIENT_PID ' in output:
            lifecycle = parse_lifecycle(output)
            (run_dir / 'lifecycle.json').write_text(json.dumps(lifecycle, indent=2) + '\n')
            result['lifecycle'] = lifecycle
            if result['verdict'] == 'PASS' and not (lifecycle['reaped'] and lifecycle['sigkill_requested']
                    and lifecycle['manager_plane1_registration_observed']):
                result['verdict'] = 'FAIL'
                result['failures'].append('lifecycle evidence did not confirm plane-1 registration, escalation and signaled-child reaping')
        elif scenario_name == 'vendor-lifecycle' and result['verdict'] == 'PASS':
            result['verdict'] = 'FAIL'
            result['failures'].append('lifecycle client identity/trace missing')
        result['applied_substitutions'] = sorted(set(result['applied_substitutions']) |
            set(storage.get('setup', {}).get('applied_substitutions', [])))
        if result['audit_requested']:
            result['audit'] = {'status': 'incomplete', 'reason': 'collection or validation did not complete'}
            if scenario_name == 'environment-audit':
                audit_output, audit_rc = output, guest_rc
            else:
                _, audit_output, audit_rc, audit_seconds = execute_guest(ser, audit_script(), remaining())
                result['audit_seconds'] = audit_seconds
            (run_dir / 'environment-audit.log').write_text(audit_output + '\n')
            audit = parse_audit(audit_output)
            (run_dir / 'environment.json').write_text(json.dumps(audit, indent=2) + '\n')
            status = evaluate(audit_output, audit_rc, {'checks': ['environment-audit']}, profile)
            result['audit'] = {'status': status['verdict'], 'path': str(run_dir / 'environment.json'),
                               'gaps': audit['gaps'], 'missing_required': audit['missing_required']}
            if status['verdict'] != 'PASS' or audit['missing_required']:
                result['verdict'] = 'FAIL'
        result['rc'] = (77 if result['verdict'] == 'BLOCKED' else
                        (guest_rc or 1) if result['verdict'] == 'FAIL' else 0)
        print(output)
        return result['rc']
    except Exception as exc:  # noqa: BLE001
        result.update(verdict='FAIL', rc=1, error=f'{type(exc).__name__}: {exc}')
        print(f"ERROR: {result['error']}", file=sys.stderr)
        return 1
    finally:
        # A second Ctrl-C must not interrupt the bounded owned-process cleanup.
        for signum in old_handlers:
            signal.signal(signum, signal.SIG_IGN)
        cleanup_errors = []
        stopped = True
        if qmp_socket is not None and proc is not None and proc.poll() is None:
            try:
                result['screenshot']['qmp'] = qmp_screendump(
                    qmp_socket, run_dir / 'qmp-screenshot.png')
            except Exception as exc:  # noqa: BLE001
                result['screenshot']['qmp_error'] = f'{type(exc).__name__}: {exc}'
        if (scenario and scenario.get('requires_visible_frame')
                and result.get('verdict') != 'BLOCKED'):
            from harness.screenshots import nonblack_bbox
            qmp = result['screenshot'].get('qmp')
            if qmp:
                pixels = nonblack_bbox(Path(qmp['path']))
                result['screenshot']['qmp_pixels'] = pixels
                if not pixels or not pixels.get('bbox'):
                    result['verdict'] = 'FAIL'
                    result['rc'] = 1
                    result.setdefault('failures', []).append(
                        'visible frame required: QMP screenshot is blank')
            else:
                result['verdict'] = 'FAIL'
                result['rc'] = 1
                result.setdefault('failures', []).append(
                    'visible frame required: QMP screenshot missing (use --qmp-screenshot)')
        if (failure_bundle and result.get('verdict') == 'FAIL'
                and ser is not None and proc is not None and proc.poll() is None):
            try:
                from harness.failures import collect_failure_bundle
                collect_failure_bundle(ser, run_dir, result)
            except Exception as exc:  # noqa: BLE001
                result['failure_bundle'] = {
                    'error': f'{type(exc).__name__}: {exc}'
                }
        if state is not None and ser is not None and proc is not None and proc.poll() is None:
            try:
                _, _, flush_rc, flush_seconds = execute_guest(ser, 'sync', 10)
                result['state'].update({'flush_rc': flush_rc, 'flush_seconds': flush_seconds})
            except Exception as exc:  # noqa: BLE001
                result['state']['flush_error'] = f'{type(exc).__name__}: {exc}'
        if proc is not None:
            try:
                if proc.poll() is None:
                    proc.kill()
                proc.wait(timeout=10)
            except Exception as exc:
                cleanup_errors.append(f'QEMU cleanup: {exc}')
                stopped = proc.poll() is not None
            result['qemu_returncode'] = proc.returncode
        if ser is not None:
            try:
                ser.close()
            except Exception as exc:
                cleanup_errors.append(f'serial cleanup: {exc}')
        if proc is not None:
            for stream in (proc.stdin, proc.stdout):
                if stream:
                    stream.close()
        for signum, handler in old_handlers.items():
            signal.signal(signum, handler)
        result['overlays'] = overlays
        result['applied_substitutions'] = sorted(set(result['applied_substitutions']) |
            set(storage.get('setup', {}).get('applied_substitutions', [])))
        if state_lock is not None:
            state_lock.close()
        if state is None and not keep_overlay and stopped:
            try:
                cleanup_overlays(run_dir, overlays)
            except Exception as exc:
                cleanup_errors.append(f'overlay cleanup: {exc}')
        result['overlays_retained'] = (state is not None or
            any(Path(item['path']).exists() for item in overlays.values()))
        if state is None and not keep_overlay and stopped and not result['overlays_retained']:
            try:
                cleanup_storage(run_dir, storage)
            except Exception as exc:
                cleanup_errors.append(f'generated storage cleanup: {exc}')
        result['generated_storage_retained'] = any(
            Path(item['backing']).exists() for item in storage['volumes'].values())
        if artifact_serial is not None:
            try:
                artifacts = extract_artifacts(artifact_serial, run_dir)
                result['screenshot']['artifacts'] = artifacts
                result['screenshot']['ok'] = any(
                    item['name'] == 'wayland-screenshot.png' for item in artifacts)
                if not result['screenshot']['ok']:
                    result['screenshot']['error'] = 'no wayland-screenshot.png artifact'
            except Exception as exc:  # noqa: BLE001
                result['screenshot']['error'] = f'artifact extraction: {exc}'
        if qmp_screenshot:
            result['screenshot']['ok'] = bool(
                result['screenshot'].get('qmp') or result['screenshot'].get('ok'))
        if cleanup_errors:
            result.update(verdict='FAIL', rc=1, cleanup_errors=cleanup_errors)
        result['total_seconds'] = round(time.monotonic() - t0, 3)
        (run_dir / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
        print(f"[vm] {result['verdict']} rc={result['rc']} result={run_dir / 'result.json'}", flush=True)
        if cleanup_errors:
            raise OSError('; '.join(cleanup_errors))


def prepare() -> None:
    if not KERNEL.exists() or not ROOTFS.exists():
        raise ValueError('missing inputs: prepare backing-image links explicitly; automatic rebuilding is disabled')
    print(f"kernel: {KERNEL} {KERNEL.stat().st_size}")
    print(f"kernel patched: {KERNEL_PATCHED} "
          f"{'yes' if kernel_is_patched(KERNEL_PATCHED) else 'no'}")
    print(f"rootfs: {ROOTFS} {ROOTFS.stat().st_size}")


def doctor(verify_images: bool = False, tools: Path | None = None,
           storage_mode: str = 'flat', graphics: bool = False) -> int:
    tools = tools or (VM / 'tools.dev.img' if (VM / 'tools.dev.img').is_file() else TOOLS)
    checks: dict[str, object] = {}
    ok = True

    def check(name: str, value: bool, detail: object = None) -> None:
        nonlocal ok
        checks[name] = {"ok": value, "detail": detail}
        ok = ok and value

    def note(name: str, detail: object) -> None:
        checks[name] = {"ok": True, "detail": detail}

    check("kvm_device", os.path.exists("/dev/kvm") and os.access("/dev/kvm", os.R_OK | os.W_OK),
          "/dev/kvm")
    version = qemu_version()
    check("qemu", bool(shutil.which("qemu-system-x86_64")), version)
    check("qemu_version", version_tuple(version) >= MIN_QEMU, {
        "found": version, "minimum": ".".join(map(str, MIN_QEMU))})
    check("qemu_img", bool(shutil.which("qemu-img")), shutil.which("qemu-img"))
    check("debugfs", bool(shutil.which("debugfs")), shutil.which("debugfs"))

    devices = qemu_devices()
    for device in REQUIRED_DEVICES:
        check(f"qemu_device_{device}", device in devices, device)
    displays = qemu_displays()
    check("qemu_display_none", "none" in displays, sorted(displays))
    note("qemu_display_backends", sorted(displays))

    if storage_mode == 'split':
        for command in ('fakeroot', 'mkfs.ext4'):
            check(f'split_storage_{command}', bool(shutil.which(command)), shutil.which(command))
        checks['split_storage_note'] = 'Synthetic USB-backed filesystems; production eMMC layout/settings/labels are not reproduced.'
    for name, path in (("kernel", KERNEL), ("rootfs", ROOTFS),
                       ("update", UPDATE), ("tools", tools)):
        check(name, path.exists(), str(path))
    checks['patched_kernel_optional'] = {'ok': kernel_is_patched(KERNEL_PATCHED),
                                         'detail': 'experimental; not required for bzImage+gdb'}
    try:
        tools_list = subprocess.run(
            ["debugfs", "-R", "ls -l /", str(tools)],
            capture_output=True, text=True, check=False).stdout
        check("tools_contents", "phase4.sh" in tools_list and "scenarios.sh" in tools_list,
              [line.split()[-1] for line in tools_list.splitlines()
               if line.strip() and not line.rstrip().endswith(" .")][:16])
    except Exception as exc:  # noqa: BLE001
        check("tools_contents", False, str(exc))
    try:
        tools_status = check_tools(tools)
        check("tools_manifest", tools_status["ok"], tools_status)
    except Exception as exc:  # noqa: BLE001
        check("tools_manifest", False, str(exc))

    pids = running_vm_pids()
    check("no_running_vm", not pids, {"pids": pids, "name": "vm-lab"})
    usage = shutil.disk_usage(VM)
    check("free_disk_gb", usage.free > 10 * 1024 ** 3,
          round(usage.free / 1024 ** 3, 1))
    note("runs", runs_stats(VM))

    state = compat_state(VM)
    if graphics:
        check("compat_host_kernel", state["host_kernel_ok"], state["host_kernel"])
        check("compat_initramfs", state["initramfs_ok"],
              state["initramfs"] + " (run: sh compat/build-initramfs.sh)")
        check("compat_modules", state["modules_ok"], state["missing_modules"])
        check("compat_spike", state["spike_ok"], state["spike"])
        check("compat_pin", state["pin"]["ok"], state["pin"])
    else:
        note("compat_capability", state)

    for name, path in (('kernel', KERNEL), ('rootfs', ROOTFS), ('tools', tools), ('usb', UPDATE)):
        if path.is_file():
            checks[f'{name}_identity'] = image_identity(path, verify_images)
    checks['identity_note'] = 'Digests identify artifacts; no trusted reference manifest has been supplied.'

    actions: list[str] = []
    if checks.get("no_running_vm", {}).get("ok") is False:
        actions.append("stop the running vm-lab QEMU processes before a managed run")
    if checks.get("tools_manifest", {}).get("ok") is False:
        actions.append("rebuild the tools image: sh build_tools.sh tools.$(date +%Y%m%d).img")
    if graphics:
        if checks.get("compat_initramfs", {}).get("ok") is False:
            actions.append("build the compat initramfs: sh compat/build-initramfs.sh")
        if checks.get("compat_modules", {}).get("ok") is False:
            actions.append("install the host virtio_gpu/bochs modules")
        if checks.get("compat_pin", {}).get("ok") is False:
            actions.append("record the compat kernel pin: sh compat/pin-kernel.sh")
    if actions:
        checks["next_actions"] = actions
    print(json.dumps({"ok": ok, "checks": checks}, indent=2))
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="action", required=True)
    sub.add_parser("prepare")
    d = sub.add_parser("doctor")
    d.add_argument('--verify-images', action='store_true', help='freshly hash large backing images (slow on CIFS)')
    d.add_argument('--tools', type=Path, help='tools image to check (prefers tools.dev.img)')
    d.add_argument('--storage', choices=('flat', 'split'), default='flat')
    d.add_argument('--graphics', action='store_true',
                   help='include Mode C compat-kernel graphics readiness checks')
    sub.add_parser('list-scenarios')
    inp = sub.add_parser('input', help='send a click or key to a running compat VM via QMP')
    inp.add_argument('--qmp', type=Path, required=True, help='QMP socket from the run directory')
    inp.add_argument('--click', metavar='X,Y', help='absolute tablet click in video pixels')
    inp.add_argument('--key', metavar='QCODE', help='QEMU qcode, e.g. ret or a')
    inp.add_argument('--width', type=int, default=1280)
    inp.add_argument('--height', type=int, default=720)
    inp.add_argument('--button', choices=('left', 'middle', 'right'), default='left')
    lst = sub.add_parser('list', help='list runs')
    lst.add_argument('--label', default=None)
    lst.add_argument('--scenario', default=None)
    lst.add_argument('--verdict', default=None)
    lst.add_argument('--limit', type=int, default=20)
    lst.add_argument('--json', action='store_true')
    show = sub.add_parser('show', help='print a run result')
    show.add_argument('run')
    extract = sub.add_parser('extract', help='copy an artifact out of a run')
    extract.add_argument('run')
    extract.add_argument('member')
    extract.add_argument('destination', type=Path)
    graft = sub.add_parser('prune', help='prune old runs (dry-run unless --apply)')
    graft.add_argument('--keep', type=int, default=20)
    graft.add_argument('--apply', action='store_true')
    graft.add_argument('--label', default=None)
    graft.add_argument('--scenario', default=None)
    gc = sub.add_parser('gc', help='prune with a larger default keep set')
    gc.add_argument('--keep', type=int, default=50)
    gc.add_argument('--apply', action='store_true')
    gc.add_argument('--label', default=None)
    gc.add_argument('--scenario', default=None)
    r = sub.add_parser("run")
    r.add_argument("--cmd", default=None)
    r.add_argument('--label', default='', help='searchable run label')
    r.add_argument('--profile', choices=('research', 'conformance'), default='research')
    r.add_argument('--scenario', choices=SCENARIO_NAMES)
    r.add_argument('--storage', choices=('flat', 'split'), default='flat',
                   help='split adds synthetic isolated update/rwdata/log filesystems, not a production disk layout')
    r.add_argument('--storage-size', action='append', default=[], metavar='ROLE=MiB',
                   help='synthetic volume size: update, rwdata or logdata; 16..8192 MiB')
    r.add_argument('--audit', action='store_true', help='collect a read-only guest environment census after the command')
    r.add_argument('--keep-overlay', action='store_true')
    r.add_argument('--graphics', choices=('patched-offscreen', 'mesa112'), default='patched-offscreen',
                   help='managed research application graphics mode; Mesa remains experimental')
    r.add_argument('--verify-images', action='store_true')
    r.add_argument("--interactive", action="store_true")
    r.add_argument("--timeout", type=float, default=300)
    r.add_argument("--usb", type=Path, default=None,
                   help="raw image to expose as a QEMU USB mass-storage stick")
    r.add_argument("--tools", type=Path, default=None,
                   help="second raw image exposed as a USB stick (research tools)")
    r.add_argument('--share', type=Path, default=None,
                   help='compat mode only: host directory mounted at /media/host via 9p')
    r.add_argument('--display', choices=('none', 'gtk', 'vnc'), default='none',
                   help='interactive display backend (gtk needs a desktop session)')
    r.add_argument('--vnc-display', default=':1', metavar=':N',
                   help='VNC display number/address for --display vnc (default :1)')
    r.add_argument('--state', type=Path, default=None,
                   help='persistent overlay directory (reuse across runs)')
    r.add_argument('--reset', action='store_true',
                   help='with --state, discard and recreate the persistent overlays')
    r.add_argument('--network', action='store_true',
                   help='compat research only: user-mode net, restrict=on, hostfwd 2222->22')
    r.add_argument('--allow-stale-tools', action='store_true',
                   help='research only: boot a stale/unmanifested tools image with a warning')
    r.add_argument('--allow-kernel-mismatch', action='store_true',
                   help='research only: boot compat mode despite a kernel-pin mismatch')
    r.add_argument('--screenshot', action='store_true',
                   help='capture wayland-screenshot.png through the second serial artifact channel')
    r.add_argument('--qmp-screenshot', action='store_true',
                   help='attach a display device and capture qmp-screenshot.png via QMP')
    r.add_argument('--display-device', choices=('none', 'std', 'virtio-vga'), default='none',
                   help='QEMU display device for QMP screendump (default: auto std for --qmp-screenshot)')
    r.add_argument('--smp', type=int, choices=(1, 2, 4), default=1,
                   help='guest CPUs (vendor kernel NR_CPUS=4)')
    r.add_argument('--memory', type=int, default=4096, metavar='MiB',
                   help='guest memory in MiB (1024..8192)')
    r.add_argument('--apic', action='store_true',
                   help='omit the noapic kernel argument (experimental)')
    r.add_argument('--no-failure-bundle', action='store_false', dest='failure_bundle',
                   help='skip the post-FAIL guest log bundle')
    r.add_argument("--kernel", choices=("vendor", "compat", "auto", "bzimage", "vmlinux"),
                   default="vendor",
                   help="vendor (exact 4.1.27-abl) or compat (pinned/host kernel + initramfs); legacy auto/bzimage/vmlinux select vendor sub-modes")
    r.add_argument("--payload", type=Path, default=None,
                   help="directory (built to ext4) or image for a third USB stick")
    args = ap.parse_args()
    if args.action == "prepare":
        prepare()
        return 0
    if args.action == "doctor":
        return doctor(args.verify_images, args.tools, args.storage, args.graphics)
    if args.action == 'list-scenarios':
        for name in SCENARIO_NAMES:
            item = load_scenario(name)
            print(f"{name}: {item['description']}")
        return 0
    if args.action == 'input':
        if bool(args.click) == bool(args.key):
            ap.error('input requires exactly one of --click X,Y or --key QCODE')
        try:
            if args.click:
                x, y = (int(part) for part in args.click.split(',', 1))
                result = qmp_click(args.qmp, x, y, args.width, args.height,
                                   args.button)
            else:
                result = qmp_key(args.qmp, args.key)
        except (ValueError, OSError, QmpError) as exc:
            ap.error(str(exc))
        print(json.dumps(result, indent=2))
        return 0
    if args.action == 'list':
        runs = filter_runs(collect_runs(VM), args.label, args.scenario,
                           args.verdict)[:args.limit]
        if args.json:
            print(json.dumps(runs, indent=2))
        else:
            for item in runs:
                label = f" [{item['label']}]" if item['label'] else ''
                print(f"{item['run']} {item['verdict']:8} {item['mode']:6} "
                      f"{item['scenario'] or 'custom':20} {label}")
        return 0
    if args.action in ('show', 'extract'):
        run_path = VM / 'runs' / args.run / 'result.json'
        if args.action == 'show':
            if not run_path.is_file():
                ap.error(f'run result not found: {run_path}')
            print(run_path.read_text())
            return 0
        try:
            print(json.dumps(extract_run(VM, args.run, args.member,
                                         args.destination), indent=2))
        except (ValueError, OSError) as exc:
            ap.error(str(exc))
        return 0
    if args.action in ('prune', 'gc'):
        try:
            print(json.dumps(prune_runs(VM, args.keep, args.apply,
                                        args.label, args.scenario), indent=2))
        except (ValueError, OSError) as exc:
            ap.error(str(exc))
        return 0
    if args.scenario and (args.cmd is not None or args.interactive):
        ap.error('--scenario cannot be combined with --cmd or --interactive')
    if args.kernel == 'compat':
        kernel_target, vendor_mode = 'compat', 'auto'
    elif args.kernel == 'vendor':
        kernel_target, vendor_mode = 'vendor', 'auto'
    else:
        kernel_target, vendor_mode = 'vendor', args.kernel
    return run(args.cmd, args.interactive, args.timeout, args.usb, args.tools,
               vendor_mode, args.payload, args.profile, args.scenario,
               args.keep_overlay, args.verify_images, args.graphics,
               args.storage, parse_sizes(args.storage_size), args.audit,
               args.allow_stale_tools, args.screenshot, args.qmp_screenshot,
               args.display_device, args.smp, args.memory, args.apic,
               args.failure_bundle, kernel_target, args.allow_kernel_mismatch,
               args.share, args.display, args.vnc_display, args.label,
               args.state, args.reset, args.network)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, OSError) as exc:
        sys.exit(f'ERROR: {exc}')
