# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""QMP screendump client for graphical QEMU runs.

`launch.py --qmp-screenshot` adds a QMP UNIX socket and a display device
(`-vga std` by default, or `--display-device virtio-vga` for a compat-kernel
guest). After the command finishes, but before QEMU is stopped, the harness
asks QMP for a PNG and records its size/dimensions/hash in `result.json`.
"""
from __future__ import annotations

import hashlib
import json
import socket
import struct
import time
from pathlib import Path


class QmpError(RuntimeError):
    pass


def _read_message(handle, deadline: float) -> dict:
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise QmpError("timed out waiting for QMP response")
        line = handle.readline()
        if not line:
            raise QmpError("QMP connection closed")
        try:
            message = json.loads(line)
        except json.JSONDecodeError as exc:
            raise QmpError(f"invalid QMP JSON: {line[:120]!r}") from exc
        if "event" in message:
            continue
        return message


def _execute(handle, deadline: float, command: str, arguments: dict | None = None) -> dict:
    request = {"execute": command}
    if arguments is not None:
        request["arguments"] = arguments
    handle.write((json.dumps(request) + "\n").encode())
    response = _read_message(handle, deadline)
    if "error" in response:
        error = response["error"]
        raise QmpError(f"{command}: {error.get('desc', error)}")
    return response


def _connect(socket_path: Path, timeout: float):
    socket_path = Path(socket_path)
    deadline = time.monotonic() + timeout
    while not socket_path.exists():
        if time.monotonic() > deadline:
            raise QmpError(f"QMP socket did not appear: {socket_path}")
        time.sleep(0.05)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(max(0.1, deadline - time.monotonic()))
    try:
        sock.connect(str(socket_path))
        handle = sock.makefile("rwb", buffering=0)
        greeting = _read_message(handle, deadline)
        if "QMP" not in greeting:
            handle.close()
            raise QmpError(f"unexpected QMP greeting: {greeting}")
        _execute(handle, deadline, "qmp_capabilities")
        return sock, handle, deadline
    except BaseException:
        sock.close()
        raise


def send_events(socket_path: Path, events: list[dict], timeout: float = 10.0) -> dict:
    """Send QMP input events to a running VM."""
    sock, handle, deadline = _connect(Path(socket_path), timeout)
    try:
        _execute(handle, deadline, "input-send-event", {"events": events})
    finally:
        handle.close()
        sock.close()
    return {"ok": True, "events": len(events)}


def click(socket_path: Path, x: int, y: int, width: int = 1280, height: int = 720,
          button: str = "left", timeout: float = 10.0) -> dict:
    """Absolute tablet click in video-pixel coordinates."""
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be positive")
    if button not in ("left", "middle", "right"):
        raise ValueError(f"invalid button: {button!r}")
    if not (0 <= x < width and 0 <= y < height):
        raise ValueError(f"click outside {width}x{height}: {x},{y}")
    ax = round(x * 32767 / max(1, width - 1))
    ay = round(y * 32767 / max(1, height - 1))
    events = [
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}},
        {"type": "btn", "data": {"down": True, "button": button}},
        {"type": "btn", "data": {"down": False, "button": button}},
    ]
    send_events(socket_path, events, timeout)
    return {"ok": True, "x": x, "y": y, "width": width, "height": height,
            "button": button}


def key(socket_path: Path, qcode: str, timeout: float = 10.0) -> dict:
    if not qcode or any(char.isspace() for char in qcode):
        raise ValueError(f"invalid qcode: {qcode!r}")
    events = [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": qcode}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}},
    ]
    send_events(socket_path, events, timeout)
    return {"ok": True, "qcode": qcode}


def device_del(socket_path: Path, device_id: str, timeout: float = 10.0) -> dict:
    """Detach a QEMU device, e.g. the usbdev update stick."""
    if not device_id or any(char.isspace() for char in device_id):
        raise ValueError(f"invalid device id: {device_id!r}")
    sock, handle, deadline = _connect(Path(socket_path), timeout)
    try:
        _execute(handle, deadline, "device_del", {"id": device_id})
    finally:
        handle.close()
        sock.close()
    return {"ok": True, "id": device_id}


def _png_dimensions(data: bytes) -> tuple[int, int]:
    if len(data) < 24 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise QmpError("screendump did not produce a PNG")
    width, height = struct.unpack(">II", data[16:24])
    if width <= 0 or height <= 0:
        raise QmpError(f"invalid PNG dimensions {width}x{height}")
    return width, height


def screendump(socket_path: Path, output: Path, timeout: float = 15.0) -> dict:
    """Ask QEMU for a PNG screendump; return artifact metadata."""
    socket_path = Path(socket_path)
    output = Path(output)
    output.unlink(missing_ok=True)
    deadline = time.monotonic() + timeout
    while not socket_path.exists():
        if time.monotonic() > deadline:
            raise QmpError(f"QMP socket did not appear: {socket_path}")
        time.sleep(0.05)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(max(0.1, deadline - time.monotonic()))
    try:
        sock.connect(str(socket_path))
        handle = sock.makefile("rwb", buffering=0)
        try:
            greeting = _read_message(handle, deadline)
            if "QMP" not in greeting:
                raise QmpError(f"unexpected QMP greeting: {greeting}")
            _execute(handle, deadline, "qmp_capabilities")
            _execute(handle, deadline, "screendump",
                     {"filename": str(output.resolve()), "format": "png"})
        finally:
            handle.close()
    finally:
        sock.close()

    # QEMU has acknowledged the synchronous screendump; wait briefly for the
    # file to settle, then validate the PNG header.
    while not output.is_file() or output.stat().st_size == 0:
        if time.monotonic() > deadline:
            raise QmpError(f"screendump file missing: {output}")
        time.sleep(0.05)
    data = output.read_bytes()
    width, height = _png_dimensions(data)
    return {
        "path": str(output),
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "width": width,
        "height": height,
    }
