# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Raw artifact channel over a second QEMU serial port.

The guest writes `HARNESS_ARTIFACT <name> <size>\\n` followed by exactly
`size` bytes to `/dev/ttyS1`. QEMU backs that port with a file in the run
directory, so screenshots and logs cross the VM boundary without mounting an
image, converting a 4 GiB overlay, or base64-inflating the console.

Only regular, single-component names are accepted. Existing files in the run
directory are overwritten only by this module's caller.
"""
from __future__ import annotations

import hashlib
import re
from pathlib import Path

MAGIC = b"HARNESS_ARTIFACT "
NAME_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}\Z")
MAX_ARTIFACT_BYTES = 64 * 1024 * 1024


def guest_screenshot_trap() -> str:
    """Shell prelude for the vendor rootfs: screenshot Weston on command exit."""
    return r'''__vm_artifact() {
  __vm_rc=$?
  if [ -c /dev/ttyS1 ] || [ -e /dev/ttyS1 ]; then
    # Raw bytes: disable NL -> CRNL output post-processing on the artifact tty.
    busybox stty -F /dev/ttyS1 -opost 2>/dev/null || \
      stty -opost < /dev/ttyS1 2>/dev/null || true
    rm -f /tmp/wayland-screenshot.png /tmp/weston-screenshooter.log
    ( cd /tmp && \
      XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run/user/root}" \
      WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
      /usr/libexec/weston-screenshooter ) >/tmp/weston-screenshooter.log 2>&1
    if [ -s /tmp/wayland-screenshot.png ]; then
      __vm_size=$(wc -c < /tmp/wayland-screenshot.png)
      printf 'HARNESS_ARTIFACT wayland-screenshot.png %s\n' "$__vm_size" > /dev/ttyS1
      cat /tmp/wayland-screenshot.png > /dev/ttyS1
      printf 'HARNESS_ARTIFACT_OK wayland-screenshot.png %s\n' "$__vm_size"
    else
      echo 'HARNESS_ARTIFACT_FAIL wayland-screenshot.png'
    fi
    busybox stty -F /dev/ttyS1 opost 2>/dev/null || \
      stty opost < /dev/ttyS1 2>/dev/null || true
  fi
  exit "$__vm_rc"
}
trap __vm_artifact EXIT
'''


def wrap_command(command: str, screenshot: bool = False) -> str:
    if not screenshot:
        return command
    return guest_screenshot_trap() + command


def _read_header(data: bytes, start: int) -> tuple[str, int, int] | None:
    line_end = data.find(b"\n", start)
    if line_end < 0:
        return None
    try:
        fields = data[start + len(MAGIC):line_end].decode("ascii").split()
    except UnicodeDecodeError:
        return None
    if len(fields) != 2:
        return None
    name, size_text = fields
    if not NAME_RE.fullmatch(name):
        return None
    try:
        size = int(size_text)
    except ValueError:
        return None
    if size < 0 or size > MAX_ARTIFACT_BYTES:
        return None
    return name, size, line_end + 1


def extract_artifacts(serial_path: Path, output_dir: Path) -> list[dict]:
    """Parse artifact frames from a captured second-serial file."""
    if not serial_path.is_file():
        return []
    data = serial_path.read_bytes()
    artifacts: list[dict] = []
    position = 0
    while True:
        start = data.find(MAGIC, position)
        if start < 0:
            break
        header = _read_header(data, start)
        if header is None:
            position = start + len(MAGIC)
            continue
        name, size, payload_start = header
        payload_end = payload_start + size
        if payload_end > len(data):
            break
        payload = data[payload_start:payload_end]
        destination = output_dir / name
        destination.write_bytes(payload)
        artifacts.append(
            {
                "name": name,
                "path": str(destination),
                "size": size,
                "sha256": hashlib.sha256(payload).hexdigest(),
            }
        )
        position = payload_end
    return artifacts
