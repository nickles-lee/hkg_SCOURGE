# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only

"""Host directory share for compat-mode guests (9p, virtio transport)."""
from __future__ import annotations

from pathlib import Path

MOUNT_TAG = "hostshare"
MOUNTPOINT = "/media/host"


def mount_script() -> str:
    return f"""set -eu
mkdir -p {MOUNTPOINT}
mount -t 9p -o trans=virtio,version=9p2000.L {MOUNT_TAG} {MOUNTPOINT}
test -d {MOUNTPOINT}
echo 'HARNESS_CHECK host-share PASS'
"""


def qemu_option(path: Path) -> str:
    path = Path(path).resolve()
    if not path.is_dir():
        raise ValueError(f"share directory missing: {path}")
    if any(char in str(path) for char in ",\r\n\x00"):
        raise ValueError(f"unsupported share path: {path}")
    return f"local,path={path},mount_tag={MOUNT_TAG},security_model=none"
